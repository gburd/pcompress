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
 * Seek index body: building it while a file is compressed, and parsing
 * it, as untrusted input, before a random read. All integers are
 * little-endian. The body is
 *
 *   magic "PCSXIDX1"  u8 taglen  u8[7] zero  u64 end  u64 nchunks
 *   u64 nmembers
 *   nchunks x  { u64 file offset of the chunk's length field,
 *                u64 uncompressed length, u8[taglen] tag }
 *   nmembers x { u64 stream offset, u64 length, u16 name length, name }
 *
 * end is the file offset of the end-of-stream marker, which the index
 * follows: with the body under the index tag, it pins the index to the
 * end of the stream it describes. A chunk's stream offset is the sum of the lengths before it, so it is
 * not stored and cannot disagree. The tag is the chunk's own verification
 * field (its digest, or its HMAC when encrypted) and ties the entry to
 * the bytes it names. Members are in archive order, which is stream
 * order; offset and length cover the member's header and data in the
 * PAX stream, so extracting a member is reading that range.
 */

#include <string.h>
#include <utils.h>
#include <pc_alloc.h>
#include "seek_index.h"

#define	SX_HDR_SZ	(SX_MAGIC_SZ + 8 + 8 + 8 + 8)

static void
put64(unsigned char *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return (v);
}

/* Append n bytes to b, growing it; sets *failed instead of failing. */
static unsigned char *
sx_grow(sx_buf_t *b, uint64_t n, int *failed)
{
	unsigned char *p;

	if (*failed)
		return (NULL);
	if (b->len + n > b->cap) {
		uint64_t cap = b->cap ? b->cap * 2 : 4096;

		while (cap < b->len + n)
			cap *= 2;
		if ((p = pc_realloc(b->buf, cap)) == NULL) {
			*failed = 1;
			return (NULL);
		}
		b->buf = p;
		b->cap = cap;
	}
	p = b->buf + b->len;
	b->len += n;
	return (p);
}

/* Start an empty index whose chunk tags are taglen bytes (0 to 64). */
void
sx_writer_init(sx_writer_t *w, int taglen)
{
	memset(w, 0, sizeof (*w));
	w->taglen = taglen;
}

void
sx_writer_free(sx_writer_t *w)
{
	pc_mfree(w->chunks.buf);
	pc_mfree(w->members.buf);
	memset(w, 0, sizeof (*w));
}

/*
 * Record the next data chunk, in file order: where its length field
 * starts in the file, how many bytes it decodes to, and its tag (the
 * taglen bytes of its verification field, as stored).
 */
void
sx_add_chunk(sx_writer_t *w, uint64_t file_off, uint64_t ulen, const unsigned char *tag)
{
	unsigned char *p = sx_grow(&w->chunks, SX_CHUNK_FIXED + w->taglen, &w->failed_c);

	if (p == NULL)
		return;
	put64(p, file_off);
	put64(p + 8, ulen);
	memcpy(p + 16, tag, w->taglen);
	w->nchunks++;
}

/*
 * Record that member name starts at stream offset off; it lasts until
 * the next call or sx_end_member(). Names longer than SX_NAME_MAX are
 * left out (they cannot be extracted by name, only sequentially).
 */
void
sx_add_member(sx_writer_t *w, const char *name, uint64_t off)
{
	size_t n = strlen(name);
	unsigned char *p;

	sx_end_member(w, off);
	if (n > SX_NAME_MAX)
		return;
	if ((p = sx_grow(&w->members, SX_MEMBER_FIXED + n, &w->failed_m)) == NULL)
		return;
	put64(p, off);
	put64(p + 8, 0);
	p[16] = (unsigned char)n;
	p[17] = (unsigned char)(n >> 8);
	memcpy(p + 18, name, n);
	w->open_member = (p - w->members.buf) + 1;
	w->nmembers++;
}

/* Close the open member record, if any, at stream offset end. */
void
sx_end_member(sx_writer_t *w, uint64_t end)
{
	unsigned char *p;

	if (w->open_member == 0 || w->failed_m)
		return;
	p = w->members.buf + w->open_member - 1;
	put64(p + 8, end - get64(p));
	w->open_member = 0;
}

/*
 * The finished body for an end-of-stream marker at file offset end, in
 * pc_malloc() memory the caller frees, or NULL if memory ran out at any
 * point.
 */
unsigned char *
sx_body(sx_writer_t *w, uint64_t end, uint64_t *len)
{
	unsigned char *b;

	if (w->failed_c || w->failed_m)
		return (NULL);
	*len = SX_HDR_SZ + w->chunks.len + w->members.len;
	if ((b = pc_malloc(*len)) == NULL)
		return (NULL);
	memset(b, 0, SX_HDR_SZ);
	memcpy(b, SX_BODY_MAGIC, SX_MAGIC_SZ);
	b[8] = (unsigned char)w->taglen;
	put64(b + 16, end);
	put64(b + 24, w->nchunks);
	put64(b + 32, w->nmembers);
	if (w->chunks.len)
		memcpy(b + SX_HDR_SZ, w->chunks.buf, w->chunks.len);
	if (w->members.len)
		memcpy(b + SX_HDR_SZ + w->chunks.len, w->members.buf, w->members.len);
	return (b);
}

/*
 * Check the chunk table: every chunk lies after the header, before the
 * end marker, after the previous one with room for at least its length
 * field, and decodes to 1 to chunksize bytes. Fills idx->uoff.
 */
static int
sx_parse_chunks(sx_index_t *idx, const sx_limits_t *lim)
{
	uint64_t i, prev = 0, off, ulen, esz = SX_CHUNK_FIXED + idx->taglen;

	idx->uoff = pc_malloc((idx->nchunks + 1) * sizeof (uint64_t));
	if (idx->uoff == NULL)
		return (-1);
	idx->uoff[0] = 0;
	for (i = 0; i < idx->nchunks; i++) {
		off = get64(idx->chunks + i * esz);
		ulen = get64(idx->chunks + i * esz + 8);
		if (off < lim->file_lo || off + 8 > lim->file_hi ||
		    (i > 0 && off < prev + 8) || ulen == 0 || ulen > lim->chunksize)
			return (-1);
		prev = off;
		idx->uoff[i + 1] = idx->uoff[i] + ulen;
	}
	idx->total = idx->uoff[idx->nchunks];
	return (0);
}

/* Check the member table: in stream order, inside the stream, names sane. */
static int
sx_parse_members(sx_index_t *idx, const unsigned char *end)
{
	const unsigned char *p = idx->members;
	uint64_t i, off, len, prev = 0;
	size_t n;

	for (i = 0; i < idx->nmembers; i++) {
		if ((uint64_t)(end - p) < SX_MEMBER_FIXED)
			return (-1);
		off = get64(p);
		len = get64(p + 8);
		n = p[16] | ((size_t)p[17] << 8);
		if ((uint64_t)(end - p - SX_MEMBER_FIXED) < n || n == 0 ||
		    memchr(p + SX_MEMBER_FIXED, 0, n) != NULL || off < prev ||
		    len == 0 || off > idx->total || len > idx->total - off)
			return (-1);
		prev = off + len;
		p += SX_MEMBER_FIXED + n;
	}
	return (p == end ? 0 : -1);
}

/*
 * Parse an authenticated, decrypted body against what the file says.
 * Still untrusted for its own consistency (a bug in an old writer, or a
 * key holder who forged it): every count, offset and length is checked
 * against the body's size and against lim. 0, or -1 (corrupt; idx is
 * then empty and need not be freed).
 */
int
sx_parse(const unsigned char *body, uint64_t len, const sx_limits_t *lim, sx_index_t *idx)
{
	uint64_t esz;

	memset(idx, 0, sizeof (*idx));
	if (len < SX_HDR_SZ || memcmp(body, SX_BODY_MAGIC, SX_MAGIC_SZ) != 0 ||
	    body[8] != lim->taglen || get64(body + 16) != lim->file_hi)
		return (-1);
	idx->taglen = body[8];
	idx->nchunks = get64(body + 24);
	idx->nmembers = get64(body + 32);
	esz = SX_CHUNK_FIXED + idx->taglen;
	if (idx->nchunks == 0 || idx->nchunks > (len - SX_HDR_SZ) / esz ||
	    idx->nmembers > len)
		return (-1);
	idx->chunks = body + SX_HDR_SZ;
	idx->members = idx->chunks + idx->nchunks * esz;
	if (sx_parse_chunks(idx, lim) != 0 || sx_parse_members(idx, body + len) != 0) {
		sx_index_free(idx);
		return (-1);
	}
	return (0);
}

void
sx_index_free(sx_index_t *idx)
{
	pc_mfree(idx->uoff);
	memset(idx, 0, sizeof (*idx));
}

/* The chunk holding stream byte off (< idx->total): binary search. */
uint64_t
sx_chunk_of(const sx_index_t *idx, uint64_t off)
{
	uint64_t lo = 0, hi = idx->nchunks - 1, mid;

	while (lo < hi) {
		mid = lo + (hi - lo + 1) / 2;
		if (idx->uoff[mid] <= off)
			lo = mid;
		else
			hi = mid - 1;
	}
	return (lo);
}

uint64_t
sx_chunk_file_off(const sx_index_t *idx, uint64_t i)
{
	return (get64(idx->chunks + i * (SX_CHUNK_FIXED + idx->taglen)));
}

const unsigned char *
sx_chunk_tag(const sx_index_t *idx, uint64_t i)
{
	return (idx->chunks + i * (SX_CHUNK_FIXED + idx->taglen) + 16);
}

/*
 * Find member name: its stream offset and length. 1 if found, else 0.
 * A linear scan: one lookup per job, and the table is in archive order.
 * With several members of the same name (an archive of overlapping
 * paths), the last one wins, as it would on a full extraction.
 */
int
sx_find_member(const sx_index_t *idx, const char *name, uint64_t *off, uint64_t *len)
{
	const unsigned char *p = idx->members;
	size_t want = strlen(name), n;
	uint64_t i;
	int found = 0;

	for (i = 0; i < idx->nmembers; i++) {
		n = p[16] | ((size_t)p[17] << 8);
		if (n == want && memcmp(p + SX_MEMBER_FIXED, name, n) == 0) {
			*off = get64(p);
			*len = get64(p + 8);
			found = 1;
		}
		p += SX_MEMBER_FIXED + n;
	}
	return (found);
}
