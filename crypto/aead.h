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

/**
 * @file aead.h
 * @brief Authenticated encryption (AEAD) and the Argon2id KDF.
 *
 * Two AEADs, both with 32-byte keys and 16-byte tags:
 * XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha: HChaCha20 from the bundled
 * Monocypher, then OpenSSL's RFC 8439 ChaCha20-Poly1305) with a 24-byte
 * nonce, and AES-256-GCM (OpenSSL) with a 12-byte nonce. Argon2id
 * (RFC 9106) is Monocypher's.
 */

#ifndef	_PC_AEAD_H
#define	_PC_AEAD_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	AEAD_XCHACHA20	1	/**< XChaCha20-Poly1305, 24-byte nonce */
#define	AEAD_AES256GCM	2	/**< AES-256-GCM, 12-byte nonce */
#define	AEAD_KEYLEN	32
#define	AEAD_TAGLEN	16
#define	AEAD_NONCE_MAX	24

/** Nonce length of alg (24 or 12), or 0 for an unknown algorithm. */
int aead_nonce_len(int alg);

/**
 * Encrypt len bytes from in to out (they may be the same buffer) and
 * authenticate them together with ad. nonce is aead_nonce_len(alg) bytes
 * and must never repeat under one key. Returns 0, or -1 on an unknown
 * algorithm or an OpenSSL failure.
 */
int aead_seal(int alg, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    const uint8_t *ad, size_t adlen, const uint8_t *in, uint8_t *out, size_t len,
    uint8_t tag[AEAD_TAGLEN]);

/**
 * Check tag over ad and the ciphertext, and decrypt in to out. Returns 0,
 * or -1 if the tag does not match (out is then zeroed) or on an error.
 */
int aead_open(int alg, const uint8_t key[AEAD_KEYLEN], const uint8_t *nonce,
    const uint8_t *ad, size_t adlen, const uint8_t *in, uint8_t *out, size_t len,
    const uint8_t tag[AEAD_TAGLEN]);

/** Argon2id cost: memory in KiB, passes, lanes. */
typedef struct {
	uint32_t m_kib, t, p;
} argon2_params_t;

/*
 * Bounds for a cost read from a file, checked before any work: memory
 * 8 KiB per lane up to 1 GiB, 1-16 passes, 1-16 lanes, and at most
 * 4 GiB of memory traffic (m * t) so a crafted file cannot make the KDF
 * run for minutes.
 */
#define	ARGON2_M_MAX	(1024U * 1024U)
#define	ARGON2_T_MAX	16
#define	ARGON2_P_MAX	16
#define	ARGON2_MT_MAX	(4ULL * 1024 * 1024)

/** 1 if kp is within the bounds above, else 0. */
int argon2_params_ok(const argon2_params_t *kp);

/**
 * Argon2id (RFC 9106, version 0x13) of pw with salt, no secret or
 * associated data, into out (outlen bytes, 4 or more). Refuses a cost
 * outside argon2_params_ok(). Returns 0, or -1 (bad cost, no memory).
 */
int argon2id_derive(const uint8_t *pw, size_t pwlen, const uint8_t *salt,
    size_t saltlen, const argon2_params_t *kp, uint8_t *out, size_t outlen);

#ifdef	__cplusplus
}
#endif

#endif
