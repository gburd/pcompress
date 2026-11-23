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
 * Repair data for .pz files: a systematic Reed-Solomon code over GF(2^8)
 * with a Cauchy generator, applied to fixed-size blocks of the finished
 * file and interleaved so that a run of bad sectors costs each stripe at
 * most a few blocks. The region goes at the very end of the file, after
 * the end record and any seek index, and covers every byte before it.
 * DESIGN.md ("Repair data") has the layout; this file is all of it.
 *
 * Finding damage does not depend on the .pz format: the region holds a
 * CRC-32 of every block, twice, so a repair needs neither the password
 * nor a parse of the chunks. The chunk checksums and AEAD tags still
 * decide whether the repaired file is right when it is decompressed.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>
#include <utils.h>
#include <pc_alloc.h>
#include <crypto_utils.h>
#include "repair.h"

extern uint32_t lzma_crc32(const uint8_t *buf, size_t size, uint32_t crc);

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define	RP_X86	1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define	RP_NEON	1
#endif

#define	RP_FLAG_SEALED	1	/* header carries an AEAD tag (version 12) */
#define	RP_TARGET_M	4	/* parity blocks per stripe the writer aims for */
#define	RP_G_DEFAULT	64	/* stripes per group the writer uses */
#define	RP_TAG_OFF	64	/* AEAD tag, after the fields and their hash */
#define	RP_HASH_OFF	48

static unsigned char gf_exp[512], gf_log[256];
static void (*muladd_fn)(unsigned char *, const unsigned char *, unsigned char, size_t);
static pthread_once_t gf_once = PTHREAD_ONCE_INIT;

static void
put_le(unsigned char *p, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get_le(const unsigned char *p, int n)
{
	uint64_t v = 0;
	int i;

	for (i = n - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return (v);
}

/*
 * The field: GF(2^8) with x^8 + x^4 + x^3 + x^2 + 1 (0x11d) and
 * generator 2, as in most storage Reed-Solomon codes. Tables, then the
 * fastest multiply-accumulate this CPU has.
 */
#ifdef RP_X86
static void muladd_ssse3(unsigned char *, const unsigned char *, unsigned char, size_t);
static void muladd_avx2(unsigned char *, const unsigned char *, unsigned char, size_t);
#endif
#ifdef RP_NEON
static void muladd_neon(unsigned char *, const unsigned char *, unsigned char, size_t);
#endif

static void
gf_init_once(void)
{
	unsigned int i, x = 1;

	for (i = 0; i < 255; i++) {
		gf_exp[i] = (unsigned char)x;
		gf_log[x] = (unsigned char)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11d;
	}
	for (i = 255; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];
	muladd_fn = rp_muladd_scalar;
#ifdef RP_X86
	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2"))
		muladd_fn = muladd_avx2;
	else if (__builtin_cpu_supports("ssse3"))
		muladd_fn = muladd_ssse3;
#endif
#ifdef RP_NEON
	muladd_fn = muladd_neon;
#endif
}

static void
gf_init(void)
{
	pthread_once(&gf_once, gf_init_once);
}

unsigned char
rp_gf_mul(unsigned char a, unsigned char b)
{
	gf_init();
	if (a == 0 || b == 0)
		return (0);
	return (gf_exp[gf_log[a] + gf_log[b]]);
}

/* NB: a must not be 0. */
unsigned char
rp_gf_inv(unsigned char a)
{
	gf_init();
	return (gf_exp[255 - gf_log[a]]);
}

/*
 * The Cauchy matrix entry 1 / (x_i + y_r) with x_i = i for the m parity
 * rows and y_r = m + r for the data columns: all distinct, so the sum
 * is never 0, and every square submatrix is invertible, which is what
 * makes any m of the k + m blocks of a stripe enough to rebuild it.
 */
unsigned char
rp_coef(uint32_t i, uint32_t r, uint32_t m)
{
	return (rp_gf_inv((unsigned char)(i ^ (m + r))));
}

void
rp_muladd_scalar(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len)
{
	unsigned char row[256];
	size_t i;

	if (c == 0)
		return;
	gf_init();
	row[0] = 0;
	for (i = 1; i < 256; i++)
		row[i] = gf_exp[gf_log[c] + gf_log[i]];
	for (i = 0; i < len; i++)
		dst[i] ^= row[src[i]];
}

/* The products of c with every low nibble and every high nibble. */
static void
nibble_tables(unsigned char c, unsigned char lo[16], unsigned char hi[16])
{
	int i;

	for (i = 0; i < 16; i++) {
		lo[i] = rp_gf_mul(c, (unsigned char)i);
		hi[i] = rp_gf_mul(c, (unsigned char)(i << 4));
	}
}

#ifdef RP_X86
__attribute__((target("ssse3"))) static void
muladd_ssse3(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len)
{
	unsigned char lo[16], hi[16];
	__m128i tl, th, mask;
	size_t i = 0;

	if (c == 0)
		return;
	nibble_tables(c, lo, hi);
	tl = _mm_loadu_si128((const __m128i *)lo);
	th = _mm_loadu_si128((const __m128i *)hi);
	mask = _mm_set1_epi8(0x0f);
	for (; i + 16 <= len; i += 16) {
		__m128i s = _mm_loadu_si128((const __m128i *)(src + i));
		__m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
		__m128i pl = _mm_shuffle_epi8(tl, _mm_and_si128(s, mask));
		__m128i ph = _mm_shuffle_epi8(th, _mm_and_si128(_mm_srli_epi64(s, 4), mask));

		_mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, _mm_xor_si128(pl, ph)));
	}
	if (i < len)
		rp_muladd_scalar(dst + i, src + i, c, len - i);
}

__attribute__((target("avx2"))) static void
muladd_avx2(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len)
{
	unsigned char lo[16], hi[16];
	__m256i tl, th, mask;
	size_t i = 0;

	if (c == 0)
		return;
	nibble_tables(c, lo, hi);
	tl = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)lo));
	th = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)hi));
	mask = _mm256_set1_epi8(0x0f);
	for (; i + 32 <= len; i += 32) {
		__m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
		__m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
		__m256i pl = _mm256_shuffle_epi8(tl, _mm256_and_si256(s, mask));
		__m256i ph = _mm256_shuffle_epi8(th,
		    _mm256_and_si256(_mm256_srli_epi64(s, 4), mask));

		_mm256_storeu_si256((__m256i *)(dst + i),
		    _mm256_xor_si256(d, _mm256_xor_si256(pl, ph)));
	}
	if (i < len)
		rp_muladd_scalar(dst + i, src + i, c, len - i);
}
#endif

#ifdef RP_NEON
static void
muladd_neon(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len)
{
	unsigned char lo[16], hi[16];
	uint8x16_t tl, th, mask;
	size_t i = 0;

	if (c == 0)
		return;
	nibble_tables(c, lo, hi);
	tl = vld1q_u8(lo);
	th = vld1q_u8(hi);
	mask = vdupq_n_u8(0x0f);
	for (; i + 16 <= len; i += 16) {
		uint8x16_t s = vld1q_u8(src + i);
		uint8x16_t p = veorq_u8(vqtbl1q_u8(tl, vandq_u8(s, mask)),
		    vqtbl1q_u8(th, vshrq_n_u8(s, 4)));

		vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), p));
	}
	if (i < len)
		rp_muladd_scalar(dst + i, src + i, c, len - i);
}
#endif

void
rp_muladd(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len)
{
	gf_init();
	muladd_fn(dst, src, c, len);
}

/*
 * Invert the e x e matrix a in place (Gauss-Jordan). A Cauchy
 * submatrix is never singular; -1 only guards a caller's mistake.
 */
static int
gf_invert(unsigned char *a, int e)
{
	unsigned char *inv, t;
	int r, c, p;

	if ((inv = pc_calloc((size_t)e, (size_t)e)) == NULL)
		return (-1);
	for (r = 0; r < e; r++)
		inv[r * e + r] = 1;
	for (c = 0; c < e; c++) {
		for (p = c; p < e && a[p * e + c] == 0; p++)
			;
		if (p == e) {
			pc_mfree(inv);
			return (-1);
		}
		for (r = 0; r < e; r++) {	/* swap rows p and c */
			t = a[p * e + r]; a[p * e + r] = a[c * e + r]; a[c * e + r] = t;
			t = inv[p * e + r]; inv[p * e + r] = inv[c * e + r]; inv[c * e + r] = t;
		}
		t = rp_gf_inv(a[c * e + c]);
		for (r = 0; r < e; r++) {
			a[c * e + r] = rp_gf_mul(a[c * e + r], t);
			inv[c * e + r] = rp_gf_mul(inv[c * e + r], t);
		}
		for (p = 0; p < e; p++) {
			if (p == c || (t = a[p * e + c]) == 0)
				continue;
			for (r = 0; r < e; r++) {
				a[p * e + r] ^= rp_gf_mul(t, a[c * e + r]);
				inv[p * e + r] ^= rp_gf_mul(t, inv[c * e + r]);
			}
		}
	}
	memcpy(a, inv, (size_t)e * e);
	pc_mfree(inv);
	return (0);
}

/*
 * Rebuild a stripe's bad blocks (rp_stripe_fix() in repair.h). The
 * syndromes of e good parity rows, with the good data taken out, are
 * the bad data times an e x e Cauchy submatrix: invert it, multiply.
 */
int
rp_stripe_fix(unsigned char **blk, unsigned char *ok, uint32_t k, uint32_t m, size_t len)
{
	unsigned char *a = NULL, *syn = NULL;
	uint32_t bad[RP_STRIPE_MAX], rows[RP_STRIPE_MAX], e = 0, nr = 0, i, j, r;

	for (r = 0; r < k; r++)
		if (!ok[r])
			bad[e++] = r;
	for (i = 0; i < m && nr < e; i++)
		if (ok[k + i])
			rows[nr++] = i;
	if (nr < e)
		return (-1);
	if (e > 0) {
		syn = pc_calloc(e, len);
		a = pc_malloc((size_t)e * e);
		if (syn == NULL || a == NULL) {
			pc_mfree(syn);
			pc_mfree(a);
			return (-1);
		}
		for (j = 0; j < e; j++) {
			memcpy(syn + j * len, blk[k + rows[j]], len);
			for (r = 0; r < k; r++)
				if (ok[r])
					rp_muladd(syn + j * len, blk[r], rp_coef(rows[j], r, m), len);
			for (i = 0; i < e; i++)
				a[j * e + i] = rp_coef(rows[j], bad[i], m);
		}
		if (gf_invert(a, (int)e) != 0) {
			pc_mfree(syn);
			pc_mfree(a);
			return (-1);
		}
		for (i = 0; i < e; i++) {
			memset(blk[bad[i]], 0, len);
			for (j = 0; j < e; j++)
				rp_muladd(blk[bad[i]], syn + j * len, a[i * e + j], len);
			ok[bad[i]] = 1;
		}
		pc_mfree(syn);
		pc_mfree(a);
	}
	for (i = 0; i < m; i++) {
		if (ok[k + i])
			continue;
		memset(blk[k + i], 0, len);
		for (r = 0; r < k; r++)
			rp_muladd(blk[k + i], blk[r], rp_coef(i, r, m), len);
		ok[k + i] = 1;
	}
	return (0);
}

/*
 * Fill in the derived sizes of h from plen, bsz, k, m and g, refusing
 * any value no writer uses or that would overflow. 0 or -1.
 */
static int
rp_geometry(rp_hdr_t *h)
{
	uint64_t per;

	if (h->bsz < RP_BLOCK_MIN || h->bsz > RP_BLOCK_MAX || (h->bsz & (h->bsz - 1)) != 0 ||
	    h->k < 1 || h->m < 1 || h->k + h->m > RP_STRIPE_MAX ||
	    h->g < 1 || h->g > RP_G_MAX ||
	    (uint64_t)h->m * h->g * h->bsz > RP_GROUP_PARITY_MAX ||
	    h->plen == 0 || h->plen > (1ULL << 52))
		return (-1);
	h->nblk = (h->plen + h->bsz - 1) / h->bsz;
	per = (uint64_t)h->k * h->g;
	h->ngroups = (h->nblk + per - 1) / per;
	h->npar = h->ngroups * h->m * h->g;
	h->rlen = 2 * RP_HDR_SZ + 8 + 2 * 4 * (h->nblk + h->npar) + h->npar * h->bsz;
	return (0);
}

/*
 * Choose the geometry for plen bytes at pct percent parity: blocks of
 * 4 KB, doubled until there are at most 2^20 of them (64 KB at most),
 * stripes of k data and m parity blocks with m / k close to pct and m
 * about RP_TARGET_M (more parity rows cost time on every byte, fewer
 * make a stripe fail sooner), and RP_G_DEFAULT stripes per group, fewer
 * if the file is smaller than one group.
 */
static void
rp_choose(rp_hdr_t *h, uint64_t plen, int pct)
{
	uint64_t n;

	memset(h, 0, sizeof (*h));
	h->plen = plen;
	h->bsz = RP_BLOCK;
	while (h->bsz < RP_BLOCK_MAX && plen / h->bsz > (1ULL << 20))
		h->bsz *= 2;
	n = (plen + h->bsz - 1) / h->bsz;
	h->k = (uint32_t)(100 * RP_TARGET_M / pct);
	if (h->k + h->k * pct / 100 + 1 > RP_STRIPE_MAX)
		h->k = (uint32_t)(RP_STRIPE_MAX * 100 / (100 + pct)) - 1;
	if (h->k > n)
		h->k = (uint32_t)n;
	h->m = (h->k * pct + 99) / 100;
	h->g = RP_G_DEFAULT;
	if ((uint64_t)h->k * h->g > n)
		h->g = (uint32_t)((n + h->k - 1) / h->k);
}

/* The fields of h, their hash, and for version 12 the AEAD tag. */
static int
rp_hdr_encode(rp_hdr_t *h, uint64_t ilen, const pz_aead_t *a, const unsigned char *end)
{
	unsigned char *b = h->raw, ad[RP_TAG_OFF + PZ_END_LEN];

	memset(b, 0, RP_HDR_SZ);
	memcpy(b, RP_HDR_MAGIC, 8);
	put_le(b + 8, h->bsz, 4);
	put_le(b + 12, h->k, 2);
	put_le(b + 14, h->m, 2);
	put_le(b + 16, h->g, 2);
	put_le(b + 18, a != NULL ? RP_FLAG_SEALED : 0, 2);
	put_le(b + 24, h->plen, 8);
	put_le(b + 32, ilen, 8);
	put_le(b + 40, h->rlen, 8);
	if (blake2b_keyed2(b + RP_HASH_OFF, 16, NULL, 0, b, RP_HASH_OFF, NULL, 0) != 0)
		return (-1);
	if (a == NULL)
		return (0);
	memcpy(ad, b, RP_TAG_OFF);
	memcpy(ad + RP_TAG_OFF, end, PZ_END_LEN);
	return (pc_pz_seal(a, PZ_STREAM_REPAIR, 0, ad, sizeof (ad), NULL, 0, b + RP_TAG_OFF));
}

int
rp_hdr_parse(const unsigned char *b, rp_hdr_t *h)
{
	unsigned char sum[16];
	uint64_t rlen;
	int flags, i;

	memset(h, 0, sizeof (*h));
	if (memcmp(b, RP_HDR_MAGIC, 8) != 0 ||
	    blake2b_keyed2(sum, 16, NULL, 0, b, RP_HASH_OFF, NULL, 0) != 0 ||
	    memcmp(sum, b + RP_HASH_OFF, 16) != 0)
		return (-1);
	flags = (int)get_le(b + 18, 2);
	if ((flags & ~RP_FLAG_SEALED) != 0 || get_le(b + 20, 4) != 0)
		return (-1);
	if (!(flags & RP_FLAG_SEALED))
		for (i = RP_TAG_OFF; i < RP_HDR_SZ; i++)
			if (b[i] != 0)
				return (-1);
	h->bsz = (uint32_t)get_le(b + 8, 4);
	h->k = (uint32_t)get_le(b + 12, 2);
	h->m = (uint32_t)get_le(b + 14, 2);
	h->g = (uint32_t)get_le(b + 16, 2);
	h->plen = get_le(b + 24, 8);
	rlen = get_le(b + 40, 8);
	if (rp_geometry(h) != 0 || rlen != h->rlen || get_le(b + 32, 8) > h->plen)
		return (-1);
	memcpy(h->raw, b, RP_HDR_SZ);
	return (0);
}

int
rp_tail_ok(const unsigned char *tr, uint64_t tail, const pz_aead_t *a,
    const unsigned char *end, uint64_t *rlen)
{
	unsigned char ad[RP_TAG_OFF + PZ_END_LEN];
	rp_hdr_t h;

	if (memcmp(tr + RP_HDR_SZ, RP_TAIL_MAGIC, 8) != 0 || rp_hdr_parse(tr, &h) != 0 ||
	    !(get_le(tr + 18, 2) & RP_FLAG_SEALED))
		return (-1);
	memcpy(ad, tr, RP_TAG_OFF);
	memcpy(ad + RP_TAG_OFF, end, PZ_END_LEN);
	if (pc_pz_verify(a, PZ_STREAM_REPAIR, 0, ad, sizeof (ad), NULL, 0,
	    tr + RP_TAG_OFF) != 0)
		return (-1);
	/* The authenticated lengths: what lies between end record and region. */
	if (tail != get_le(tr + 32, 8) + h.rlen)
		return (-1);
	*rlen = h.rlen;
	return (0);
}

/* Read len bytes at off; short reads past the end leave zeros. Bytes read, or -1. */
static int64_t
rp_pread(int fd, uint64_t off, void *buf, uint64_t len)
{
	int64_t n;

	memset(buf, 0, len);
	if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1)
		return (-1);
	n = Read(fd, buf, len);
	return (n);
}

static int
rp_pwrite(int fd, uint64_t off, const void *buf, uint64_t len)
{
	if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1 ||
	    Write(fd, buf, len) != (int64_t)len)
		return (-1);
	return (0);
}

uint64_t
rp_logical_size(int fd, uint64_t fsize)
{
	unsigned char tr[RP_TRAILER_SZ];
	rp_hdr_t h;

	if (fsize < RP_TRAILER_SZ ||
	    rp_pread(fd, fsize - RP_TRAILER_SZ, tr, RP_TRAILER_SZ) != RP_TRAILER_SZ ||
	    memcmp(tr + RP_HDR_SZ, RP_TAIL_MAGIC, 8) != 0 || rp_hdr_parse(tr, &h) != 0 ||
	    h.plen + h.rlen != fsize)
		return (fsize);
	return (h.plen);
}

/* Where parity block (group gi, row i, stripe s) lies in the region. */
static uint64_t
rp_par_off(const rp_hdr_t *h, uint64_t gi, uint32_t i, uint32_t s)
{
	uint64_t idx = gi * h->m * h->g + (uint64_t)i * h->g + s;

	return (h->plen + RP_HDR_SZ + 4 * (h->nblk + h->npar) + idx * h->bsz);
}

/*
 * Data block number of (group gi, row r, stripe s): consecutive blocks
 * go to consecutive stripes, so a run of g blocks hits each stripe once.
 */
static uint64_t
rp_blk(const rp_hdr_t *h, uint64_t gi, uint32_t r, uint32_t s)
{
	return (gi * h->k * h->g + (uint64_t)r * h->g + s);
}

/*
 * Compute group gi's parity into par (m * g blocks, zeroed here) from fd,
 * recording each data block's CRC in crc[]. row is a g-block buffer.
 */
static int
rp_encode_group(int fd, const rp_hdr_t *h, uint64_t gi, unsigned char *par,
    unsigned char *row, uint32_t *crc)
{
	uint64_t b0, nb, bytes;
	uint32_t r, s, i, ns;

	memset(par, 0, (size_t)h->m * h->g * h->bsz);
	for (r = 0; r < h->k; r++) {
		b0 = rp_blk(h, gi, r, 0);
		if (b0 >= h->nblk)
			break;
		nb = h->nblk - b0 < h->g ? h->nblk - b0 : h->g;
		ns = (uint32_t)nb;
		bytes = b0 * h->bsz + nb * h->bsz > h->plen ? h->plen - b0 * h->bsz :
		    nb * h->bsz;
		/* Read no further than plen: the region's header follows it. */
		memset(row, 0, nb * h->bsz);
		if (rp_pread(fd, b0 * h->bsz, row, bytes) != (int64_t)bytes)
			return (-1);
		for (s = 0; s < ns; s++) {
			unsigned char *blk = row + (size_t)s * h->bsz;

			crc[b0 + s] = lzma_crc32(blk, h->bsz, 0);
			for (i = 0; i < h->m; i++)
				rp_muladd(par + ((size_t)i * h->g + s) * h->bsz, blk,
				    rp_coef(i, r, h->m), h->bsz);
		}
	}
	return (0);
}

/*
 * Write the region for h (header already encoded in h->raw) at h->plen
 * of fd: header, CRC table, parity, CRC table again, header again, magic.
 * The first table is written last, when every CRC is known.
 */
static int
rp_emit(int fd, const rp_hdr_t *h)
{
	uint64_t ncrc = h->nblk + h->npar, gi, tab = h->plen + RP_HDR_SZ;
	uint64_t tab2 = tab + 4 * ncrc + h->npar * h->bsz;
	size_t gpar = (size_t)h->m * h->g * h->bsz;
	unsigned char *par, *row, *ct = NULL;
	uint32_t *crc, j;
	int rv = -1;

	par = pc_malloc(gpar);
	row = pc_malloc((size_t)h->g * h->bsz);
	crc = pc_malloc(ncrc * sizeof (uint32_t));
	if (par == NULL || row == NULL || crc == NULL ||
	    (ct = pc_malloc(ncrc * 4)) == NULL) {
		log_error_code(PC_ERR_NOMEM);
		log_msg(LOG_ERR, 0, "Out of memory writing repair data.");
		goto out;
	}
	if (rp_pwrite(fd, h->plen, h->raw, RP_HDR_SZ) != 0)
		goto io;
	for (gi = 0; gi < h->ngroups; gi++) {
		if (rp_encode_group(fd, h, gi, par, row, crc) != 0)
			goto io;
		for (j = 0; j < h->m * h->g; j++)
			crc[h->nblk + gi * h->m * h->g + j] =
			    lzma_crc32(par + (size_t)j * h->bsz, h->bsz, 0);
		if (rp_pwrite(fd, rp_par_off(h, gi, 0, 0), par, gpar) != 0)
			goto io;
	}
	for (gi = 0; gi < ncrc; gi++)
		put_le(ct + 4 * gi, crc[gi], 4);
	if (rp_pwrite(fd, tab2, ct, ncrc * 4) != 0 ||
	    rp_pwrite(fd, tab2 + ncrc * 4, h->raw, RP_HDR_SZ) != 0 ||
	    rp_pwrite(fd, tab2 + ncrc * 4 + RP_HDR_SZ, RP_TAIL_MAGIC, 8) != 0 ||
	    rp_pwrite(fd, tab, ct, ncrc * 4) != 0)
		goto io;
	rv = 0;
	goto out;
io:
	log_msg(LOG_ERR, 1, "Writing repair data: ");
out:
	pc_mfree(par);
	pc_mfree(row);
	pc_mfree(crc);
	pc_mfree(ct);
	return (rv);
}

int
rp_write(int fd, uint64_t plen, uint64_t ilen, int pct, const pz_aead_t *a,
    const unsigned char *end)
{
	rp_hdr_t h;

	rp_choose(&h, plen, pct);
	if (rp_geometry(&h) != 0 || rp_hdr_encode(&h, ilen, a, end) != 0) {
		log_error_code(PC_ERR_INTERNAL);
		log_msg(LOG_ERR, 0, "Cannot set up repair data for %" PRIu64 " bytes.", plen);
		return (-1);
	}
	return (rp_emit(fd, &h));
}

/*
 * Repair. Pass 1 reads every block of the file and of the parity area in
 * order and compares its CRC with the table; pass 2 rebuilds the stripes
 * that hold a bad block. Nothing is written until every damaged stripe
 * is known to be repairable, and each rebuilt block must match its CRC.
 */
typedef struct {
	int fd;
	rp_hdr_t h;
	uint64_t ncrc;		/* nblk + npar */
	uint64_t tab1, tab2;	/* file offsets of the two CRC tables */
	uint32_t *t1, *t2;	/* the tables as read */
	unsigned char *have1, *have2;	/* entry read without an I/O error */
	uint32_t *crc;		/* each block's CRC as read (or rebuilt) */
	unsigned char *bad;	/* block failed its CRC or could not be read */
	uint64_t nbad;
	int fix_hdr;		/* a header copy or the magic is damaged */
} rp_job_t;

/* File offset of block idx of the table order (data, then parity). */
static uint64_t
rp_blk_off(const rp_hdr_t *h, uint64_t idx)
{
	if (idx < h->nblk)
		return (idx * h->bsz);
	return (h->plen + RP_HDR_SZ + 4 * (h->nblk + h->npar) + (idx - h->nblk) * h->bsz);
}

/*
 * Read block idx into buf (bsz bytes), zero past the protected bytes for
 * the last data block. -1 on an I/O error: an unreadable sector is the
 * damage this code exists for, so the caller counts it as a bad block.
 */
static int
rp_read_blk(const rp_job_t *j, uint64_t idx, unsigned char *buf)
{
	uint64_t off = rp_blk_off(&j->h, idx), len = j->h.bsz;

	if (idx < j->h.nblk && off + len > j->h.plen) {
		len = j->h.plen - off;
		memset(buf + len, 0, j->h.bsz - len);
	}
	return (rp_pread(j->fd, off, buf, len) == (int64_t)len ? 0 : -1);
}

static int
rp_crc_ok(const rp_job_t *j, uint64_t i, uint32_t c)
{
	return ((j->have1[i] && j->t1[i] == c) || (j->have2[i] && j->t2[i] == c));
}

/*
 * Read one CRC table (n entries at off) in 4 KB pieces; a piece that
 * cannot be read leaves its entries marked missing in have.
 */
static void
rp_load_table(int fd, uint64_t off, uint64_t n, uint32_t *tab, unsigned char *have)
{
	unsigned char buf[4096];
	uint64_t i, e, cnt;

	for (i = 0; i < n; i += cnt) {
		cnt = n - i < sizeof (buf) / 4 ? n - i : sizeof (buf) / 4;
		if (rp_pread(fd, off + 4 * i, buf, 4 * cnt) != (int64_t)(4 * cnt)) {
			memset(have + i, 0, cnt);
			continue;
		}
		for (e = 0; e < cnt; e++) {
			tab[i + e] = (uint32_t)get_le(buf + 4 * e, 4);
			have[i + e] = 1;
		}
	}
}

/*
 * Pass 1: the CRC of every block, read 256 at a time, falling back to
 * one at a time around an I/O error.
 */
static int
rp_scan(rp_job_t *j)
{
	uint64_t i, n, b, end;
	unsigned char *buf;
	size_t bsz = j->h.bsz;

	if ((buf = pc_malloc(256 * bsz)) == NULL)
		return (-1);
	for (i = 0; i < j->ncrc; i += n) {
		/* A run of blocks contiguous in the file: stop at the data's end. */
		end = i < j->h.nblk ? j->h.nblk : j->ncrc;
		n = end - i < 256 ? end - i : 256;
		if (i + n == j->h.nblk && j->h.plen % bsz != 0 && n > 1)
			n--;	/* the partial last data block on its own */
		if (n > 1 && rp_pread(j->fd, rp_blk_off(&j->h, i), buf, n * bsz) ==
		    (int64_t)(n * bsz)) {
			for (b = 0; b < n; b++)
				j->crc[i + b] = lzma_crc32(buf + b * bsz, bsz, 0);
		} else {
			for (b = 0; b < n; b++) {
				if (rp_read_blk(j, i + b, buf) != 0) {
					j->bad[i + b] = 1;
					continue;
				}
				j->crc[i + b] = lzma_crc32(buf, bsz, 0);
			}
		}
		for (b = 0; b < n; b++) {
			if (!j->bad[i + b] && !rp_crc_ok(j, i + b, j->crc[i + b]))
				j->bad[i + b] = 1;
			j->nbad += j->bad[i + b];
		}
	}
	pc_mfree(buf);
	return (0);
}

/* Table index of block (group gi, row r, stripe s): data rows, then parity. */
static uint64_t
rp_stripe_idx(const rp_hdr_t *h, uint64_t gi, uint32_t r, uint32_t s)
{
	if (r < h->k)
		return (rp_blk(h, gi, r, s));
	return (h->nblk + gi * h->m * h->g + (uint64_t)(r - h->k) * h->g + s);
}

/* The stripe (gi, s) of table index idx. */
static void
rp_stripe_of(const rp_hdr_t *h, uint64_t idx, uint64_t *gi, uint32_t *s)
{
	if (idx < h->nblk) {
		*gi = idx / ((uint64_t)h->k * h->g);
		*s = (uint32_t)(idx % h->g);
	} else {
		*gi = (idx - h->nblk) / ((uint64_t)h->m * h->g);
		*s = (uint32_t)((idx - h->nblk) % h->g);
	}
}

/* Bad blocks of stripe (gi, s); data blocks past the file's end are good zeros. */
static uint32_t
rp_stripe_bad(const rp_job_t *j, uint64_t gi, uint32_t s, unsigned char *ok)
{
	uint32_t r, n = 0;
	uint64_t idx;

	for (r = 0; r < j->h.k + j->h.m; r++) {
		idx = rp_stripe_idx(&j->h, gi, r, s);
		ok[r] = (r < j->h.k && idx >= j->h.nblk) || !j->bad[idx];
		n += !ok[r];
	}
	return (n);
}

/*
 * Rebuild stripe (gi, s) in blk (k + m blocks of scratch) and write its
 * bad blocks back. A rebuilt block must match its CRC: a mismatch means
 * the table, not the block, is what is damaged beyond repair.
 */
static int
rp_fix_one(rp_job_t *j, uint64_t gi, uint32_t s, unsigned char *mem)
{
	unsigned char *blk[RP_STRIPE_MAX], ok[RP_STRIPE_MAX], was[RP_STRIPE_MAX];
	uint32_t r, n = j->h.k + j->h.m;
	uint64_t idx, off, len;

	(void) rp_stripe_bad(j, gi, s, ok);
	for (r = 0; r < n; r++) {
		blk[r] = mem + (size_t)r * j->h.bsz;
		was[r] = ok[r];
		idx = rp_stripe_idx(&j->h, gi, r, s);
		memset(blk[r], 0, j->h.bsz);
		if (ok[r] && idx < j->ncrc && !(r < j->h.k && idx >= j->h.nblk) &&
		    rp_read_blk(j, idx, blk[r]) != 0)
			return (-1);
	}
	if (rp_stripe_fix(blk, ok, j->h.k, j->h.m, j->h.bsz) != 0)
		return (-1);
	for (r = 0; r < n; r++) {
		if (was[r])
			continue;
		idx = rp_stripe_idx(&j->h, gi, r, s);
		j->crc[idx] = lzma_crc32(blk[r], j->h.bsz, 0);
		if ((j->have1[idx] || j->have2[idx]) && !rp_crc_ok(j, idx, j->crc[idx])) {
			log_msg(LOG_ERR, 0, "Rebuilt block %" PRIu64 " does not match its "
			    "checksum: the repair data is damaged too.", idx);
			return (-1);
		}
		off = rp_blk_off(&j->h, idx);
		len = idx < j->h.nblk && off + j->h.bsz > j->h.plen ? j->h.plen - off : j->h.bsz;
		if (rp_pwrite(j->fd, off, blk[r], len) != 0) {
			log_msg(LOG_ERR, 1, "Cannot write rebuilt block %" PRIu64 ": ", idx);
			return (-1);
		}
		j->bad[idx] = 0;
	}
	return (0);
}

/*
 * Pass 2: refuse if any stripe has more bad blocks than parity, then
 * rebuild each damaged stripe once. *nstripes counts them.
 */
static int
rp_fix(rp_job_t *j, uint64_t *nstripes)
{
	unsigned char ok[RP_STRIPE_MAX], *mem;
	uint64_t i, gi, lost = 0;
	uint32_t s;

	*nstripes = 0;
	for (i = 0; i < j->ncrc; i++) {
		if (!j->bad[i])
			continue;
		rp_stripe_of(&j->h, i, &gi, &s);
		if (rp_stripe_bad(j, gi, s, ok) > j->h.m)
			lost++;
	}
	if (lost > 0) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "%" PRIu64 " damaged blocks of %u bytes: too many to "
		    "repair (a stripe of %u blocks survives at most %u). Nothing was "
		    "changed.", j->nbad, j->h.bsz, j->h.k + j->h.m, j->h.m);
		return (-1);
	}
	if ((mem = pc_malloc((size_t)(j->h.k + j->h.m) * j->h.bsz)) == NULL)
		return (-1);
	for (i = 0; i < j->ncrc; i++) {
		if (!j->bad[i])
			continue;	/* cleared once its stripe is rebuilt */
		rp_stripe_of(&j->h, i, &gi, &s);
		if (rp_fix_one(j, gi, s, mem) != 0) {
			pc_mfree(mem);
			return (-1);
		}
		(*nstripes)++;
	}
	pc_mfree(mem);
	return (0);
}

/*
 * Find a good repair header for a file of fsize bytes: the copy before
 * the magic at the end, else the first copy, found by scanning back
 * from the end (it starts the region, which is at most about half the
 * file). A copy counts only if its region ends exactly at fsize.
 */
static int
rp_find_hdr(int fd, uint64_t fsize, rp_hdr_t *h)
{
	unsigned char tr[RP_TRAILER_SZ], *buf;
	uint64_t pos, n, i;
	const size_t win = 1 << 20;

	if (fsize < RP_TRAILER_SZ)
		return (-1);
	if (rp_pread(fd, fsize - RP_TRAILER_SZ, tr, RP_TRAILER_SZ) == RP_TRAILER_SZ &&
	    rp_hdr_parse(tr, h) == 0 && h->plen + h->rlen == fsize)
		return (0);
	if ((buf = pc_malloc(win + RP_HDR_SZ)) == NULL)
		return (-1);
	/* Windows overlap by a header, so no copy straddles two unseen. */
	for (pos = fsize; pos > 0; pos -= n) {
		n = pos < win ? pos : win;
		if (rp_pread(fd, pos - n, buf, n + RP_HDR_SZ) < 0)
			continue;
		for (i = n; i-- > 0; ) {
			if (buf[i] == 'P' && memcmp(buf + i, RP_HDR_MAGIC, 8) == 0 &&
			    rp_hdr_parse(buf + i, h) == 0 && h->plen == pos - n + i &&
			    h->plen + h->rlen == fsize) {
				pc_mfree(buf);
				return (0);
			}
		}
	}
	pc_mfree(buf);
	return (-1);
}

int
rp_present(int fd, uint64_t fsize)
{
	rp_hdr_t h;

	return (rp_find_hdr(fd, fsize, &h) == 0);
}

/* Set up j for fd: header, both CRC tables, and the per-block arrays. */
static int
rp_job_open(rp_job_t *j, int fd, uint64_t fsize)
{
	memset(j, 0, sizeof (*j));
	j->fd = fd;
	if (rp_find_hdr(fd, fsize, &j->h) != 0) {
		log_error_code(PC_ERR_FORMAT);
		log_msg(LOG_ERR, 0, "No usable repair data in this file (compress with -Y "
		    "to add it).");
		return (-1);
	}
	j->ncrc = j->h.nblk + j->h.npar;
	j->tab1 = j->h.plen + RP_HDR_SZ;
	j->tab2 = j->tab1 + 4 * j->ncrc + j->h.npar * j->h.bsz;
	j->t1 = pc_malloc(j->ncrc * 4);
	j->t2 = pc_malloc(j->ncrc * 4);
	j->crc = pc_calloc(j->ncrc, 4);
	j->have1 = pc_calloc(j->ncrc, 1);
	j->have2 = pc_calloc(j->ncrc, 1);
	j->bad = pc_calloc(j->ncrc, 1);
	if (j->t1 == NULL || j->t2 == NULL || j->crc == NULL || j->have1 == NULL ||
	    j->have2 == NULL || j->bad == NULL) {
		log_error_code(PC_ERR_NOMEM);
		log_msg(LOG_ERR, 0, "Out of memory checking repair data.");
		return (-1);
	}
	rp_load_table(fd, j->tab1, j->ncrc, j->t1, j->have1);
	rp_load_table(fd, j->tab2, j->ncrc, j->t2, j->have2);
	return (0);
}

static void
rp_job_free(rp_job_t *j)
{
	pc_mfree(j->t1);
	pc_mfree(j->t2);
	pc_mfree(j->crc);
	pc_mfree(j->have1);
	pc_mfree(j->have2);
	pc_mfree(j->bad);
}

/*
 * Damage outside the blocks: a header copy, the magic, or a CRC table
 * entry (each has a second copy). Returns how many of those differ from
 * what they should be once the blocks are good.
 */
static uint64_t
rp_meta_damage(rp_job_t *j)
{
	unsigned char hb[RP_TRAILER_SZ];
	uint64_t i, n = 0, pend = j->tab2 + 4 * j->ncrc;

	if (rp_pread(j->fd, j->h.plen, hb, RP_HDR_SZ) != RP_HDR_SZ ||
	    memcmp(hb, j->h.raw, RP_HDR_SZ) != 0)
		n++;
	if (rp_pread(j->fd, pend, hb, RP_TRAILER_SZ) != RP_TRAILER_SZ ||
	    memcmp(hb, j->h.raw, RP_HDR_SZ) != 0 ||
	    memcmp(hb + RP_HDR_SZ, RP_TAIL_MAGIC, 8) != 0)
		n++;
	for (i = 0; i < j->ncrc; i++)
		n += (!j->have1[i] || j->t1[i] != j->crc[i]) +
		    (!j->have2[i] || j->t2[i] != j->crc[i]);
	return (n);
}

/* Rewrite both header copies, the magic and both CRC tables from j. */
static int
rp_rewrite_meta(rp_job_t *j)
{
	uint64_t i, pend = j->tab2 + 4 * j->ncrc;
	unsigned char *ct;
	int rv = -1;

	if ((ct = pc_malloc(j->ncrc * 4)) == NULL)
		return (-1);
	for (i = 0; i < j->ncrc; i++)
		put_le(ct + 4 * i, j->crc[i], 4);
	if (rp_pwrite(j->fd, j->h.plen, j->h.raw, RP_HDR_SZ) == 0 &&
	    rp_pwrite(j->fd, j->tab1, ct, j->ncrc * 4) == 0 &&
	    rp_pwrite(j->fd, j->tab2, ct, j->ncrc * 4) == 0 &&
	    rp_pwrite(j->fd, pend, j->h.raw, RP_HDR_SZ) == 0 &&
	    rp_pwrite(j->fd, pend + RP_HDR_SZ, RP_TAIL_MAGIC, 8) == 0)
		rv = 0;
	pc_mfree(ct);
	return (rv);
}

/*
 * Copy len bytes of in to out (both from offset 0). A read error is not
 * fatal: the unreadable 4 KB is written as zeros, and the repair, which
 * found it bad in the scan, rebuilds it.
 */
static int
rp_copy(int in, int out, uint64_t len)
{
	unsigned char *buf;
	uint64_t off, n, b;
	const uint64_t win = 1 << 20, sec = 4096;
	int rv = 0;

	if ((buf = pc_malloc(win)) == NULL)
		return (-1);
	for (off = 0; off < len && rv == 0; off += n) {
		n = len - off < win ? len - off : win;
		if (rp_pread(in, off, buf, n) != (int64_t)n) {
			for (b = 0; b < n; b += sec)
				(void) rp_pread(in, off + b, buf + b, n - b < sec ? n - b : sec);
		}
		rv = rp_pwrite(out, off, buf, n);
	}
	pc_mfree(buf);
	return (rv);
}

/*
 * Make the repaired file: a copy of path in its directory (or out),
 * fixed in place, scanned again, and renamed over path. Returns the
 * open copy's descriptor in *fdp and its name in tmp.
 */
static int
rp_make_copy(rp_job_t *j, const char *path, const char *out, char *tmp, size_t tmpsz,
    uint64_t fsize, int *fdp)
{
	struct stat sb;
	char dir[PATH_MAX];
	int fd;

	if (out != NULL) {
		snprintf(tmp, tmpsz, "%s", out);
		fd = open(out, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	} else {
		snprintf(dir, sizeof (dir), "%s", path);
		snprintf(tmp, tmpsz, "%s/.pcrepairXXXXXX", dirname(dir));
		fd = mkstemp(tmp);
	}
	if (fd == -1) {
		log_msg(LOG_ERR, 1, "Cannot create %s: ", tmp);
		return (-1);
	}
	add_fname(tmp);
	*fdp = fd;
	if (fstat(j->fd, &sb) == 0)
		(void) fchmod(fd, sb.st_mode & 07777);
	if (rp_copy(j->fd, fd, fsize) != 0) {
		log_msg(LOG_ERR, 1, "Cannot copy %s: ", path);
		return (-1);
	}
	return (0);
}

/* Fix the copy at fd, check it again, and rewrite the region's own fields. */
static int
rp_fix_copy(rp_job_t *j, int fd)
{
	uint64_t nstripes, before = j->nbad;

	j->fd = fd;
	if (rp_fix(j, &nstripes) != 0)
		return (-1);
	memset(j->bad, 0, j->ncrc);
	j->nbad = 0;
	if (rp_scan(j) != 0 || j->nbad != 0) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Blocks still damaged after repair.");
		return (-1);
	}
	if (rp_rewrite_meta(j) != 0 || fsync(fd) != 0) {
		log_msg(LOG_ERR, 1, "Cannot write the repaired file: ");
		return (-1);
	}
	log_msg(LOG_WARN, 0, "Repaired %" PRIu64 " damaged blocks of %u bytes in %" PRIu64
	    " stripes.", before, j->h.bsz, nstripes);
	return (0);
}

int
rp_repair(const char *path, const char *out)
{
	char tmp[PATH_MAX];
	struct stat sb;
	rp_job_t j;
	uint64_t meta;
	int fd, ofd = -1, rv = -1;

	tmp[0] = '\0';
	if ((fd = open(path, O_RDONLY, 0)) == -1 || fstat(fd, &sb) != 0) {
		log_msg(LOG_ERR, 1, "Cannot open %s: ", path);
		if (fd != -1)
			close(fd);
		return (-1);
	}
	if (rp_job_open(&j, fd, (uint64_t)sb.st_size) != 0 || rp_scan(&j) != 0)
		goto done;
	meta = rp_meta_damage(&j);
	if (j.nbad == 0 && meta == 0 && out == NULL) {
		log_msg(LOG_INFO, 0, "%s: no damage found (%" PRIu64 " blocks checked).",
		    path, j.ncrc);
		rv = 0;
		goto done;
	}
	if (rp_make_copy(&j, path, out, tmp, sizeof (tmp), (uint64_t)sb.st_size, &ofd) != 0 ||
	    rp_fix_copy(&j, ofd) != 0)
		goto done;
	/* Closed first: Windows does not rename over a file held open. */
	close(ofd);
	close(fd);
	fd = -1;
	if (out == NULL && rename(tmp, path) != 0) {
		log_msg(LOG_ERR, 1, "Cannot replace %s: ", path);
		unlink(tmp);
	} else {
		rv = 0;
	}
	rm_fname(tmp);
	ofd = -1;
done:
	if (ofd != -1) {
		close(ofd);
		unlink(tmp);
		rm_fname(tmp);
	}
	rp_job_free(&j);
	if (fd != -1)
		close(fd);
	return (rv);
}
