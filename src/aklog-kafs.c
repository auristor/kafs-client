/* aklog.c: description
 *
 * Copyright (C) 2017 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * Based on code:
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 * Copyright (C) 2008 Chaskiel Grundman. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Kerberos-5 strong enctype support for rxkad:
 *	https://tools.ietf.org/html/draft-kaduk-afs3-rxkad-k5-kdf-00
 *
 * Invoke as: aklog-k5 <cell> [<realm>]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <keyutils.h>
#include <krb5/krb5.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/des.h>
#include <openssl/md5.h>
#include <openssl/err.h>

struct rxrpc_key_sec2_v1 {
        uint32_t        kver;                   /* key payload interface version */
        uint16_t        security_index;         /* RxRPC header security index */
        uint16_t        ticket_length;          /* length of ticket[] */
        uint32_t        expiry;                 /* time at which expires */
        uint32_t        kvno;                   /* key version number */
        uint8_t         session_key[8];         /* DES session key */
        uint8_t         ticket[0];              /* the encrypted ticket */
};

#define RXKAD_TKT_TYPE_KERBEROS_V5              256
#define OSERROR(X, Y) do { if ((long)(X) == -1) { perror(Y); exit(1); } } while(0)
#define OSZERROR(X, Y) do { if ((long)(X) == 0) { perror(Y); exit(1); } } while(0)
#define KRBERROR(X, Y) do { if ((X) != 0) { const char *msg = krb5_get_error_message(k5_ctx, (X)); fprintf(stderr, "%s: %s\n", (Y), msg); krb5_free_error_message(k5_ctx, msg); exit(1); } } while(0)

/*
 * Report an error from the crypto lib.
 */
static void crypto_error(const char *msg)
{
	const char *file;
	char buf[120];
	int e, line;

	if (ERR_peek_error() == 0)
		return;
	fprintf(stderr, "aklog: %s:\n", msg);

	while ((e = ERR_get_error_line(&file, &line))) {
		ERR_error_string(e, buf);
		fprintf(stderr, "- SSL %s: %s:%d\n", buf, file, line);
	}

	exit(1);
}

/*
 * Strip Triple-DES parity bits from a block.
 *
 * Discard the parity bits and converts an 8-octet block to a 7-octet block.
 *
 * See [afs3-rxkad-k5-kdf-00 §4.2] and [RFC3961 §6.3.1].
 *
 * [These docs number the bits weirdly.  Bit '8' appears to be the LSB of the
 * first octet, and 1 the MSB].
 */
static void des3_strip_parity_bits(void *random, const void *key)
{
	const unsigned char *k = key;
	unsigned char *r = random, lsbs;
	int i;

	lsbs = k[7] >> 1;
	for (i = 0; i < 7; i++) {
		r[i] = (k[i] & 0xfe) | (lsbs & 0x1);
		lsbs >>= 1;
	}
}

/*
 * Reverse the Triple-DES random-to-key operation, converting three 64-bit DES
 * keys to 56-bit random strings and concatenate to give a 168-bit random
 * string that can then be fed to the KDF.
 */
static unsigned int des3_key_to_random(void *random, const void *key, unsigned int len)
{
	unsigned int new_len = 0;

	while (len > 8) {
		des3_strip_parity_bits(random, key);
		key += 8;
		random += 7;
		len -= 8;
		new_len += 7;
	}

	return new_len;
}

/*
 * The data to pass into the key derivation function.
 */
struct kdf_data {
	unsigned char i_2;
	unsigned char Label[6];
	unsigned char L_2[4];
} __attribute__((packed));

static const struct kdf_data rxkad_kdf_data = {
	.Label	= "rxkad",		/* Including NUL separator */
	.L_2	= { 0, 0, 0, 64 },	/* BE integer */
};

/*
 * Derive a 64-bit key we can pass to rxkad from the ticket data.  The ticket
 * data is used as the key for the HMAC-MD5 algorithm, which is used as the
 * PRF.  We then iterate over a series of constructed source strings, passing
 * each one through the PRF until we get an MD5 output that we can cut down and
 * use as a substitute for the DES session key that isn't too weak.
 *
 * [afs3-rxkad-k5-kdf-00 §4.3]
 */
static void key_derivation_function(krb5_creds *creds, uint8_t *session_key)
{
	unsigned int i, len;
	union {
		unsigned char md5[MD5_DIGEST_LENGTH];
		DES_cblock des;
	} buf;
	const EVP_MD *algo = EVP_md5(); /* We use HMAC-MD5 */

	struct kdf_data kdf_data = rxkad_kdf_data;

	for (i = 1; i <= 255; i++) {
		/* K(i) = PRF(Ks, [i]_2 || Label || 0x00 || [L]_2) */
		kdf_data.i_2 = i;
		len = sizeof(buf.md5);
		if (!HMAC(algo,
			  creds->keyblock.contents, creds->keyblock.length,
			  (unsigned char *)&kdf_data, sizeof(kdf_data),
			  buf.md5, &len))
			    crypto_error("HMAC");

		if (len < sizeof(buf.des)) {
			fprintf(stderr, "aklog: HMAC returned short result\n");
			exit(1);
		}

		/* Overlay the DES parity. */
		DES_set_odd_parity(&buf.des);
		if (!DES_is_weak_key(&buf.des))
			goto success;
	}

	fprintf(stderr, "aklog: Unable to derive strong DES key\n");
	exit(1);

success:
	memcpy(session_key, buf.des, sizeof(buf.des));
}

/*
 * Extract or derive the session key.
 */
static void derive_key(krb5_creds *creds, uint8_t *session_key)
{
	unsigned int length = creds->keyblock.length;

	switch (creds->keyblock.enctype) {
	case ENCTYPE_NULL:		goto not_supported;
	case ENCTYPE_DES_CBC_CRC:	goto just_copy;
	case ENCTYPE_DES_CBC_MD4:	goto just_copy;
	case ENCTYPE_DES_CBC_MD5:	goto just_copy;
	case ENCTYPE_DES_CBC_RAW:	goto deprecated;
	case ENCTYPE_DES3_CBC_SHA:	goto des3_discard_parity; /* des3-cbc-md5 */
	case ENCTYPE_DES3_CBC_RAW:	goto deprecated;
	case 7:				goto des3_discard_parity; /* des3-cbc-sha1 */
	case ENCTYPE_DES_HMAC_SHA1:	goto deprecated;
	case ENCTYPE_DSA_SHA1_CMS:	goto not_supported;
	case ENCTYPE_MD5_RSA_CMS:	goto not_supported;
	case ENCTYPE_SHA1_RSA_CMS:	goto not_supported;
	case ENCTYPE_RC2_CBC_ENV:	goto not_supported;
	case ENCTYPE_RSA_ENV:		goto not_supported;
	case ENCTYPE_RSA_ES_OAEP_ENV:	goto not_supported;
	case ENCTYPE_DES3_CBC_ENV:	goto not_supported;
	case ENCTYPE_DES3_CBC_SHA1:	goto des3_discard_parity; /* des3-cbc-sha1-kd */
	default:
		if (length < 7)
			goto key_too_short;
		if (creds->keyblock.enctype < 0)
			goto not_supported;
		goto derive_key;
	}

	/* Strip the parity bits for 3DES then do KDF [afs3-rxkad-k5-kdf-00 §4.2]. */
des3_discard_parity:
	if (length & 7) {
		fprintf(stderr, "aklog: 3DES session key not multiple of 8 octets.\n");
		exit(1);
	}
	creds->keyblock.length = des3_key_to_random(creds->keyblock.contents,
						    creds->keyblock.contents,
						    length);
	goto derive_key;

	/* Do KDF [afs3-rxkad-k5-kdf-00 §4.3]. */
derive_key:
	key_derivation_function(creds, session_key);
	return;

	/* Use as-is for single-DES [afs3-rxkad-k5-kdf-00 §4.1]. */
just_copy:
	if (length != 8) {
		fprintf(stderr, "aklog: DES session key not 8 octets.\n");
		exit(1);
	}

	memcpy(session_key, creds->keyblock.contents, length);
	return;

deprecated:
	fprintf(stderr, "aklog: Ticket contains deprecated enc type (%d)\n",
		creds->keyblock.enctype);
	exit(1);

not_supported:
	fprintf(stderr, "aklog: Ticket contains unsupported enc type (%d)\n",
		creds->keyblock.enctype);
	exit(1);
key_too_short:
	fprintf(stderr, "aklog: Ticket contains short key block (%u)\n", length);
	exit(1);
}

int main(int argc, char **argv)
{
	char *cell, *realm, *princ, *desc, *p;
	int ret;
	size_t plen;
	struct rxrpc_key_sec2_v1 *payload;
	krb5_error_code kresult;
	krb5_context k5_ctx;
	krb5_ccache cc;
	krb5_creds search_cred, *creds;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: aklog <cell> [<realm>]\n");
		exit(1);
	}

	cell = argv[1];
	if (argc == 3) {
		realm = strdup(argv[3]);
		OSZERROR(realm, "strdup");
	} else {
		realm = strdup(cell);
		OSZERROR(realm, "strdup");
		for (p = realm; *p; p++)
			*p = toupper(*p);
	}

	for (p = cell; *p; p++)
		*p = tolower(*p);

	ret = asprintf(&princ, "afs/%s@%s", cell, realm);
	OSERROR(ret, "asprintf");
	ret = asprintf(&desc, "afs@%s", cell);
	OSERROR(ret, "asprintf");

	printf("CELL %s\n", cell);
	printf("PRINC %s\n", princ);

	kresult = krb5_init_context(&k5_ctx);
	if (kresult) { fprintf(stderr, "krb5_init_context failed\n"); exit(1); }

	kresult = krb5_cc_default(k5_ctx, &cc);
	KRBERROR(kresult, "Getting credential cache");

	memset(&search_cred, 0, sizeof(krb5_creds));

	kresult = krb5_cc_get_principal(k5_ctx, cc, &search_cred.client);
	KRBERROR(kresult, "Getting client principal");

	kresult = krb5_parse_name(k5_ctx, princ, &search_cred.server);
	KRBERROR(kresult, "Parsing server principal name");

	//search_cred.keyblock.enctype = ENCTYPE_DES_CBC_CRC;

	kresult = krb5_get_credentials(k5_ctx, 0, cc, &search_cred, &creds);
	KRBERROR(kresult, "Getting tickets");

	plen = sizeof(*payload) + creds->ticket.length;
	payload = calloc(1, plen + 4);
	if (!payload) {
		perror("calloc");
		exit(1);
	}

	printf("plen=%zu tklen=%u rk=%zu\n",
	       plen, creds->ticket.length, sizeof(*payload));

	/* use version 1 of the key data interface */
	payload->kver           = 1;
	payload->security_index = 2;
	payload->ticket_length  = creds->ticket.length;
	payload->expiry         = creds->times.endtime;
	payload->kvno           = RXKAD_TKT_TYPE_KERBEROS_V5;

	derive_key(creds, payload->session_key);
	memcpy(payload->ticket, creds->ticket.data, creds->ticket.length);

	ret = add_key("rxrpc", desc, payload, plen, KEY_SPEC_SESSION_KEYRING);
	OSERROR(ret, "add_key");

	krb5_free_creds(k5_ctx, creds);
	krb5_free_cred_contents(k5_ctx, &search_cred);
	krb5_cc_close(k5_ctx, cc);
	krb5_free_context(k5_ctx);
	exit(0);
}
