/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 */

/*
 * BLAKE2b implementation using ARM NEON intrinsics.
 * Based on the BLAKE2 reference source code package by Samuel Neves.
 */

/*
 * On non-x86 targets without NEON this file also provides the portable
 * scalar BLAKE2b (RFC 7693) under the same _neon names, so the dispatch
 * table is never left NULL.
 */
#if (!defined(__x86_64__) && !defined(__i386__)) || defined(PC_FORCE_GENERIC)

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "blake2.h"
#include "blake2-impl.h"
#if (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && !defined(PC_FORCE_GENERIC)
#include <arm_neon.h>
#include "blake2b-round-neon.h"
#define BLAKE2B_USE_NEON 1
#endif

#define BLAKE_NAMESPACE(x) x##_neon

static const uint64_t blake2b_IV[8] __attribute__((aligned(64))) = {
  0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
  0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
  0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
  0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static inline int blake2b_set_lastnode(blake2b_state *S)
{
	S->f[1] = ~0ULL;
	return 0;
}

static inline int blake2b_set_lastblock(blake2b_state *S)
{
	if (S->last_node) blake2b_set_lastnode(S);
	S->f[0] = ~0ULL;
	return 0;
}

static inline int blake2b_increment_counter(blake2b_state *S, const uint64_t inc)
{
	S->t[0] += inc;
	S->t[1] += (S->t[0] < inc);
	return 0;
}

int BLAKE_NAMESPACE(blake2b_init_param)(blake2b_state *S, const blake2b_param *P)
{
	const uint8_t *p = (const uint8_t *)(P);

	memset(S, 0, sizeof(blake2b_state));
	/* load64() is little-endian on every host (RFC 7693 section 2.4). */
	for (int i = 0; i < 8; ++i)
		S->h[i] = blake2b_IV[i] ^ load64(p + 8 * i);
	return 0;
}

int BLAKE_NAMESPACE(blake2b_init)(blake2b_state *S, const uint8_t outlen)
{
	if ((!outlen) || (outlen > BLAKE2B_OUTBYTES)) return -1;
	const blake2b_param P = {
		outlen, 0, 1, 1, 0, 0, 0, 0, {0}, {0}, {0}
	};
	return BLAKE_NAMESPACE(blake2b_init_param)(S, &P);
}

int BLAKE_NAMESPACE(blake2b_init_key)(blake2b_state *S, const uint8_t outlen,
	const void *key, const uint8_t keylen)
{
	if ((!outlen) || (outlen > BLAKE2B_OUTBYTES)) return -1;
	if ((!keylen) || keylen > BLAKE2B_KEYBYTES) return -1;

	const blake2b_param P = {
		outlen, keylen, 1, 1, 0, 0, 0, 0, {0}, {0}, {0}
	};

	if (BLAKE_NAMESPACE(blake2b_init_param)(S, &P) < 0)
		return 0;

	{
		uint8_t block[BLAKE2B_BLOCKBYTES];
		memset(block, 0, BLAKE2B_BLOCKBYTES);
		memcpy(block, key, keylen);
		BLAKE_NAMESPACE(blake2b_update)(S, block, BLAKE2B_BLOCKBYTES);
		secure_zero_memory(block, BLAKE2B_BLOCKBYTES);
	}
	return 0;
}

#if !defined(BLAKE2B_USE_NEON)
static const uint8_t blake2b_sigma_ref[12][16] = {
  {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
  { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
  { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
  {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
  {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
  {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
  { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
  { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
  {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
  { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
  {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
  { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define B2B_G(a, b, c, d, x, y) do { \
	v[a] = v[a] + v[b] + (x); v[d] = ROTR64(v[d] ^ v[a], 32); \
	v[c] = v[c] + v[d];       v[b] = ROTR64(v[b] ^ v[c], 24); \
	v[a] = v[a] + v[b] + (y); v[d] = ROTR64(v[d] ^ v[a], 16); \
	v[c] = v[c] + v[d];       v[b] = ROTR64(v[b] ^ v[c], 63); \
} while (0)

/* Portable, endian- and alignment-safe compression function (RFC 7693). */
static inline int blake2b_compress_neon(blake2b_state *S, const uint8_t block[BLAKE2B_BLOCKBYTES])
{
	uint64_t m[16], v[16];
	int i, r;

	for (i = 0; i < 16; i++)
		m[i] = load64(block + 8 * i);
	for (i = 0; i < 8; i++) {
		v[i] = S->h[i];
		v[i + 8] = blake2b_IV[i];
	}
	v[12] ^= S->t[0];
	v[13] ^= S->t[1];
	v[14] ^= S->f[0];
	v[15] ^= S->f[1];
	for (r = 0; r < 12; r++) {
		const uint8_t *s = blake2b_sigma_ref[r];
		B2B_G(0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);
		B2B_G(1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
		B2B_G(2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
		B2B_G(3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
		B2B_G(0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);
		B2B_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
		B2B_G(2, 7,  8, 13, m[s[12]], m[s[13]]);
		B2B_G(3, 4,  9, 14, m[s[14]], m[s[15]]);
	}
	for (i = 0; i < 8; i++)
		S->h[i] ^= v[i] ^ v[i + 8];
	return 0;
}
#else
static inline int blake2b_compress_neon(blake2b_state *S, const uint8_t block[BLAKE2B_BLOCKBYTES])
{
	uint64x2_t row1l, row1h;
	uint64x2_t row2l, row2h;
	uint64x2_t row3l, row3h;
	uint64x2_t row4l, row4h;

	const uint64_t *m = (const uint64_t *)block;

	row1l = vld1q_u64(&S->h[0]);
	row1h = vld1q_u64(&S->h[2]);
	row2l = vld1q_u64(&S->h[4]);
	row2h = vld1q_u64(&S->h[6]);
	row3l = vld1q_u64(&blake2b_IV[0]);
	row3h = vld1q_u64(&blake2b_IV[2]);
	row4l = veorq_u64(vld1q_u64(&blake2b_IV[4]), vld1q_u64(&S->t[0]));
	row4h = veorq_u64(vld1q_u64(&blake2b_IV[6]), vld1q_u64(&S->f[0]));

	ROUND_NEON(0, m);
	ROUND_NEON(1, m);
	ROUND_NEON(2, m);
	ROUND_NEON(3, m);
	ROUND_NEON(4, m);
	ROUND_NEON(5, m);
	ROUND_NEON(6, m);
	ROUND_NEON(7, m);
	ROUND_NEON(8, m);
	ROUND_NEON(9, m);
	ROUND_NEON(10, m);
	ROUND_NEON(11, m);

	row1l = veorq_u64(row3l, row1l);
	row1h = veorq_u64(row3h, row1h);
	vst1q_u64(&S->h[0], veorq_u64(vld1q_u64(&S->h[0]), row1l));
	vst1q_u64(&S->h[2], veorq_u64(vld1q_u64(&S->h[2]), row1h));

	row2l = veorq_u64(row4l, row2l);
	row2h = veorq_u64(row4h, row2h);
	vst1q_u64(&S->h[4], veorq_u64(vld1q_u64(&S->h[4]), row2l));
	vst1q_u64(&S->h[6], veorq_u64(vld1q_u64(&S->h[6]), row2h));

	return 0;
}
#endif /* BLAKE2B_USE_NEON */

int BLAKE_NAMESPACE(blake2b_update)(blake2b_state *S, const uint8_t *in, uint64_t inlen)
{
	while (inlen > 0) {
		size_t left = S->buflen;
		size_t fill = 2 * BLAKE2B_BLOCKBYTES - left;

		if (inlen > fill) {
			memcpy(S->buf + left, in, fill);
			S->buflen += fill;
			blake2b_increment_counter(S, BLAKE2B_BLOCKBYTES);
			blake2b_compress_neon(S, S->buf);
			memcpy(S->buf, S->buf + BLAKE2B_BLOCKBYTES, BLAKE2B_BLOCKBYTES);
			S->buflen -= BLAKE2B_BLOCKBYTES;
			in += fill;
			inlen -= fill;
		} else {
			memcpy(S->buf + left, in, inlen);
			S->buflen += inlen;
			in += inlen;
			inlen -= inlen;
		}
	}
	return 0;
}

int BLAKE_NAMESPACE(blake2b_final)(blake2b_state *S, uint8_t *out, uint8_t outlen)
{
	if (S->buflen > BLAKE2B_BLOCKBYTES) {
		blake2b_increment_counter(S, BLAKE2B_BLOCKBYTES);
		blake2b_compress_neon(S, S->buf);
		S->buflen -= BLAKE2B_BLOCKBYTES;
		memcpy(S->buf, S->buf + BLAKE2B_BLOCKBYTES, S->buflen);
	}

	blake2b_increment_counter(S, S->buflen);
	blake2b_set_lastblock(S);
	memset(S->buf + S->buflen, 0, 2 * BLAKE2B_BLOCKBYTES - S->buflen);
	blake2b_compress_neon(S, S->buf);
	{
		uint8_t buffer[BLAKE2B_OUTBYTES];

		for (int i = 0; i < 8; ++i)
			store64(buffer + 8 * i, S->h[i]);
		memcpy(out, buffer, outlen);
	}
	return 0;
}

int BLAKE_NAMESPACE(blake2b)(uint8_t *out, const void *in, const void *key,
	const uint8_t outlen, const uint64_t inlen, uint8_t keylen)
{
	blake2b_state S[1];

	if (NULL == in) return -1;
	if (NULL == out) return -1;
	if (NULL == key) keylen = 0;

	if (keylen) {
		if (BLAKE_NAMESPACE(blake2b_init_key)(S, outlen, key, keylen) < 0)
			return -1;
	} else {
		if (BLAKE_NAMESPACE(blake2b_init)(S, outlen) < 0)
			return -1;
	}

	BLAKE_NAMESPACE(blake2b_update)(S, (uint8_t *)in, inlen);
	BLAKE_NAMESPACE(blake2b_final)(S, out, outlen);
	return 0;
}

#else
/* Avoid empty translation unit warning on x86 */
typedef int blake2b_neon_unused;
#endif /* !x86 */
