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
 * The pinned raw deflate the reflate filter re-creates streams with
 * (rf_deflate.c). No zlib types here: reflate.c uses the system zlib to
 * inflate, and the two zlib.h files cannot meet in one file.
 */

#ifndef	_RF_DEFLATE_H
#define	_RF_DEFLATE_H

#include <stddef.h>

/* zlib's deflateInit2() parameters, as stored in a reflate record. */
typedef struct {
	int level;	/* 1 to 9 */
	int memlevel;	/* 1 to 9 */
	int wbits;	/* window bits, 9 to 15 */
	int strategy;	/* 0 default, 1 filtered */
} rf_params_t;

/*
 * Deflate rawlen bytes at raw with p into dst (cap bytes); *outlen is
 * what was written. 0 when the stream is complete within cap, else -1.
 */
int rf_deflate(const unsigned char *raw, size_t rawlen, unsigned char *dst, size_t cap,
    const rf_params_t *p, size_t *outlen);

/*
 * 1 if deflating raw with p gives exactly the clen bytes at comp, else
 * 0. Stops at the first byte that differs.
 */
int rf_deflate_match(const unsigned char *raw, size_t rawlen, const unsigned char *comp,
    size_t clen, const rf_params_t *p);

#endif
