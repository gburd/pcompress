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
 * Reflate (-Z): store an archive member's deflate streams inflated, so
 * the compressor and dedupe see the data inside zip containers (jar,
 * odt, docx, xlsx, apk) and gzip files, and re-deflate them bit for bit
 * on extraction. DESIGN.md ("Reflate") has the record layout.
 */

#ifndef	_REFLATE_H
#define	_REFLATE_H

#include <stddef.h>
#include <stdint.h>

#define	RF_MAGIC	"PCRF"
#define	RF_MAX_IN	(64ULL << 20)	/* members up to this size are tried */
#define	RF_MAX_OUT	(256ULL << 20)	/* and their record up to this */

/*
 * Encode the len bytes at in: find deflate streams, keep those the
 * pinned deflate reproduces, and return the record in *out (pc_malloc()
 * memory) and its length, or 0 if no stream was kept (store the member
 * as it is). Never fails the member: any problem returns 0.
 */
size_t rf_encode(const unsigned char *in, size_t len, unsigned char **out);

/*
 * Decode a record of len bytes at in into *out (pc_malloc() memory),
 * returning the original length, or 0 if the record is malformed or
 * does not rebuild to the checksum it carries. Hostile input: every
 * field is checked before use.
 */
size_t rf_decode(const unsigned char *in, size_t len, unsigned char **out);

/* Bytes of record header and segment table for nseg streams. */
#define	RF_HDR_SZ	24
#define	RF_SEG_SZ	28

#endif
