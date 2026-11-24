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
 * Repair data for .pz files (-Y, -y): Reed-Solomon parity over the bytes
 * of the file, appended as the last region of the file. DESIGN.md
 * ("Repair data") has the layout and the argument for it.
 */

#ifndef	_REPAIR_H
#define	_REPAIR_H

#include <sys/types.h>
#include <stdint.h>
#include <pz_crypt.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	RP_HDR_MAGIC	"PCRPAIR1"	/* first 8 bytes of each header copy */
#define	RP_TAIL_MAGIC	"PCREPR01"	/* last 8 bytes of the file */
#define	RP_HDR_SZ	80		/* fields 48, hash 16 (of fields and tag), tag 16 */
#define	RP_TRAILER_SZ	(RP_HDR_SZ + 8)	/* header copy, then the magic */
#define	RP_BLOCK	4096		/* block size the writer uses */
#define	RP_PCT_MAX	50		/* -Y bound */

/* Format limits, checked on every header read. */
#define	RP_BLOCK_MIN	512
#define	RP_BLOCK_MAX	65536
#define	RP_STRIPE_MAX	255		/* k + m: GF(2^8) Cauchy points */
#define	RP_G_MAX	256		/* stripes per group */
#define	RP_GROUP_PARITY_MAX	(64ULL << 20)	/* m * G * B */

/* A parsed repair header: the geometry of the region. */
typedef struct {
	uint64_t plen;		/* protected bytes: the file before the region */
	uint32_t bsz;		/* block size B */
	uint32_t k, m;		/* data and parity blocks per stripe */
	uint32_t g;		/* stripes per group, the interleave */
	uint64_t rlen;		/* region bytes, header to trailer magic */
	uint64_t nblk;		/* data blocks: plen / B, rounded up */
	uint64_t ngroups;	/* nblk / (k * g), rounded up */
	uint64_t npar;		/* parity blocks: ngroups * m * g */
	unsigned char raw[RP_HDR_SZ];	/* as stored, tag included */
} rp_hdr_t;

/*
 * Append a repair region for bytes [0, plen) of fd, which must be open
 * for reading and writing and positioned at plen (nothing after it).
 * ilen is how many of those bytes follow the file's trailer (the end
 * marker, and in version 12 the end record): the seek index, or 0. pct
 * is the parity size in percent of the data (1 to RP_PCT_MAX). For a
 * version 12 file, a and end are its key and end record: the header is
 * then sealed as AEAD stream 5; both NULL otherwise. 0 or -1 (logged).
 */
int rp_write(int fd, uint64_t plen, uint64_t ilen, int pct, const pz_aead_t *a,
    const unsigned char *end);

/*
 * Check the repair data of the file at path and, if anything is damaged
 * and it can be rebuilt, write the repaired file to out (NULL: replace
 * path, through a temporary file and rename). 0 if the file is intact
 * or was repaired, -1 if it has no repair data, cannot be repaired, or
 * an I/O error occurred (logged; nothing is replaced then).
 */
int rp_repair(const char *path, const char *out);

/*
 * Parse a repair header (RP_HDR_SZ bytes at hdr): magic, hash, flags and
 * every size checked. 0 if it is well formed, -1 if not.
 */
int rp_hdr_parse(const unsigned char *hdr, rp_hdr_t *h);

/*
 * Decompression of a version 12 file, after the end record (end): tr is
 * the last RP_TRAILER_SZ bytes of the file and tail the number of bytes
 * after the end record. 0 if tr is the trailer of a repair region of at
 * most tail bytes sealed under a for this end record; *rlen is then the
 * region's length.
 */
int rp_tail_ok(const unsigned char *tr, uint64_t tail, const pz_aead_t *a,
    const unsigned char *end, uint64_t *rlen);

/*
 * Where the bytes before a repair region end: fsize, or the protected
 * length if fd's last bytes are a well-formed repair trailer (the seek
 * index is found before it).
 */
uint64_t rp_logical_size(int fd, uint64_t fsize);

/*
 * Whether fd (fsize bytes) has a repair region, found from either copy
 * of its header: for the hint after a failed decompression, so it may
 * read the whole file when the last copy is damaged.
 */
int rp_present(int fd, uint64_t fsize);

/* GF(2^8), polynomial 0x11d: exposed for the unit tests. */
unsigned char rp_gf_mul(unsigned char a, unsigned char b);
unsigned char rp_gf_inv(unsigned char a);
/* dst ^= c * src over len bytes; the vector paths when the CPU has them. */
void rp_muladd(unsigned char *dst, const unsigned char *src, unsigned char c, size_t len);
/* The same, portable C only. */
void rp_muladd_scalar(unsigned char *dst, const unsigned char *src, unsigned char c,
    size_t len);
/* Coefficient of data row r in parity row i of a stripe with m parity rows. */
unsigned char rp_coef(uint32_t i, uint32_t r, uint32_t m);
/*
 * Rebuild the blocks of one stripe whose ok[] is 0: blk[0..k) data,
 * blk[k..k+m) parity, len bytes each. Data blocks are rebuilt from good
 * parity, then bad parity from the data. 0, or -1 if more blocks are bad
 * than there are good parity blocks.
 */
int rp_stripe_fix(unsigned char **blk, unsigned char *ok, uint32_t k, uint32_t m, size_t len);

#ifdef	__cplusplus
}
#endif

#endif
