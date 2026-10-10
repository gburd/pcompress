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
 *      
 */

/*-
 * Copyright (c) 2001-2003 Allan Saddi <allan@saddi.com>
 * Copyright (c) 2012 Moinak Ghosh moinakg <at1> gm0il <dot> com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Define WORDS_BIGENDIAN if compiling on a big-endian architecture.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#if HAVE_INTTYPES_H
# include <inttypes.h>
#else
# if HAVE_STDINT_H
#  include <stdint.h>
# endif
#endif

#include <pthread.h>
#include <string.h>
#include <utils.h>
#include "sha512.h"


#ifdef WORDS_BIGENDIAN

#define BYTESWAP(x) (x)
#define BYTESWAP64(x) (x)

#else /* WORDS_BIGENDIAN */

#define BYTESWAP(x) htonl(x)
#define BYTESWAP64(x) htonll(x)

#endif /* WORDS_BIGENDIAN */

typedef void (*update_func_ptr)(const void *input_data, void *digest, uint64_t num_blks);

static const uint8_t padding[SHA512_BLOCK_SIZE] = {
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint64_t iv512[SHA512_HASH_WORDS] = {
  0x6a09e667f3bcc908LL,
  0xbb67ae8584caa73bLL,
  0x3c6ef372fe94f82bLL,
  0xa54ff53a5f1d36f1LL,
  0x510e527fade682d1LL,
  0x9b05688c2b3e6c1fLL,
  0x1f83d9abfb41bd6bLL,
  0x5be0cd19137e2179LL
};

static const uint64_t iv256[SHA512_HASH_WORDS] = {
  0x22312194fc2bf72cLL,
  0x9f555fa3c84c64c2LL,
  0x2393b86b6f53b151LL,
  0x963877195940eabdLL,
  0x96283ee2a88effe3LL,
  0xbe5e1e2553863992LL,
  0x2b0199fc2c85b8aaLL,
  0x0eb72ddc81c52ca2LL
};

/*
 * Portable FIPS 180-4 SHA-512 block function, same interface as the Intel
 * SSE4/AVX routines: digest[8] in host order, input blocks big-endian.
 * Used on every CPU without those, so SHA256 (which is SHA-512/256) and
 * SHA512 give the same digests everywhere.
 */
static const uint64_t K512[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR64(x, n)	(((x) >> (n)) | ((x) << (64 - (n))))

static void
sha512_generic(const void *input_data, void *digest, uint64_t num_blks)
{
	const uint8_t *in = (const uint8_t *)input_data;
	uint64_t *H = (uint64_t *)digest;
	uint64_t W[80], a, b, c, d, e, f, g, h, t1, t2;
	int t, j;

	while (num_blks--) {
		for (t = 0; t < 16; t++) {
			uint64_t w = 0;
			for (j = 0; j < 8; j++)
				w = (w << 8) | in[t * 8 + j];
			W[t] = w;
		}
		for (t = 16; t < 80; t++) {
			uint64_t s0 = ROR64(W[t-15], 1) ^ ROR64(W[t-15], 8) ^ (W[t-15] >> 7);
			uint64_t s1 = ROR64(W[t-2], 19) ^ ROR64(W[t-2], 61) ^ (W[t-2] >> 6);
			W[t] = W[t-16] + s0 + W[t-7] + s1;
		}
		a = H[0]; b = H[1]; c = H[2]; d = H[3];
		e = H[4]; f = H[5]; g = H[6]; h = H[7];
		for (t = 0; t < 80; t++) {
			t1 = h + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) +
			    ((e & f) ^ (~e & g)) + K512[t] + W[t];
			t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) +
			    ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		H[0] += a; H[1] += b; H[2] += c; H[3] += d;
		H[4] += e; H[5] += f; H[6] += g; H[7] += h;
		in += SHA512_BLOCK_SIZE;
	}
}

static update_func_ptr sha512_update_func = sha512_generic;

/*
 * Pick the block function. Always succeeds: the portable function is the
 * fallback, so the optimized code path is used on every CPU.
 */
int
APS_NAMESPACE(Init_SHA512) (processor_cap_t *pc)
{
	sha512_update_func = sha512_generic;
#if defined(__x86_64__) && defined(HAVE_SHA512_ASM)
	if (pc->proc_type == PROC_X64_INTEL || pc->proc_type == PROC_X64_AMD) {
		if (pc->avx_level > 0)
			sha512_update_func = sha512_avx;
		else if (pc->sse_level >= 4)
			sha512_update_func = sha512_sse4;
	}
#endif
	return (0);
}

static void
_init (SHA512_Context *sc, const uint64_t iv[SHA512_HASH_WORDS])
{
	int i;

	sc->totalLength[0] = 0LL;
	sc->totalLength[1] = 0LL;
	for (i = 0; i < SHA512_HASH_WORDS; i++)
		sc->hash[i] = iv[i];
	sc->bufferLength = 0L;
}

void
APS_NAMESPACE(SHA512_Init) (SHA512_Context *sc)
{
	_init (sc, iv512);
}

void
APS_NAMESPACE(SHA512t256_Init) (SHA512_Context *sc)
{
	_init (sc, iv256);
}

void
APS_NAMESPACE(SHA512_Update) (SHA512_Context *sc, const void *vdata, size_t len)
{
	const uint8_t *data = (const uint8_t *)vdata;
	uint32_t bufferBytesLeft;
	size_t bytesToCopy;
	int rem;
	uint64_t carryCheck;

	if (sc->bufferLength) {
		do {
			bufferBytesLeft = SHA512_BLOCK_SIZE - sc->bufferLength;
			bytesToCopy = bufferBytesLeft;
			if (bytesToCopy > len)
				bytesToCopy = len;

			memcpy (&sc->buffer.bytes[sc->bufferLength], data, bytesToCopy);
			carryCheck = sc->totalLength[1];
			sc->totalLength[1] += bytesToCopy * 8L;
			if (sc->totalLength[1] < carryCheck)
				sc->totalLength[0]++;

			sc->bufferLength += bytesToCopy;
			data += bytesToCopy;
			len -= bytesToCopy;

			if (sc->bufferLength == SHA512_BLOCK_SIZE) {
				sc->blocks = 1;
				sha512_update_func(sc->buffer.words, sc->hash, sc->blocks);
				sc->bufferLength = 0L;
			} else {
				return;
			}
		} while (len > 0 && len <= SHA512_BLOCK_SIZE);
		if (!len) return;
	}

	sc->blocks = len >> 7;
	rem = len - (sc->blocks << 7);
	len = sc->blocks << 7;
	carryCheck = sc->totalLength[1];
	sc->totalLength[1] += rem * 8L;
	if (sc->totalLength[1] < carryCheck)
		sc->totalLength[0]++;

	if (len) {
		carryCheck = sc->totalLength[1];
		sc->totalLength[1] += len * 8L;
		if (sc->totalLength[1] < carryCheck)
			sc->totalLength[0]++;
		sha512_update_func((uint32_t *)data, sc->hash, sc->blocks);
	}
	if (rem) {
		memcpy (&sc->buffer.bytes[0], data + len, rem);
		sc->bufferLength = rem;
	}
}

void
APS_NAMESPACE(SHA512t256_Update) (SHA512_Context *sc, const void *data, size_t len)
{
	APS_NAMESPACE(SHA512_Update) (sc, data, len);
}

static void
_final (SHA512_Context *sc, uint8_t *hash, int hashWords, int halfWord)
{
	uint32_t bytesToPad;
	uint64_t lengthPad[2];
	int i;
	
	bytesToPad = 240L - sc->bufferLength;
	if (bytesToPad > SHA512_BLOCK_SIZE)
		bytesToPad -= SHA512_BLOCK_SIZE;
	
	lengthPad[0] = BYTESWAP64(sc->totalLength[0]);
	lengthPad[1] = BYTESWAP64(sc->totalLength[1]);
	
	APS_NAMESPACE(SHA512_Update) (sc, padding, bytesToPad);
	APS_NAMESPACE(SHA512_Update) (sc, lengthPad, 16L);
	
	if (hash) {
		for (i = 0; i < hashWords; i++) {
			*((uint64_t *) hash) = BYTESWAP64(sc->hash[i]);
			hash += 8;
		}
		if (halfWord) {
			hash[0] = (uint8_t) (sc->hash[i] >> 56);
			hash[1] = (uint8_t) (sc->hash[i] >> 48);
			hash[2] = (uint8_t) (sc->hash[i] >> 40);
			hash[3] = (uint8_t) (sc->hash[i] >> 32);
		}
	}
}

void
APS_NAMESPACE(SHA512_Final) (SHA512_Context *sc, uint8_t hash[SHA512_HASH_SIZE])
{
	_final (sc, hash, SHA512_HASH_WORDS, 0);
}

void
APS_NAMESPACE(SHA512t256_Final) (SHA512_Context *sc, uint8_t hash[SHA512t256_HASH_SIZE])
{
	_final (sc, hash, SHA512t256_HASH_WORDS, 0);
}

#define HASH_CONTEXT SHA512_Context
#define HASH_INIT APS_NAMESPACE(SHA512_Init)
#define HASH_UPDATE APS_NAMESPACE(SHA512_Update)
#define HASH_FINAL APS_NAMESPACE(SHA512_Final)
#define HASH_SIZE SHA512_HASH_SIZE
#define HASH_BLOCK_SIZE SHA512_BLOCK_SIZE

#define HMAC_CONTEXT HMAC_SHA512_Context
#define HMAC_INIT APS_NAMESPACE(HMAC_SHA512_Init)
#define HMAC_UPDATE APS_NAMESPACE(HMAC_SHA512_Update)
#define HMAC_FINAL APS_NAMESPACE(HMAC_SHA512_Final)

#include "_hmac.c"

#undef HASH_CONTEXT
#undef HASH_INIT
#undef HASH_UPDATE
#undef HASH_FINAL
#undef HASH_SIZE
#undef HASH_BLOCK_SIZE
#undef HMAC_CONTEXT
#undef HMAC_INIT
#undef HMAC_UPDATE
#undef HMAC_FINAL

#define HASH_CONTEXT SHA512_Context
#define HASH_INIT APS_NAMESPACE(SHA512t256_Init)
#define HASH_UPDATE APS_NAMESPACE(SHA512t256_Update)
#define HASH_FINAL APS_NAMESPACE(SHA512t256_Final)
#define HASH_SIZE SHA512t256_HASH_SIZE
#define HASH_BLOCK_SIZE SHA512_BLOCK_SIZE

#define HMAC_CONTEXT HMAC_SHA512_Context
#define HMAC_INIT APS_NAMESPACE(HMAC_SHA512t256_Init)
#define HMAC_UPDATE APS_NAMESPACE(HMAC_SHA512t256_Update)
#define HMAC_FINAL APS_NAMESPACE(HMAC_SHA512t256_Final)

#include "_hmac.c"

