/*
 * pkcs15-crypt.c: Tool for cryptography operations with SmartCards
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <opensc/opensc.h>
#include <opensc/pkcs15.h>
#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#endif
#include "util.h"

const char *app_name = "pkcs15-crypt";

int opt_reader = 0, quiet = 0;
int opt_debug = 0;
char * opt_pincode = NULL, * opt_key_id = NULL;
char * opt_input = NULL, * opt_output = NULL;
int opt_crypt_flags = 0;

#define OPT_SHA1	0x101
#define OPT_MD5		0x102
#define OPT_PKCS1	0x103

const struct option options[] = {
	{ "sign",		0, 0,		's' },
	{ "decipher",		0, 0,		'c' },
	{ "key",		1, 0,		'k' },
	{ "reader",		1, 0,		'r' },
	{ "input",		1, 0,		'i' },
	{ "output",		1, 0,		'o' },
	{ "sha-1",		0, 0,		OPT_SHA1 },
	{ "md5",		0, 0,		OPT_MD5 },
	{ "pkcs1",		0, 0,		OPT_PKCS1 },
	{ "quiet",		0, 0,		'q' },
	{ "debug",		0, 0,		'd' },
	{ "pin",		1, 0,		'p' },
	{ 0, 0, 0, 0 }
};

const char *option_help[] = {
	"Performs digital signature operation",
	"Decipher operation",
	"Selects the private key ID to use",
	"Uses reader number <arg>",
	"Selects the input file to use",
	"Outputs to file <arg>",
	"Input file is a SHA-1 hash",
	"Input file is a MD5 hash",
	"Use PKCS #1 v1.5 padding",
	"Quiet operation",
	"Debug output -- may be supplied several times",
	"Uses password (PIN) <arg>",
};

struct sc_context *ctx = NULL;
struct sc_card *card = NULL;
struct sc_pkcs15_card *p15card = NULL;

char * get_pin(struct sc_pkcs15_object *obj)
{
	char buf[80];
	char *pincode;
	struct sc_pkcs15_pin_info *pinfo = (struct sc_pkcs15_pin_info *) obj->data;
	
	if (opt_pincode != NULL)
		return strdup(opt_pincode);
	sprintf(buf, "Enter PIN [%s]: ", obj->label);
	while (1) {
		pincode = getpass(buf);
		if (strlen(pincode) == 0)
			return NULL;
		if (strlen(pincode) < pinfo->min_length ||
		    strlen(pincode) > pinfo->stored_length)
		    	continue;
		return pincode;
	}
}

int read_input(u8 *buf, int buflen)
{
	FILE *inf;
	int c;
	
	inf = fopen(opt_input, "r");
	if (inf == NULL) {
		fprintf(stderr, "Unable to open '%s' for reading.\n", opt_input);
		return -1;
	}
	c = fread(buf, 1, buflen, inf);
	fclose(inf);
	if (c < 0) {
		perror("read");
		return -1;
	}
	return c;
}

int write_output(const u8 *buf, int len)
{
	FILE *outf;
	int output_binary = 1;
	
	if (opt_output != NULL) {
		outf = fopen(opt_output, "wb");
		if (outf == NULL) {
			fprintf(stderr, "Unable to open '%s' for writing.\n", opt_output);
			return -1;
		}
	} else {
		outf = stdout;
		output_binary = 0;
	}
	if (output_binary == 0)
		print_binary(outf, buf, len);
	else
		fwrite(buf, len, 1, outf);
	if (outf != stdout)
		fclose(outf);
	return 0;
}

#ifdef HAVE_OPENSSL
#define GETBN(bn)	((bn)->len? BN_bin2bn((bn)->data, (bn)->len, NULL) : NULL)
int extract_key(struct sc_pkcs15_object *obj, EVP_PKEY **pk)
{
	struct sc_pkcs15_prkey	*key;
	const char	*pass = NULL;
	int		r;

	while (1) {
		r = sc_pkcs15_read_prkey(p15card, obj, pass, &key);
		if (r != SC_ERROR_PASSPHRASE_REQUIRED)
			break;

		if (pass)
			return SC_ERROR_INTERNAL;
		pass = "lalla"; continue;
		pass = getpass("Please enter pass phrase "
				"to unlock secret key: ");
		if (!pass || !*pass)
			break;
	}

	if (r < 0)
		return r;

	*pk = EVP_PKEY_new();
	switch (key->algorithm) {
	case SC_ALGORITHM_RSA:
		{
		RSA	*rsa = RSA_new();

		EVP_PKEY_set1_RSA(*pk, rsa);
		rsa->n = GETBN(&key->u.rsa.modulus);
		rsa->e = GETBN(&key->u.rsa.exponent);
		rsa->d = GETBN(&key->u.rsa.d);
		rsa->p = GETBN(&key->u.rsa.p);
		rsa->q = GETBN(&key->u.rsa.q);
		break;
		}
	case SC_ALGORITHM_DSA:
		{
		DSA	*dsa = DSA_new();

		EVP_PKEY_set1_DSA(*pk, dsa);
		dsa->priv_key = GETBN(&key->u.dsa.priv);
		break;
		}
	default:
		r = SC_ERROR_NOT_SUPPORTED;
	}

	/* DSA keys need additional parameters from public key file */
	if (obj->type == SC_PKCS15_TYPE_PRKEY_DSA) {
		struct sc_pkcs15_id     *id;
		struct sc_pkcs15_object *pub_obj;
		struct sc_pkcs15_pubkey *pub;
		DSA			*dsa;

		id = &((struct sc_pkcs15_prkey_info *) obj->data)->id;
		r = sc_pkcs15_find_pubkey_by_id(p15card, id, &pub_obj);
		if (r < 0)
			goto done;
		r = sc_pkcs15_read_pubkey(p15card, pub_obj, &pub);
		if (r < 0)
			goto done;

		dsa = (*pk)->pkey.dsa;
		dsa->pub_key = GETBN(&pub->u.dsa.pub);
		dsa->p = GETBN(&pub->u.dsa.p);
		dsa->q = GETBN(&pub->u.dsa.q);
		dsa->g = GETBN(&pub->u.dsa.g);
		sc_pkcs15_free_pubkey(pub);
	}

done:	if (r < 0)
		EVP_PKEY_free(*pk);
	sc_pkcs15_free_prkey(key);
	return r;
}

int sign_ext(struct sc_pkcs15_object *obj,
		u8 *data, size_t len, u8 *out, size_t out_len)
{
	EVP_PKEY *pkey = NULL;
	int	r, nid = -1;

	r = extract_key(obj, &pkey);
	if (r < 0)
		return r;

	switch (obj->type) {
	case SC_PKCS15_TYPE_PRKEY_RSA:
		if (opt_crypt_flags & SC_ALGORITHM_RSA_HASH_MD5) {
			nid = NID_md5;
		} else if (opt_crypt_flags & SC_ALGORITHM_RSA_HASH_SHA1) {
			nid = NID_sha1;
		} else {
			if (len == 16)
				nid = NID_md5;
			else if (len == 20)
				nid = NID_sha1;
			else {
				fprintf(stderr,
					"Invalid input size (%u bytes)\n",
					len);
				return SC_ERROR_INVALID_ARGUMENTS;
			}
		}
		r = RSA_sign(nid, data, len, out, (unsigned int *) &out_len,
				pkey->pkey.rsa);
		if (r <= 0)
			r = SC_ERROR_INTERNAL;
		break;
	case SC_PKCS15_TYPE_PRKEY_DSA:
		r = DSA_sign(NID_sha1, data, len, out, (unsigned int *) &out_len,
				pkey->pkey.dsa);
		if (r <= 0)
			r = SC_ERROR_INTERNAL;
		break;
	}
	if (r >= 0)
		r = out_len;
	EVP_PKEY_free(pkey);
	return r;
}
#endif

int sign(struct sc_pkcs15_object *obj)
{
	u8 buf[1024], out[1024];
	struct sc_pkcs15_prkey_info *key = (struct sc_pkcs15_prkey_info *) obj->data;
	int r, c, len;
	
	if (opt_input == NULL) {
		fprintf(stderr, "No input file specified.\n");
		return 2;
	}
	if (opt_output == NULL) {
		fprintf(stderr, "No output file specified.\n");
		return 2;
	}
	c = read_input(buf, sizeof(buf));
	if (c < 0)
		return 2;
	len = sizeof(out);
	if (obj->type == SC_PKCS15_TYPE_PRKEY_RSA
	 && !(opt_crypt_flags & SC_ALGORITHM_RSA_PAD_PKCS1)
	 && c != key->modulus_length/8) {
		fprintf(stderr, "Input has to be exactly %d bytes, when using no padding.\n",
			key->modulus_length/8);
		return 2;
	}
	if (!key->native) {
#ifdef HAVE_OPENSSL
		r = sign_ext(obj, buf, c, out, len);
#else
		fprintf(stderr, "Cannot use extractable key because this "
				"program was compiled without crypto "
				"support.\n");
		r = SC_ERROR_NOT_SUPPORTED;
#endif
	} else {
		r = sc_pkcs15_compute_signature(p15card, obj, opt_crypt_flags,
					buf, c, out, len);
	}
	if (r < 0) {
		fprintf(stderr, "Compute signature failed: %s\n", sc_strerror(r));
		return 1;
	}
	r = write_output(out, r);
	
	return 0;
}

#ifdef HAVE_OPENSSL
static int decipher_ext(struct sc_pkcs15_object *obj,
		u8 *data, size_t len, u8 *out, size_t out_len)
{
	EVP_PKEY *pkey = NULL;
	int	r;

	r = extract_key(obj, &pkey);
	if (r < 0)
		return r;

	switch (obj->type) {
	case SC_PKCS15_TYPE_PRKEY_RSA:
		r = EVP_PKEY_decrypt(out, data, len, pkey);
		if (r <= 0) {
			fprintf(stderr, "Decryption failed.\n");
			r = SC_ERROR_INTERNAL;
		}
		break;
	default:
		fprintf(stderr, "Key type not supported.\n");
		r = SC_ERROR_NOT_SUPPORTED;
	}
	return r;
}
#endif

int decipher(struct sc_pkcs15_object *obj)
{
	u8 buf[1024], out[1024];
	int r, c, len;
	
	if (opt_input == NULL) {
		fprintf(stderr, "No input file specified.\n");
		return 2;
	}
	c = read_input(buf, sizeof(buf));
	if (c < 0)
		return 2;

	len = sizeof(out);
	if (!((struct sc_pkcs15_prkey_info *) obj->data)->native) {
#ifdef HAVE_OPENSSL
		r = decipher_ext(obj, buf, c, out, len);
#else
		fprintf(stderr, "Cannot use extractable key because this "
				"program was compiled without crypto "
				"support.\n");
		r = SC_ERROR_NOT_SUPPORTED;
#endif
	} else {
		r = sc_pkcs15_decipher(p15card, obj,
			opt_crypt_flags & SC_ALGORITHM_RSA_PAD_PKCS1,
			buf, c, out, len);
	}

	if (r < 0) {
		fprintf(stderr, "Decrypt failed: %s\n", sc_strerror(r));
		return 1;
	}
	r = write_output(out, r);
	
	return 0;
}

int main(int argc, char * const argv[])
{
	int err = 0, r, c, long_optind = 0;
	int do_decipher = 0;
	int do_sign = 0;
	int action_count = 0;
        struct sc_pkcs15_object *key, *pin, *objs[32];
	struct sc_pkcs15_id id;
	char *pincode;
		
	while (1) {
		c = getopt_long(argc, argv, "sck:r:i:o:qp:d", options, &long_optind);
		if (c == -1)
			break;
		if (c == '?')
			print_usage_and_die();
		switch (c) {
		case 's':
			do_sign++;
			action_count++;
			break;
		case 'c':
			do_decipher++;
			action_count++;
			break;
		case 'k':
			opt_key_id = optarg;
			action_count++;
			break;
		case 'r':
			opt_reader = atoi(optarg);
			break;
		case 'i':
			opt_input = optarg;
			break;
		case 'o':
			opt_output = optarg;
			break;
		case OPT_SHA1:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA1;
			break;
		case OPT_MD5:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_MD5;
			break;
		case OPT_PKCS1:
			opt_crypt_flags |= SC_ALGORITHM_RSA_PAD_PKCS1;
			break;
		case 'q':
			quiet++;
			break;
		case 'd':
			opt_debug++;
			break;
		case 'p':
			opt_pincode = optarg;
			break;
		}
	}
	if (action_count == 0)
		print_usage_and_die();
	r = sc_establish_context(&ctx, app_name);
	if (r) {
		fprintf(stderr, "Failed to establish context: %s\n", sc_strerror(r));
		return 1;
	}
	if (opt_debug)
		ctx->debug = opt_debug;
	if (opt_reader >= ctx->reader_count || opt_reader < 0) {
		fprintf(stderr, "Illegal reader number. Only %d reader(s) configured.\n", ctx->reader_count);
		err = 1;
		goto end;
	}
	if (sc_detect_card_presence(ctx->reader[opt_reader], 0) != 1) {
		fprintf(stderr, "Card not present.\n");
		return 3;
	}
	if (!quiet)
		fprintf(stderr, "Connecting to card in reader %s...\n", ctx->reader[opt_reader]->name);
	r = sc_connect_card(ctx->reader[opt_reader], 0, &card);
	if (r) {
		fprintf(stderr, "Failed to connect to card: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}

#if 1
	r = sc_lock(card);
	if (r) {
		fprintf(stderr, "Unable to lock card: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}
#endif

	if (!quiet)
		fprintf(stderr, "Trying to find a PKCS #15 compatible card...\n");
	r = sc_pkcs15_bind(card, &p15card);
	if (r) {
		fprintf(stderr, "PKCS #15 initialization failed: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}
	if (!quiet)
		fprintf(stderr, "Found %s!\n", p15card->label);

	r = sc_pkcs15_get_objects(p15card, SC_PKCS15_TYPE_PRKEY, objs, 32);
	if (r <= 0) {
		if (r == 0)
			r = SC_ERROR_OBJECT_NOT_FOUND;
		fprintf(stderr, "Private key enumeration failed: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}
	if (opt_key_id != NULL) {
		sc_pkcs15_hex_string_to_id(opt_key_id, &id);
		r = sc_pkcs15_find_prkey_by_id(p15card, &id, &key);
		if (r < 0) {
			fprintf(stderr, "Unable to find private key '%s': %s\n",
				opt_key_id, sc_strerror(r));
			err = 2;
			goto end;
		}
	} else
		key = objs[0];
	if (key->auth_id.len) {
		r = sc_pkcs15_find_pin_by_auth_id(p15card, &key->auth_id, &pin);
		if (r) {
			fprintf(stderr, "Unable to find PIN code for private key: %s\n",
				sc_strerror(r));
			err = 1;
			goto end;
		}
		pincode = get_pin(pin);
		if (pincode == NULL) {
			err = 5;
			goto end;
		}
		r = sc_pkcs15_verify_pin(p15card, (struct sc_pkcs15_pin_info *) pin->data, (const u8 *) pincode, strlen(pincode));
		if (r) {
			fprintf(stderr, "PIN code verification failed: %s\n", sc_strerror(r));
			err = 5;
			goto end;
		}
		free(pincode);
		if (!quiet)
			fprintf(stderr, "PIN code correct.\n");
	}
	if (do_decipher) {
		if ((err = decipher(key)))
			goto end;
		action_count--;
	}
	if (do_sign) {
		if ((err = sign(key)))
			goto end;
		action_count--;
	}
end:
	if (p15card)
		sc_pkcs15_unbind(p15card);
	if (card) {
#if 1
		sc_unlock(card);
#endif
		sc_disconnect_card(card, 0);
	}
	if (ctx)
		sc_release_context(ctx);
	return err;
}
