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
 * AEADs and Argon2id for the .pz format and chunk objects.
 *
 * The AEADs run on OpenSSL's EVP code (ChaCha20-Poly1305 and AES-256-GCM),
 * which is vetted and, on x86-64 and ARM64, vectorised: 4.2 GB/s for
 * ChaCha20-Poly1305 on a Xeon 8488C, where Monocypher's portable C runs
 * at 0.48 GB/s. OpenSSL has no XChaCha20, so the extended nonce is the
 * draft-irtf-cfrg-xchacha construction: HChaCha20 (Monocypher's
 * pcmono_chacha20_h()) turns the key and the first 16 nonce bytes into a
 * subkey, and the IETF nonce is four zero bytes and the last 8 nonce
 * bytes. Argon2id is Monocypher's: OpenSSL has Argon2 only from 3.2.
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <openssl/evp.h>
#include <utils.h>
#include <allocator.h>
#include "aead.h"
#include "monocypher/monocypher.h"

int
aead_nonce_len(int alg)
{
	if (alg == AEAD_XCHACHA20)
		return (24);
	if (alg == AEAD_AES256GCM)
		return (12);
	return (0);
}

/*
 * The OpenSSL cipher, key and 12-byte nonce for alg: for XChaCha20 the
 * HChaCha20 subkey and the IETF nonce made from the 24-byte one.
 */
static const EVP_CIPHER *
aead_setup(int alg, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    uint8_t subkey[AEAD_KEYLEN], uint8_t iv[12])
{
	if (alg == AEAD_XCHACHA20) {
		pcmono_chacha20_h(subkey, key, nonce);
		memset(iv, 0, 4);
		memcpy(iv + 4, nonce + 16, 8);
		return (EVP_chacha20_poly1305());
	}
	if (alg == AEAD_AES256GCM) {
		memcpy(subkey, key, AEAD_KEYLEN);
		memcpy(iv, nonce, 12);
		return (EVP_aes_256_gcm());
	}
	return (NULL);
}

/* EVP_CipherUpdate() takes an int length: feed len bytes in pieces. */
static int
aead_update(EVP_CIPHER_CTX *x, uint8_t *out, const uint8_t *in, size_t len)
{
	int n;

	while (len > 0) {
		int piece = len > (1U << 30) ? (1 << 30) : (int)len;

		if (EVP_CipherUpdate(x, out, &n, in, piece) != 1 || n != piece)
			return (-1);
		if (out != NULL)
			out += piece;
		in += piece;
		len -= piece;
	}
	return (0);
}

/*
 * One AEAD pass: enc 1 seals (writes tag), 0 opens (checks tag). The
 * OpenSSL context is per call: callers run in many threads at once.
 */
static int
aead_run(int alg, int enc, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    const uint8_t *ad, size_t adlen, const uint8_t *in, uint8_t *out, size_t len,
    uint8_t tag[AEAD_TAGLEN])
{
	uint8_t subkey[AEAD_KEYLEN], iv[12], fin[16];
	const EVP_CIPHER *cipher = aead_setup(alg, key, nonce, subkey, iv);
	EVP_CIPHER_CTX *x = NULL;
	int n, rv = -1;

	if (cipher == NULL || (x = EVP_CIPHER_CTX_new()) == NULL)
		goto out;
	if (EVP_CipherInit_ex(x, cipher, NULL, subkey, iv, enc) != 1)
		goto out;
	if (!enc && EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_TAG, AEAD_TAGLEN, tag) != 1)
		goto out;
	if (adlen > 0 && aead_update(x, NULL, ad, adlen) != 0)
		goto out;
	if (len > 0 && aead_update(x, out, in, len) != 0)
		goto out;
	/* Opening: the tag is checked here, 0 on a mismatch. */
	if (EVP_CipherFinal_ex(x, fin, &n) != 1)
		goto out;
	if (enc && EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_GET_TAG, AEAD_TAGLEN, tag) != 1)
		goto out;
	rv = 0;
out:
	if (x != NULL)
		EVP_CIPHER_CTX_free(x);
	pcmono_wipe(subkey, sizeof (subkey));
	if (rv != 0 && !enc && len > 0)
		memset(out, 0, len);	/* never hand out unauthenticated plaintext */
	return (rv);
}

int
aead_seal(int alg, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    const uint8_t *ad, size_t adlen, const uint8_t *in, uint8_t *out, size_t len,
    uint8_t tag[AEAD_TAGLEN])
{
	return (aead_run(alg, 1, key, nonce, ad, adlen, in, out, len, tag));
}

int
aead_open(int alg, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    const uint8_t *ad, size_t adlen, const uint8_t *in, uint8_t *out, size_t len,
    const uint8_t tag[AEAD_TAGLEN])
{
	uint8_t t[AEAD_TAGLEN];

	/* EVP's set-tag control takes a non-const pointer. */
	memcpy(t, tag, sizeof (t));
	return (aead_run(alg, 0, key, nonce, ad, adlen, in, out, len, t));
}

int
argon2_params_ok(const argon2_params_t *kp)
{
	if (kp->p < 1 || kp->p > ARGON2_P_MAX || kp->t < 1 || kp->t > ARGON2_T_MAX)
		return (0);
	/* RFC 9106: at least 8 KiB per lane. */
	if (kp->m_kib < 8 * kp->p || kp->m_kib > ARGON2_M_MAX)
		return (0);
	return ((uint64_t)kp->m_kib * kp->t <= ARGON2_MT_MAX);
}

int
argon2id_derive(const uint8_t *pw, size_t pwlen, const uint8_t *salt,
    size_t saltlen, const argon2_params_t *kp, uint8_t *out, size_t outlen)
{
	pcmono_argon2_config cf;
	pcmono_argon2_inputs in;
	void *work;

	if (!argon2_params_ok(kp) || outlen < 4 || outlen > UINT32_MAX ||
	    pwlen > UINT32_MAX || saltlen < 8 || saltlen > UINT32_MAX)
		return (-1);
	/* 1 KiB blocks of 64-bit words: pc_malloc() alignment is enough. */
	if ((work = pc_malloc((size_t)kp->m_kib * 1024)) == NULL)
		return (-1);
	cf.algorithm = CRYPTO_ARGON2_ID;
	cf.nb_blocks = kp->m_kib;
	cf.nb_passes = kp->t;
	cf.nb_lanes = kp->p;
	in.pass = pw;
	in.pass_size = (uint32_t)pwlen;
	in.salt = salt;
	in.salt_size = (uint32_t)saltlen;
	/* Wipes the work area itself. */
	pcmono_argon2(out, (uint32_t)outlen, work, cf, in, pcmono_argon2_no_extras);
	pc_mfree(work);
	return (0);
}
