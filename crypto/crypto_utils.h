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
#define	DEFAULT_CKSUM		"SKEIN256"

#define ENCRYPT_FLAG	1
#define DECRYPT_FLAG	0
#define	CRYPTO_ALG_AES	0x10
#define	MAX_SALTLEN	64

/*
 * Public checksum properties. CKSUM_MAX_BYTES must be updated if a
 * newer larger checksum is added to the list.
 */
typedef enum {
	CKSUM_CRC64 = 0x100,
	CKSUM_SKEIN256 = 0x200,
	CKSUM_SKEIN512 = 0x300,
	CKSUM_SHA256 = 0x400,
	CKSUM_SHA512 = 0x500
} cksum_t;

typedef struct {
	void *crypto_ctx;
	int crypto_alg;
	int enc_dec;
	uchar_t *salt;
	int saltlen;
} crypto_ctx_t;

typedef struct {
	void *mac_ctx;
	void *mac_ctx_reinit;
	int mac_cksum;
} mac_ctx_t;

/*
 * Generic message digest functions.
 */
int compute_checksum(uchar_t *cksum_buf, int cksum, uchar_t *buf, ssize_t bytes);
int get_checksum_props(char *name, int *cksum, int *cksum_bytes, int *mac_bytes);
void serialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);
void deserialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);

/*
 * Encryption related functions.
 */
int init_crypto(crypto_ctx_t *cctx, uchar_t *pwd, int pwd_len, int crypto_alg,
	       uchar_t *salt, int saltlen, uint64_t nonce, int enc_dec);
int crypto_buf(crypto_ctx_t *cctx, uchar_t *from, uchar_t *to, ssize_t bytes, uint64_t id);
uint64_t crypto_nonce(crypto_ctx_t *cctx);
void crypto_clean_pkey(crypto_ctx_t *cctx);
void cleanup_crypto(crypto_ctx_t *cctx);
int get_pw_string(char pw[MAX_PW_LEN], char *prompt);

/*
 * HMAC functions.
 */
int hmac_init(mac_ctx_t *mctx, int cksum, crypto_ctx_t *cctx);
int hmac_reinit(mac_ctx_t *mctx);
int hmac_update(mac_ctx_t *mctx, uchar_t *data, size_t len);
int hmac_cleanup(mac_ctx_t *mctx);

#ifdef	__cplusplus
}
#endif

#endif	