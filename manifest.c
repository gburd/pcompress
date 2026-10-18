/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Snapshot manifests (ROADMAP Phase 6): the list of entries in one backup,
 * each a path, its metadata and, for files, the ids and lengths of its
 * chunks in order. A backup tool stores the encoded manifest as an object
 * of its own (pc_chunk_seal() encrypts and authenticates it).
 *
 * Encoding, little-endian:
 *   "PCMF1\0\0\0", u64 entry count, then per entry, sorted by path bytes:
 *   u16 path length, path, u8 type, u32 mode, u32 uid, u32 gid,
 *   i64 mtime, u32 mtime nanoseconds, u64 size, u16 target length, target
 *   (symlinks), u32 chunk count, then per chunk a 32-byte id and u32 length.
 *
 * The decoder is written for hostile input: every path is relative with
 * no empty, "." or ".." component; entries are strictly sorted (no
 * duplicates); no entry lies under a symlink entry; a file's chunk lengths
 * add up to its size; and nothing is left over.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <pcompress.h>

#define	PCM_MAGIC	"PCMF1\0\0\0"
#define	PCM_MIN_ENTRY	(2 + 1 + 1 + 12 + 12 + 8 + 2 + 4)	/* one-byte path */

struct ment {
	char *path, *target;
	size_t plen, tlen, n;
	int type;
	uint32_t mode, uid, gid, nsec;
	int64_t mtime;
	uint64_t size;
	uint8_t (*ids)[32];
	uint32_t *lens;
};

struct pc_manifest {
	struct ment *e;
	size_t n, cap;
	int sorted;
};

static int
path_ok(const char *p, size_t len)
{
	size_t i, s = 0, c;

	if (len == 0 || len > 65535)
		return (0);
	for (i = 0; i <= len; i++) {
		if (i < len && p[i] == '\0')
			return (0);
		if (i < len && p[i] != '/')
			continue;
		c = i - s;
		if (c == 0 || (c == 1 && p[s] == '.') || (c == 2 && p[s] == '.' && p[s + 1] == '.'))
			return (0);	/* leading, trailing or double '/', ".", ".." */
		s = i + 1;
	}
	return (1);
}

static int
entry_ok(const struct ment *e)
{
	uint64_t sum = 0;
	size_t i;

	if (!path_ok(e->path, e->plen) || e->nsec >= 1000000000)
		return (0);
	switch (e->type) {
	case PC_ENTRY_FILE:
		if (e->tlen != 0)
			return (0);
		for (i = 0; i < e->n; i++) {
			if (e->lens[i] == 0 || sum > UINT64_MAX - e->lens[i])
				return (0);
			sum += e->lens[i];
		}
		return (sum == e->size);
	case PC_ENTRY_DIR:
		return (e->n == 0 && e->size == 0 && e->tlen == 0);
	case PC_ENTRY_SYMLINK:
		return (e->n == 0 && e->size == 0 && e->tlen > 0 && e->tlen <= 65535 &&
		    memchr(e->target, '\0', e->tlen) == NULL);
	}
	return (0);
}

static void
ment_free(struct ment *e)
{
	free(e->path);
	free(e->target);
	free(e->ids);
	free(e->lens);
}

/* Byte order of (p, plen) against an entry's path; a prefix sorts first. */
static int
pcmp(const char *p, size_t plen, const struct ment *e)
{
	int c = memcmp(p, e->path, plen < e->plen ? plen : e->plen);

	if (c != 0)
		return (c);
	return (plen < e->plen ? -1 : plen > e->plen);
}

static int
ment_cmp(const void *a, const void *b)
{
	const struct ment *x = a;

	return (pcmp(x->path, x->plen, (const struct ment *)b));
}

static int64_t
lookup(const pc_manifest_t *m, const char *p, size_t plen)
{
	size_t lo = 0, hi = m->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = pcmp(p, plen, &m->e[mid]);

		if (c == 0)
			return ((int64_t)mid);
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return (-1);
}

/*
 * Sorted, no duplicates, nothing under a symlink: what makes a restore
 * that follows the manifest unable to write through a link it created.
 * (Paths compare as bytes: on a case- or normalisation-insensitive file
 * system two different paths can still name one file; see pcbackup.)
 */
static int
check_tree(const pc_manifest_t *m)
{
	size_t i;

	for (i = 1; i < m->n; i++)
		if (pcmp(m->e[i - 1].path, m->e[i - 1].plen, &m->e[i]) >= 0)
			return (0);
	/*
	 * Entries starting "<link>/" are contiguous in byte order, from the
	 * lower bound of that key on: one search per symlink, not one per '/'
	 * of every path (quadratic in a long path).
	 */
	for (i = 0; i < m->n; i++) {
		const struct ment *l = &m->e[i];
		size_t lo = i + 1, hi = m->n, kl = l->plen + 1;
		char *k;
		int bad;

		if (l->type != PC_ENTRY_SYMLINK)
			continue;
		if ((k = malloc(kl)) == NULL)
			return (0);
		memcpy(k, l->path, l->plen);
		k[l->plen] = '/';
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;

			if (pcmp(k, kl, &m->e[mid]) > 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		bad = lo < m->n && m->e[lo].plen > kl && memcmp(m->e[lo].path, k, kl) == 0;
		free(k);
		if (bad)
			return (0);
	}
	return (1);
}

static void
msort(pc_manifest_t *m)
{
	if (!m->sorted) {
		qsort(m->e, m->n, sizeof (*m->e), ment_cmp);
		m->sorted = 1;
	}
}

pc_manifest_t DLL_EXPORT *
pc_manifest_new(void)
{
	pc_manifest_t *m = calloc(1, sizeof (*m));

	if (m != NULL)
		m->sorted = 1;
	return (m);
}

void DLL_EXPORT
pc_manifest_free(pc_manifest_t *m)
{
	size_t i;

	if (m == NULL)
		return;
	for (i = 0; i < m->n; i++)
		ment_free(&m->e[i]);
	free(m->e);
	free(m);
}

static int
grow(pc_manifest_t *m)
{
	size_t nc;
	void *p;

	if (m->n < m->cap)
		return (0);
	nc = m->cap ? m->cap * 2 : 64;
	if (nc > SIZE_MAX / sizeof (*m->e) || (p = realloc(m->e, nc * sizeof (*m->e))) == NULL)
		return (-1);
	m->e = p;
	m->cap = nc;
	return (0);
}

static char *
dupn(const char *s, size_t n)
{
	char *d = malloc(n + 1);

	if (d != NULL) {
		memcpy(d, s, n);
		d[n] = '\0';
	}
	return (d);
}

int DLL_EXPORT
pc_manifest_add(pc_manifest_t *m, const pc_entry_t *in)
{
	struct ment e;

	if (m == NULL || in == NULL || in->path == NULL || (in->nchunks > 0 &&
	    (in->ids == NULL || in->lens == NULL)) || in->nchunks > SIZE_MAX / 32)
		return (-1);
	memset(&e, 0, sizeof (e));
	e.plen = strlen(in->path);
	e.tlen = in->target ? strlen(in->target) : 0;	/* "" is no target */
	e.type = in->type;
	e.mode = in->mode;
	e.uid = in->uid;
	e.gid = in->gid;
	e.mtime = in->mtime;
	e.nsec = in->mtime_nsec;
	e.size = in->size;
	e.n = in->nchunks;
	e.path = dupn(in->path, e.plen);
	e.target = e.tlen ? dupn(in->target, e.tlen) : NULL;
	if (e.n > 0) {
		e.ids = malloc(e.n * 32);
		e.lens = malloc(e.n * sizeof (uint32_t));
	}
	if (e.path == NULL || (e.tlen && e.target == NULL) ||
	    (e.n > 0 && (e.ids == NULL || e.lens == NULL)))
		goto fail;
	if (e.n > 0) {
		memcpy(e.ids, in->ids, e.n * 32);
		memcpy(e.lens, in->lens, e.n * sizeof (uint32_t));
	}
	if (!entry_ok(&e) || grow(m) != 0)
		goto fail;
	if (m->n > 0 && pcmp(m->e[m->n - 1].path, m->e[m->n - 1].plen, &e) >= 0)
		m->sorted = 0;	/* added out of order: sort before use */
	m->e[m->n++] = e;
	return (0);
fail:
	ment_free(&e);
	return (-1);
}

size_t DLL_EXPORT
pc_manifest_count(pc_manifest_t *m)
{
	return (m ? m->n : 0);
}

static void
fill(const struct ment *e, pc_entry_t *out)
{
	out->path = e->path;
	out->target = e->target;
	out->type = e->type;
	out->mode = e->mode;
	out->uid = e->uid;
	out->gid = e->gid;
	out->mtime = e->mtime;
	out->mtime_nsec = e->nsec;
	out->size = e->size;
	out->nchunks = e->n;
	out->ids = (const uint8_t *)e->ids;
	out->lens = e->lens;
}

int DLL_EXPORT
pc_manifest_get(pc_manifest_t *m, size_t i, pc_entry_t *out)
{
	if (m == NULL || out == NULL || i >= m->n)
		return (-1);
	msort(m);
	fill(&m->e[i], out);
	return (0);
}

int64_t DLL_EXPORT
pc_manifest_find(pc_manifest_t *m, const char *path, pc_entry_t *out)
{
	int64_t k;

	if (m == NULL || path == NULL)
		return (-1);
	msort(m);
	k = lookup(m, path, strlen(path));
	if (k >= 0 && out != NULL)
		fill(&m->e[k], out);
	return (k);
}

static uchar_t *
put(uchar_t *p, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (uchar_t)(v >> (8 * i));
	return (p + n);
}

int DLL_EXPORT
pc_manifest_encode(pc_manifest_t *m, void *buf, size_t *len)
{
	size_t need = 16, i, j;
	uchar_t *p;

	if (m == NULL || len == NULL)
		return (-1);
	msort(m);
	if (!check_tree(m))
		return (-1);	/* duplicate path, or an entry under a symlink */
	for (i = 0; i < m->n; i++) {
		size_t sz = PCM_MIN_ENTRY - 1 + m->e[i].plen + m->e[i].tlen;

		if (m->e[i].n > (SIZE_MAX - need - sz) / 36)
			return (-1);
		need += sz + m->e[i].n * 36;
	}
	if (buf == NULL) {
		*len = need;
		return (0);
	}
	if (*len < need)
		return (-1);
	p = buf;
	memcpy(p, PCM_MAGIC, 8);
	p = put(p + 8, m->n, 8);
	for (i = 0; i < m->n; i++) {
		const struct ment *e = &m->e[i];

		p = put(p, e->plen, 2);
		memcpy(p, e->path, e->plen);
		p = put(p + e->plen, (uint64_t)e->type, 1);
		p = put(p, e->mode, 4);
		p = put(p, e->uid, 4);
		p = put(p, e->gid, 4);
		p = put(p, (uint64_t)e->mtime, 8);
		p = put(p, e->nsec, 4);
		p = put(p, e->size, 8);
		p = put(p, e->tlen, 2);
		if (e->tlen)
			memcpy(p, e->target, e->tlen);
		p = put(p + e->tlen, e->n, 4);
		for (j = 0; j < e->n; j++) {
			memcpy(p, e->ids[j], 32);
			p = put(p + 32, e->lens[j], 4);
		}
	}
	*len = need;
	return (0);
}

/* Bounded little-endian reader over the input. */
struct rd {
	const uchar_t *p;
	size_t left;
	int bad;
};

static uint64_t
get(struct rd *r, int n)
{
	uint64_t v = 0;
	int i;

	if (r->bad || r->left < (size_t)n) {
		r->bad = 1;
		return (0);
	}
	for (i = 0; i < n; i++)
		v |= (uint64_t)r->p[i] << (8 * i);
	r->p += n;
	r->left -= n;
	return (v);
}

static const uchar_t *
take(struct rd *r, size_t n)
{
	const uchar_t *p = r->p;

	if (r->bad || r->left < n) {
		r->bad = 1;
		return (NULL);
	}
	r->p += n;
	r->left -= n;
	return (p);
}

pc_manifest_t DLL_EXPORT *
pc_manifest_decode(const void *buf, size_t len)
{
	struct rd r = { buf, len, 0 };
	pc_manifest_t *m;
	uint64_t count, i, j;
	const uchar_t *s;

	if (buf == NULL || len < 16 || memcmp(buf, PCM_MAGIC, 8) != 0)
		return (NULL);
	(void) take(&r, 8);
	count = get(&r, 8);
	if (count > r.left / PCM_MIN_ENTRY || (m = pc_manifest_new()) == NULL)
		return (NULL);	/* the count cannot fit: refuse before allocating */
	if (count > 0 && (m->e = calloc((size_t)count, sizeof (*m->e))) == NULL) {
		free(m);
		return (NULL);
	}
	m->cap = (size_t)count;
	for (i = 0; i < count; i++) {
		struct ment *e = &m->e[i];

		m->n = (size_t)i + 1;	/* so pc_manifest_free() releases it */
		e->plen = (size_t)get(&r, 2);
		if ((s = take(&r, e->plen)) == NULL || (e->path = dupn((const char *)s, e->plen)) == NULL)
			goto bad;
		e->type = (int)get(&r, 1);
		e->mode = (uint32_t)get(&r, 4);
		e->uid = (uint32_t)get(&r, 4);
		e->gid = (uint32_t)get(&r, 4);
		e->mtime = (int64_t)get(&r, 8);
		e->nsec = (uint32_t)get(&r, 4);
		e->size = get(&r, 8);
		e->tlen = (size_t)get(&r, 2);
		if (e->tlen > 0 && ((s = take(&r, e->tlen)) == NULL ||
		    (e->target = dupn((const char *)s, e->tlen)) == NULL))
			goto bad;
		e->n = (size_t)get(&r, 4);
		if (r.bad || e->n > r.left / 36)
			goto bad;
		if (e->n > 0) {
			if ((e->ids = malloc(e->n * 32)) == NULL ||
			    (e->lens = malloc(e->n * sizeof (uint32_t))) == NULL)
				goto bad;
			for (j = 0; j < e->n; j++) {	/* 36 bytes each: checked above */
				memcpy(e->ids[j], take(&r, 32), 32);
				e->lens[j] = (uint32_t)get(&r, 4);
			}
		}
		if (r.bad || !entry_ok(e))
			goto bad;
	}
	if (r.left != 0 || !check_tree(m))
		goto bad;
	return (m);
bad:
	pc_manifest_free(m);
	return (NULL);
}
