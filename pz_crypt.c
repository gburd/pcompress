/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2026 Greg Burd. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/*
 * .pz format version 12 encryption: the file key, and the AEAD framing
 * every stream of the file shares (see pz_crypt.h and DESIGN.md).
 */

#include <string.h>
#include <utils.h>
#include <crypto_utils.h>
#include "pz_crypt.h"
#include "crypto/monocypher/monocypher.h"

static void
put_be32(uchar_t *p, uint32_t v)
{
	p[0] = (uchar_t)(v >> 24); p[1] = (uchar_t)(v >> 16);
	p[2] = (uchar_t)(v >> 8); p[3] = (uchar_t)v;
}

static uint32_t
get_be32(const uchar_t *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

static void
put_be64(uchar_t *p, uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}

static uint64_t
get_be64(const uchar_t *p)
{
	return ((uint64_t)get_be32(p) << 32 | get_be32(p + 4));
}

int
pz_aead_alg(int crypto_flag)
{
	if (crypto_flag == CRYPTO_ALG_AES)
		return (AEAD_AES256GCM);
	if (crypto_flag == CRYPTO_ALG_SALSA20)
		return (AEAD_XCHACHA20);
	return (0);
}

/* Derive the file key from pw with the KDF fields of hdr (already checked). */
static int
pz_derive(pz_aead_t *a, int crypto_flag, const uchar_t *pw, int pwlen,
    const uchar_t hdr[PZ_HDR_CRYPTO])
{
	argon2_params_t kp;

	pz_key_wipe(a);
	if ((a->alg = pz_aead_alg(crypto_flag)) == 0 || pwlen < 0)
		return (-1);
	kp.m_kib = get_be32(hdr + PZ_SALT_LEN + 4);
	kp.t = get_be32(hdr + PZ_SALT_LEN + 8);
	kp.p = get_be32(hdr + PZ_SALT_LEN + 12);
	if (get_be32(hdr + PZ_SALT_LEN) != PZ_KDF_ARGON2ID || !argon2_params_ok(&kp)) {
		pz_key_wipe(a);
		return (-2);
	}
	if (argon2id_derive(pw, (size_t)pwlen, hdr, PZ_SALT_LEN, &kp, a->key,
	    AEAD_KEYLEN) != 0) {
		pz_key_wipe(a);
		return (-1);
	}
	return (0);
}

int
pz_key_new(pz_aead_t *a, int crypto_flag, const uchar_t *pw, int pwlen,
    uchar_t hdr[PZ_HDR_CRYPTO])
{
	/* No fallback to clock or pid: a guessable salt is a repeated key. */
	if (geturandom_bytes(hdr, PZ_SALT_LEN) != 0)
		return (-1);
	put_be32(hdr + PZ_SALT_LEN, PZ_KDF_ARGON2ID);
	put_be32(hdr + PZ_SALT_LEN + 4, ARGON2_PZ_M);
	put_be32(hdr + PZ_SALT_LEN + 8, ARGON2_PZ_T);
	put_be32(hdr + PZ_SALT_LEN + 12, ARGON2_PZ_P);
	return (pz_derive(a, crypto_flag, pw, pwlen, hdr));
}

int
pz_key_open(pz_aead_t *a, int crypto_flag, const uchar_t *pw, int pwlen,
    const uchar_t hdr[PZ_HDR_CRYPTO])
{
	return (pz_derive(a, crypto_flag, pw, pwlen, hdr));
}

void
pz_key_wipe(pz_aead_t *a)
{
	pcmono_wipe(a->key, sizeof (a->key));
	a->alg = 0;
}

/*
 * The nonce and associated data of piece ord of stream. The nonce is
 * stream (4 bytes) and ord (8), big-endian, then zeros to the AEAD's
 * length: unique within the file, and the key is unique to the file.
 * The associated data starts with the same 12 bytes, then the caller's.
 */
static int
pz_frame(const pz_aead_t *a, int stream, uint64_t ord, const uchar_t *ad, size_t adlen,
    uchar_t nonce[AEAD_NONCE_MAX], uchar_t full[12 + PZ_AD_MAX])
{
	if (a->alg == 0 || adlen > PZ_AD_MAX)
		return (-1);
	memset(nonce, 0, AEAD_NONCE_MAX);
	put_be32(nonce, (uint32_t)stream);
	put_be64(nonce + 4, ord);
	memcpy(full, nonce, 12);
	if (adlen > 0)
		memcpy(full + 12, ad, adlen);
	return (0);
}

int
pz_seal(const pz_aead_t *a, int stream, uint64_t ord, const uchar_t *ad,
    size_t adlen, uchar_t *buf, size_t len, uchar_t tag[AEAD_TAGLEN])
{
	uchar_t nonce[AEAD_NONCE_MAX], full[12 + PZ_AD_MAX];

	if (pz_frame(a, stream, ord, ad, adlen, nonce, full) != 0)
		return (-1);
	return (aead_seal(a->alg, a->key, nonce, full, 12 + adlen, buf, buf, len, tag));
}

int
pz_open(const pz_aead_t *a, int stream, uint64_t ord, const uchar_t *ad,
    size_t adlen, uchar_t *buf, size_t len, const uchar_t tag[AEAD_TAGLEN])
{
	uchar_t nonce[AEAD_NONCE_MAX], full[12 + PZ_AD_MAX];

	if (pz_frame(a, stream, ord, ad, adlen, nonce, full) != 0)
		return (-1);
	return (aead_open(a->alg, a->key, nonce, full, 12 + adlen, buf, buf, len, tag));
}

int
pz_end_make(const pz_aead_t *a, uint64_t ndata, uint64_t nmeta, uchar_t out[PZ_END_LEN])
{
	put_be64(out, ndata);
	put_be64(out + 8, nmeta);
	return (pz_seal(a, PZ_STREAM_END, 0, out, 16, NULL, 0, out + 16));
}

int
pz_end_check(const pz_aead_t *a, const uchar_t in[PZ_END_LEN], uint64_t *ndata,
    uint64_t *nmeta)
{
	if (pz_open(a, PZ_STREAM_END, 0, in, 16, NULL, 0, in + 16) != 0)
		return (-1);
	*ndata = get_be64(in);
	*nmeta = get_be64(in + 8);
	return (0);
}
