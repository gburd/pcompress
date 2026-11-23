/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
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
 */

/*
 * The seek index of a .pz file written with -X: where each data chunk
 * is in the file and in the uncompressed stream, and (archives) where
 * each member is in the stream. DESIGN.md has the layout. This file only
 * builds and parses the index body; pcompress.c authenticates, encrypts
 * and places it.
 */

#ifndef	_SEEK_INDEX_H
#define	_SEEK_INDEX_H

#include <sys/types.h>
#include <stdint.h>

#define	SX_BODY_MAGIC		"PCSXIDX1"	/* first 8 bytes of the body */
#define	SX_TRAILER_MAGIC	"PCSEEK01"	/* last 8 bytes of the file */
#define	SX_MAGIC_SZ		8
#define	SX_CHUNK_FIXED		16		/* file offset, length */
#define	SX_MEMBER_FIXED		18		/* offset, length, name length */
#define	SX_NAME_MAX		65535
#define	SX_BODY_MIN		40		/* the body's fixed header */
#define	SX_BODY_MAX		(1ULL << 30)	/* refused above: not a real index */

/* A growing byte array. */
typedef struct {
	unsigned char *buf;
	uint64_t len, cap;
} sx_buf_t;

/* The index of a file being written. */
typedef struct {
	sx_buf_t chunks, members;
	uint64_t nchunks, nmembers;
	uint64_t open_member;	/* offset in members of the open record, +1; 0: none */
	int taglen;		/* bytes of each chunk's verification field */
	/*
	 * Out of memory: the index is not written. One flag per table: the
	 * writer thread adds chunks while the archiver thread adds members.
	 */
	int failed_c, failed_m;
} sx_writer_t;

/* A parsed index; chunks and members point into the caller's body. */
typedef struct {
	uint64_t nchunks, nmembers;
	uint64_t total;		/* uncompressed stream bytes */
	const unsigned char *chunks, *members;
	uint64_t *uoff;		/* nchunks + 1 stream offsets, pc_malloc()ed */
	int taglen;
} sx_index_t;

/* What a parsed index must agree with: from the file being read. */
typedef struct {
	uint64_t file_lo;	/* first byte after the file header */
	uint64_t file_hi;	/* offset of the end-of-stream marker */
	uint64_t chunksize;	/* largest uncompressed chunk */
	int taglen;
} sx_limits_t;

void sx_writer_init(sx_writer_t *w, int taglen);
void sx_writer_free(sx_writer_t *w);
void sx_add_chunk(sx_writer_t *w, uint64_t file_off, uint64_t ulen,
    const unsigned char *tag);
void sx_add_member(sx_writer_t *w, const char *name, uint64_t off);
void sx_end_member(sx_writer_t *w, uint64_t end);
unsigned char *sx_body(sx_writer_t *w, uint64_t end, uint64_t *len);

int sx_parse(const unsigned char *body, uint64_t len, const sx_limits_t *lim,
    sx_index_t *idx);
void sx_index_free(sx_index_t *idx);
uint64_t sx_chunk_of(const sx_index_t *idx, uint64_t off);
uint64_t sx_chunk_file_off(const sx_index_t *idx, uint64_t i);
const unsigned char *sx_chunk_tag(const sx_index_t *idx, uint64_t i);
int sx_find_member(const sx_index_t *idx, const char *name, uint64_t *off,
    uint64_t *len);

#endif
