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

#if defined(__aarch64__) || defined(__arm64__)

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <arm_neon.h>

#include "blake2.h"
#include "blake2-impl.h"
#include "blake2b-round-neon.h"

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
	uint8_t *p, *h, *v;
	v = (uint8_t *)(blake2b_IV);
	h = (uint8_t *)(S->h);
	p = (uint8_t *)(P);
	memset(S, 0, sizeof(blake2b_state));
	for (int i = 0; i < BLAKE2B_OUTBYTES; ++i) h[i] = v[i] ^ p[i];
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
	memcpy(out, &S->h[0], outlen);
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
/* Avoid empty translation unit warning on non-ARM platforms */
typedef int blake2b_neon_unused;
#endif /* __aarch64__ || __arm64__ */
