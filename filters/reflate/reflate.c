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
 * The reflate filter's record: the member with each kept deflate stream
 * replaced by its inflated bytes, and a table to put them back.
 *
 *   "PCRF"  u8 version (1)  u8 zero  u16 nseg  u64 original length
 *   u64 checksum: XXH64-like digest of the original (see rf_sum())
 *   nseg x { u64 offset in the original, u64 deflated length,
 *            u64 inflated length, u8 level, u8 memlevel, u8 wbits,
 *            u8 strategy }
 *   the original with each stream's bytes replaced by its inflated
 *   bytes, in order
 *
 * Little-endian. Offsets increase and streams do not overlap.
 *
 * Streams are found by structure, never by scanning for deflate data:
 * gzip members (from the start of a file), zip local file entries with
 * method 8 (walked from the start; one that does not parse ends the
 * walk), and the IDAT data of a PNG when it is one zlib stream in one
 * IDAT chunk. A stream is kept only if the system zlib inflates it, its
 * end is where the container says, and the pinned deflate
 * (rf_deflate.c) reproduces it with one of the parameter sets tried.
 * The whole record is then decoded again and compared before it is
 * used, so what is stored always extracts to the original.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <utils.h>
#include <pc_alloc.h>
#include "rf_deflate.h"
#include "reflate.h"

#define	RF_VERSION	1
#define	RF_MIN_STREAM	256		/* smaller streams are left alone */
#define	RF_MAX_SEG	65535
#ifndef RF_MIN_PCT
#define	RF_MIN_PCT	0
#endif

typedef struct {
	uint64_t off, clen, rlen;
	rf_params_t p;
	unsigned char *raw;	/* inflated bytes, pc_malloc() */
} rf_seg_t;

typedef struct {
	rf_seg_t *seg;
	size_t n, cap;
	uint64_t rawtotal;
	uint64_t budget;	/* bytes the member's streams may still inflate to */
	rf_params_t last;	/* the set that matched last: tried first */
} rf_list_t;

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
 * A 64-bit check of the original bytes: FNV-1a over 8-byte words, then
 * the tail. Not a cryptographic hash: it catches a re-deflate that
 * differs (a bug, or a record from a build with another deflate), and
 * the chunk checksums of the .pz already cover tampering.
 */
static uint64_t
rf_sum(const unsigned char *p, size_t len)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	size_t i;

	for (i = 0; i + 8 <= len; i += 8)
		h = (h ^ get_le(p + i, 8)) * 0x100000001b3ULL;
	for (; i < len; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return (h ^ len);
}

/*
 * Inflate the raw deflate stream at in (at most avail bytes) with the
 * system zlib. On success *raw is the output (pc_malloc()), *rlen its
 * length and *clen the bytes the stream took. The output is capped at
 * *budget, which is reduced by what was inflated, kept or not: so a
 * member of many deflate bombs costs at most RF_MAX_OUT in all.
 */
static int
rf_inflate(const unsigned char *in, size_t avail, int wbits, unsigned char **raw,
    uint64_t *rlen, uint64_t *clen, uint64_t *budget)
{
	size_t cap = avail * 4 + 4096, n, max = *budget;
	unsigned char *buf = NULL, *nb;
	z_stream zs;
	int rv;

	if (max == 0)
		return (-1);
	memset(&zs, 0, sizeof (zs));
	if (inflateInit2(&zs, -wbits) != Z_OK)
		return (-1);
	zs.next_in = (Bytef *)in;
	zs.avail_in = avail > UINT32_MAX ? UINT32_MAX : (uInt)avail;
	n = 0;
	do {
		if (cap > max)
			cap = max;
		if ((nb = pc_realloc(buf, cap)) == NULL)
			break;
		buf = nb;
		zs.next_out = buf + n;
		zs.avail_out = (uInt)(cap - n);
		rv = inflate(&zs, Z_NO_FLUSH);
		n = cap - zs.avail_out;
		if (rv == Z_STREAM_END) {
			inflateEnd(&zs);
			*budget -= n;
			*raw = buf;
			*rlen = n;
			*clen = avail - zs.avail_in;
			return (0);
		}
		cap *= 2;
	} while (rv == Z_OK && n < max);
	inflateEnd(&zs);
	*budget -= n;
	pc_mfree(buf);
	return (-1);
}

/*
 * The parameter sets tried, in order: (level, memLevel, strategy), all
 * with the window the container gives. These are every set that matched
 * a stream of the measured corpus (jar, odt, docx, xlsx, gzip, PNG; see
 * DESIGN.md), most frequent first: zlib's default (6, 8) made 90% of
 * them, then gzip -9 style (9, 8), fast (1, 8), and libpng's filtered
 * default. Whether a stream is kept depends only on its own bytes, never
 * on the member's other streams, so identical parts of two files (the
 * styles and themes of documents) are stored alike and still match each
 * other in the compressor.
 */
static const signed char rf_sets[][3] = {
	{ 6, 8, 0 }, { 9, 8, 0 }, { 1, 8, 0 }, { 6, 9, 0 }, { 9, 9, 0 }, { 1, 9, 0 },
	{ 4, 8, 0 }, { 6, 8, 1 }
};

static int
rf_find_params(const unsigned char *raw, uint64_t rlen, const unsigned char *comp,
    uint64_t clen, int wbits, rf_list_t *l)
{
	rf_params_t p;
	size_t i;

	p = l->last;
	p.wbits = wbits;
	if (p.level != 0 && rf_deflate_match(raw, rlen, comp, clen, &p)) {
		l->last = p;	/* this stream's window, which rf_try() records */
		return (0);
	}
	for (i = 0; i < sizeof (rf_sets) / sizeof (rf_sets[0]); i++) {
		p.level = rf_sets[i][0];
		p.memlevel = rf_sets[i][1];
		p.strategy = rf_sets[i][2];
		p.wbits = wbits;
		if (rf_deflate_match(raw, rlen, comp, clen, &p)) {
			l->last = p;
			return (0);
		}
	}
	return (-1);
}

/*
 * Try the deflate stream at off of in (len bytes in all), which the
 * container says is clen bytes long (0: unknown, take what inflate
 * uses). Adds it to l if it is kept. Returns the bytes the stream
 * takes whether or not it is kept, so a container walk can step over
 * it, or 0 if it does not inflate (or ends elsewhere than clen says).
 */
static uint64_t
rf_try(const unsigned char *in, size_t len, uint64_t off, uint64_t clen, int wbits,
    rf_list_t *l)
{
	uint64_t rlen, used;
	unsigned char *raw;
	rf_seg_t *s;

	if (off >= len || (clen != 0 && clen > len - off))
		return (0);
	if (rf_inflate(in + off, clen ? clen : len - off, wbits, &raw, &rlen, &used,
	    &l->budget) != 0)
		return (0);
	if (clen != 0 && used != clen) {
		pc_mfree(raw);
		return (0);
	}
	if (used < RF_MIN_STREAM || l->n >= RF_MAX_SEG ||
	    rf_find_params(raw, rlen, in + off, used, wbits, l) != 0) {
		pc_mfree(raw);
		return (used);
	}
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 16;
		rf_seg_t *ns = pc_realloc(l->seg, cap * sizeof (*ns));

		if (ns == NULL) {
			pc_mfree(raw);
			return (used);
		}
		l->seg = ns;
		l->cap = cap;
	}
	s = &l->seg[l->n++];
	s->off = off;
	s->clen = used;
	s->rlen = rlen;
	s->p = l->last;
	s->raw = raw;
	l->rawtotal += rlen;
	return (used);
}

/* gzip members from offset 0 (RFC 1952), each with its 8-byte trailer. */
static void
rf_scan_gzip(const unsigned char *in, size_t len, rf_list_t *l)
{
	uint64_t pos = 0, p, used;
	int flg;

	while (pos + 18 <= len && in[pos] == 0x1f && in[pos + 1] == 0x8b && in[pos + 2] == 8) {
		flg = in[pos + 3];
		if (flg & 0xe0)
			return;
		p = pos + 10;
		if (flg & 4) {
			if (p + 2 > len)
				return;
			p += 2 + get_le(in + p, 2);
		}
		if (flg & 8)
			while (p < len && in[p++] != 0)
				;
		if (flg & 16)
			while (p < len && in[p++] != 0)
				;
		if (flg & 2)
			p += 2;
		if (p >= len || (used = rf_try(in, len, p, 0, 15, l)) == 0)
			return;
		pos = p + used + 8;
	}
}

/*
 * Zip local file entries from offset 0 (APPNOTE 4.3.7). An entry with
 * a data descriptor (flag bit 3) has no sizes in its header: inflate
 * finds the end. Encrypted entries (bit 0) are skipped over when their
 * size is known; the walk stops at anything else it cannot follow.
 */
static void
rf_scan_zip(const unsigned char *in, size_t len, rf_list_t *l)
{
	uint64_t pos = 0, data, csize, used;
	unsigned int gp, meth, nl, xl;

	while (pos + 30 <= len && memcmp(in + pos, "PK\003\004", 4) == 0) {
		gp = (unsigned int)get_le(in + pos + 6, 2);
		meth = (unsigned int)get_le(in + pos + 8, 2);
		csize = get_le(in + pos + 18, 4);
		nl = (unsigned int)get_le(in + pos + 26, 2);
		xl = (unsigned int)get_le(in + pos + 28, 2);
		data = pos + 30 + nl + xl;
		if (data > len || csize == 0xffffffffULL)
			return;		/* past the end, or zip64 */
		if (gp & 8) {
			if (meth != 8 || (gp & 1) ||
			    (used = rf_try(in, len, data, 0, 15, l)) == 0)
				return;
			pos = data + used;
			/* The descriptor: optional signature, crc, sizes. */
			if (pos + 4 <= len && memcmp(in + pos, "PK\007\010", 4) == 0)
				pos += 4;
			pos += 12;
			continue;
		}
		if (csize > len - data)
			return;
		if (meth == 8 && !(gp & 1))
			(void) rf_try(in, len, data, csize, 15, l);
		pos = data + csize;
	}
}

/*
 * A PNG whose image data is one IDAT chunk holding one zlib stream
 * (RFC 1950 header, then deflate, then Adler-32). Images split over
 * several IDAT chunks are left alone: the splits are another encoder
 * choice to record.
 */
static void
rf_scan_png(const unsigned char *in, size_t len, rf_list_t *l)
{
	uint64_t pos = 8, n;
	int nidat = 0;
	uint64_t ioff = 0, ilen = 0;

	while (pos + 12 <= len) {
		n = (uint64_t)in[pos] << 24 | in[pos + 1] << 16 | in[pos + 2] << 8 | in[pos + 3];
		if (n > len - pos - 12)
			return;
		if (memcmp(in + pos + 4, "IDAT", 4) == 0) {
			nidat++;
			ioff = pos + 8;
			ilen = n;
		}
		pos += 12 + n;
	}
	if (nidat != 1 || ilen < 6 || (in[ioff] & 0x0f) != 8 ||
	    ((in[ioff] << 8) | in[ioff + 1]) % 31 != 0 || (in[ioff + 1] & 0x20))
		return;
	(void) rf_try(in, len, ioff + 2, ilen - 6, (in[ioff] >> 4) + 8, l);
}

static void
rf_list_free(rf_list_t *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		pc_mfree(l->seg[i].raw);
	pc_mfree(l->seg);
}

/* Lay out the record for l, from in (len bytes). NULL if out of memory. */
static unsigned char *
rf_build(const unsigned char *in, size_t len, const rf_list_t *l, size_t *olen)
{
	uint64_t gaps = len, total, pos = 0;
	unsigned char *o, *q;
	size_t i;

	for (i = 0; i < l->n; i++)
		gaps -= l->seg[i].clen;
	total = RF_HDR_SZ + RF_SEG_SZ * l->n + gaps + l->rawtotal;
	if (total > RF_MAX_OUT || (o = pc_malloc(total)) == NULL)
		return (NULL);
	memcpy(o, RF_MAGIC, 4);
	o[4] = RF_VERSION;
	o[5] = 0;
	put_le(o + 6, l->n, 2);
	put_le(o + 8, len, 8);
	put_le(o + 16, rf_sum(in, len), 8);
	q = o + RF_HDR_SZ;
	for (i = 0; i < l->n; i++, q += RF_SEG_SZ) {
		const rf_seg_t *s = &l->seg[i];

		put_le(q, s->off, 8);
		put_le(q + 8, s->clen, 8);
		put_le(q + 16, s->rlen, 8);
		q[24] = (unsigned char)s->p.level;
		q[25] = (unsigned char)s->p.memlevel;
		q[26] = (unsigned char)s->p.wbits;
		q[27] = (unsigned char)s->p.strategy;
	}
	for (i = 0; i < l->n; i++) {
		memcpy(q, in + pos, l->seg[i].off - pos);
		q += l->seg[i].off - pos;
		memcpy(q, l->seg[i].raw, l->seg[i].rlen);
		q += l->seg[i].rlen;
		pos = l->seg[i].off + l->seg[i].clen;
	}
	memcpy(q, in + pos, len - pos);
	*olen = total;
	return (o);
}

size_t
rf_encode(const unsigned char *in, size_t len, unsigned char **out)
{
	unsigned char *rec, *back = NULL;
	rf_list_t l;
	size_t rlen = 0, blen, i;
	uint64_t kept;

	*out = NULL;
	if (len < 64 || len > RF_MAX_IN)
		return (0);
	memset(&l, 0, sizeof (l));
	l.budget = RF_MAX_OUT;
	if (in[0] == 0x1f && in[1] == 0x8b)
		rf_scan_gzip(in, len, &l);
	else if (memcmp(in, "PK\003\004", 4) == 0)
		rf_scan_zip(in, len, &l);
	else if (memcmp(in, "\211PNG\r\n\032\n", 8) == 0)
		rf_scan_png(in, len, &l);
	for (i = 0, kept = 0; i < l.n; i++)
		kept += l.seg[i].clen;
	if (l.n == 0 || kept * 100 < (uint64_t)len * RF_MIN_PCT) {
		rf_list_free(&l);
		return (0);
	}
	rec = rf_build(in, len, &l, &rlen);
	rf_list_free(&l);
	if (rec == NULL)
		return (0);
	/* What is stored must extract: decode it now and compare. */
	blen = rf_decode(rec, rlen, &back);
	if (blen != len || memcmp(back, in, len) != 0) {
		pc_mfree(rec);
		rlen = 0;
		rec = NULL;
	}
	pc_mfree(back);
	*out = rec;
	return (rlen);
}

static void
rf_seg_params(const unsigned char *q, rf_params_t *p)
{
	p->level = q[24];
	p->memlevel = q[25];
	p->wbits = q[26];
	p->strategy = q[27];
}

/* Check one table entry against what precedes it. 0 if usable. */
static int
rf_seg_ok(const unsigned char *q, uint64_t olen, uint64_t *pos, uint64_t *rawsum,
    rf_params_t *p)
{
	uint64_t off = get_le(q, 8), clen = get_le(q + 8, 8), rlen = get_le(q + 16, 8);

	rf_seg_params(q, p);
	if (off < *pos || clen == 0 || off > olen || clen > olen - off ||
	    rlen > RF_MAX_OUT || *rawsum + rlen > RF_MAX_OUT ||
	    p->level < 1 || p->level > 9 || p->memlevel < 1 || p->memlevel > 9 ||
	    p->wbits < 9 || p->wbits > 15 || p->strategy > 1)
		return (-1);
	*pos = off + clen;
	*rawsum += rlen;
	return (0);
}

size_t
rf_decode(const unsigned char *in, size_t len, unsigned char **out)
{
	uint64_t olen, nseg, i, pos = 0, rawsum = 0, src, clen, rlen, off;
	unsigned char *o = NULL;
	const unsigned char *q;
	rf_params_t p;
	size_t got;

	*out = NULL;
	if (len < RF_HDR_SZ || memcmp(in, RF_MAGIC, 4) != 0 || in[4] != RF_VERSION ||
	    in[5] != 0)
		return (0);
	nseg = get_le(in + 6, 2);
	olen = get_le(in + 8, 8);
	if (nseg == 0 || olen == 0 || olen > RF_MAX_IN || len < RF_HDR_SZ + RF_SEG_SZ * nseg)
		return (0);
	for (i = 0, q = in + RF_HDR_SZ; i < nseg; i++, q += RF_SEG_SZ)
		if (rf_seg_ok(q, olen, &pos, &rawsum, &p) != 0)
			return (0);
	/* The body is the gaps plus the inflated streams, exactly. */
	src = RF_HDR_SZ + RF_SEG_SZ * nseg;
	for (i = 0, q = in + RF_HDR_SZ, clen = 0; i < nseg; i++, q += RF_SEG_SZ)
		clen += get_le(q + 8, 8);
	if (len - src != olen - clen + rawsum || (o = pc_malloc(olen)) == NULL)
		return (0);
	pos = 0;
	for (i = 0, q = in + RF_HDR_SZ; i < nseg; i++, q += RF_SEG_SZ) {
		off = get_le(q, 8);
		clen = get_le(q + 8, 8);
		rlen = get_le(q + 16, 8);
		rf_seg_params(q, &p);
		memcpy(o + pos, in + src, off - pos);
		src += off - pos;
		if (rf_deflate(in + src, rlen, o + off, clen, &p, &got) != 0 || got != clen) {
			pc_mfree(o);
			return (0);
		}
		src += rlen;
		pos = off + clen;
	}
	memcpy(o + pos, in + src, olen - pos);
	if (rf_sum(o, olen) != get_le(in + 16, 8)) {
		pc_mfree(o);
		return (0);
	}
	*out = o;
	return (olen);
}
