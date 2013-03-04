/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *      
 * This program includes partly-modified public domain source
 * code from the LZMA SDK: http://www.7-zip.org/sdk.html
 */

#ifndef	_CRYPTO_UTILS_H
#define	_CRYPTO_UTILS_H

#include <arpa/nameser_compat.h>
#include <sys/types.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	MAX_PW_LEN	16
#define	CKSUM_MASK		0x700
#define	CKSUM_MAX_BYTES		64
#define	DEFAULT_CKSUM		"BLAKE256"

/*
 * Default key length for Encryption and Decryption
 */
#ifndef	DEFAULT_KEYLEN
#define	DEFAULT_KEYLEN	32
#define	MAX_KEYLEN 32
#else
#define	MAX_KEYLEN DEFAULT_KEYLEN
#endif

#define	OLD_KEYLEN		16
#define	ENCRYPT_FLAG		1
#define	DECRYPT_FLAG		0
#define	CRYPTO_ALG_AES		0x10
#define	CRYPTO_ALG_SALSA20	0x20
#define	MAX_SALTLEN		64
#define	MAX_NONCE		32

#define	KECCAK_MAX_SEG	(2305843009213693950ULL)
/*
 * Public checksum properties. CKSUM_MAX_BYTES must be updated if a
 * newer larger checksum is added to the list.
 */
typedef enum {
	CKSUM_CRC64 = 0x100,
	CKSUM_BLAKE256 = 0x200,
	CKSUM_BLAKE512 = 0x300,
	CKSUM_SHA256 = 0x400,
	CKSUM_SHA512 = 0x500,
	CKSUM_KECCAK256 = 0x600,
	CKSUM_KECCAK512 = 0x700,
/*
 * Backwards compatibility options. SKEIN in release 1.2 was replaced with
 * Blake2 from 1.3 onwards (for sheer speed of Blake2). We want to be able
 * to decode archives created with 1.2. New archives do not use SKEIN.
 */
	CKSUM_SKEIN256 = 0x800,
	CKSUM_SKEIN512 = 0x900,
	CKSUM_INVALID = 0
} cksum_t;

typedef struct {
	void *crypto_ctx;
	int crypto_alg;
	int enc_dec;
	uchar_t *salt;
	uchar_t *pkey;
	int saltlen;
	int keylen;
} crypto_ctx_t;

typedef struct {
	void *mac_ctx;
	void *mac_ctx_reinit;
	int mac_cksum;
} mac_ctx_t;

/*
 * Generic message digest functions.
 */
int compute_checksum(uchar_t *cksum_buf, int cksum, uchar_t *buf, uint64_t bytes, int mt);
void list_checksums(FILE *strm, char *pad);
int get_checksum_props(const char *name, int *cksum, int *cksum_bytes,
		      int *mac_bytes, int accept_compatible);
void serialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);
void deserialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);

/*
 * Encryption related functions.
 */
int init_crypto(crypto_ctx_t *cctx, uchar_t *pwd, int pwd_len, int crypto_alg,
	       uchar_t *salt, int saltlen, int keylen, uchar_t *nonce, int enc_dec);
int crypto_buf(crypto_ctx_t *cctx, uchar_t *from, uchar_t *to, uint64_t bytes, uint64_t id);
uchar_t *crypto_nonce(crypto_ctx_t *cctx);
void crypto_clean_pkey(crypto_ctx_t *cctx);
void cleanup_crypto(crypto_ctx_t *cctx);
int get_pw_string(uchar_t pw[MAX_PW_LEN], const char *prompt, int twice);
int get_crypto_alg(char *name);
int geturandom_bytes(uchar_t *rbytes, int nbytes);

/*
 * HMAC functions.
 */
int hmac_init(mac_ctx_t *mctx, int cksum, crypto_ctx_t *cctx);
int hmac_reinit(mac_ctx_t *mctx);
int hmac_update(mac_ctx_t *mctx, uchar_t *data, uint64_t len);
int hmac_final(mac_ctx_t *mctx, uchar_t *hash, unsigned int *len);
int hmac_cleanup(mac_ctx_t *mctx);

#ifdef	__cplusplus
}
#endif

#endif	
