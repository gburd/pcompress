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
 * Raw deflate with the zlib 1.3.1 bundled in filters/reflate/zlib, and
 * nothing else from it. The reflate filter keeps a deflate stream in
 * inflated form only if this code reproduces it bit for bit, and must
 * reproduce it again at extraction, on any machine, years later: so it
 * cannot use the system zlib, whose deflate output may change between
 * versions (zlib-ng's always differs). The bundled copy is compiled with
 * every symbol prefixed pcz_ (zconf.h), so it never meets the system's.
 * It is portable C with no CPU-specific paths, so its output is the
 * same on every target. Do not upgrade it: a new zlib would make some
 * stored members fail to re-deflate. A newer one goes beside it, chosen
 * by a new parameter value.
 */

#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <pc_alloc.h>
#include "zlib/zlib.h"
#include "rf_deflate.h"

static voidpf
rf_alloc(voidpf opaque, uInt items, uInt size)
{
	(void) opaque;
	if (size != 0 && items > SIZE_MAX / size)
		return (NULL);
	return (pc_malloc((size_t)items * size));	/* zlib needs no zeroing */
}

static void
rf_free(voidpf opaque, voidpf p)
{
	(void) opaque;
	pc_mfree(p);
}

/*
 * Set up a raw deflate stream with parameters p. 0 or -1 (bad
 * parameters, no memory).
 */
static int
rf_init(z_stream *zs, const rf_params_t *p)
{
	zs->zalloc = rf_alloc;
	zs->zfree = rf_free;
	zs->opaque = NULL;
	return (deflateInit2_(zs, p->level, Z_DEFLATED, -(int)p->wbits, p->memlevel,
	    p->strategy, ZLIB_VERSION, (int)sizeof (z_stream)) == Z_OK ? 0 : -1);
}

int
rf_deflate(const unsigned char *raw, size_t rawlen, unsigned char *dst, size_t cap,
    const rf_params_t *p, size_t *outlen)
{
	z_stream zs;
	int rv;

	if (rawlen > UINT32_MAX || cap > UINT32_MAX || rf_init(&zs, p) != 0)
		return (-1);
	zs.next_in = (Bytef *)raw;
	zs.avail_in = (uInt)rawlen;
	zs.next_out = dst;
	zs.avail_out = (uInt)cap;
	rv = deflate(&zs, Z_FINISH);
	*outlen = cap - zs.avail_out;
	/*
	 * Output that exactly fills dst can leave deflate() returning Z_OK
	 * with only its end-of-stream state pending: one more call with
	 * spare room must end the stream without writing anything.
	 */
	if (rv == Z_OK && zs.avail_out == 0) {
		unsigned char spare[16];

		zs.next_out = spare;
		zs.avail_out = sizeof (spare);
		rv = deflate(&zs, Z_FINISH);
		if (zs.avail_out != sizeof (spare))
			rv = Z_BUF_ERROR;	/* more than cap bytes: not this stream */
	}
	(void) deflateEnd(&zs);
	return (rv == Z_STREAM_END ? 0 : -1);
}

int
rf_deflate_match(const unsigned char *raw, size_t rawlen, const unsigned char *comp,
    size_t clen, const rf_params_t *p)
{
	unsigned char buf[65536];
	size_t done = 0, n, step = 4096, fed = 0;
	z_stream zs;
	int rv = Z_OK, flush;

	if (rawlen > UINT32_MAX || rf_init(&zs, p) != 0)
		return (0);
	/*
	 * Feed the input in steps and compare what comes out as it comes.
	 * Nearly every wrong parameter set differs in its first output, so
	 * the first step is 4 KB and a miss costs that much deflating, not
	 * the whole stream. deflate() emits nothing until a block is done,
	 * so small or uniform streams still run to their first block.
	 */
	while (rv == Z_OK) {
		n = rawlen - fed < step ? rawlen - fed : step;
		step = 65536;	/* the first step is small: a miss shows early */
		zs.next_in = (Bytef *)raw + fed;
		zs.avail_in = (uInt)n;
		fed += n;
		flush = fed == rawlen ? Z_FINISH : Z_NO_FLUSH;
		do {
			zs.next_out = buf;
			zs.avail_out = sizeof (buf);
			rv = deflate(&zs, flush);
			n = sizeof (buf) - zs.avail_out;
			if (done + n > clen || memcmp(buf, comp + done, n) != 0) {
				(void) deflateEnd(&zs);
				return (0);
			}
			done += n;
		} while (zs.avail_out == 0 && rv == Z_OK);
		/* Out of input before the end: feed more. At the end, a stall fails. */
		if (rv == Z_BUF_ERROR && flush == Z_NO_FLUSH)
			rv = Z_OK;
	}
	(void) deflateEnd(&zs);
	return (rv == Z_STREAM_END && done == clen);
}
