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
 * or in one directory of it, each a path, its metadata and, for files, the
 * ids and lengths of its chunks in order. A backup tool stores the encoded
 * manifest as an object of its own (pc_chunk_seal() encrypts and
 * authenticates it).
 *
 * Encoding, little-endian:
 *   "PCMF2\0\0\0", u64 entry count, then per entry, sorted by path bytes:
 *   u16 path length, path, u8 type, u32 mode, u32 uid, u32 gid,
 *   i64 mtime, u32 mtime nanoseconds, u64 size, u16 target length, target
 *   (symlinks, hardlinks), u32 chunk count, then per chunk a 32-byte id
 *   and u32 length; then records, each u8 tag, u32 length and a value,
 *   ended by a 0 byte. PCMF1 (4.2, 4.3) is the same without the records
 *   and the 0 byte, and only types 1-3.
 *
 * Records come in tag order. XATTR and ACL repeat, ordered by name and by
 * kind; no other known tag repeats. Tags this library does not know are
 * kept, in order, as the entry's ext bytes and written back. A tag with
 * bit 0x80 set is critical: a reader that does not know it must refuse
 * the manifest instead of restoring without it.
 *
 * The decoder is written for hostile input: every path is relative with
 * no empty, "." or ".." component; entries are strictly sorted (no
 * duplicates); no entry lies under a symlink entry or a directory entry
 * with a subtree; a file's chunk lengths add up to its size; records are
 * well formed, in order and allowed for the entry's type; and nothing is
 * left over. So a decoded manifest re-encodes to the same bytes.
 */

#include <sys/types.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <pcompress.h>

#define	PCM_MAGIC1	"PCMF1\0\0\0"
#define	PCM_MAGIC2	"PCMF2\0\0\0"
#define	PCM_MIN_ENTRY	(2 + 1 + 1 + 12 + 12 + 8 + 2 + 4)	/* one-byte path, v1 */

/* Record tags. Never renumber: they are the format. */
enum {
	T_END = 0, T_BTIME = 1, T_CTIME = 2, T_INODE = 3, T_RDEV = 4, T_FLAGS = 5,
	T_OWNER = 6, T_GROUP = 7, T_XATTR = 8, T_ACL = 9, T_SUBTREE = 10,
	T_SPARSE = 11, T_LAST_KNOWN = 11, T_CRITICAL = 0x80
};
#define	REC_HDR		5	/* u8 tag, u32 length */

struct ment {
	char *path, *target;
	size_t plen, tlen, n;
	int type;
	uint32_t mode, uid, gid, nsec;
	int64_t mtime;
	uint64_t size;
	uint8_t (*ids)[32];
	uint32_t *lens;
	/* Format 2. */
	uint32_t have, bnsec, cnsec;
	int64_t btime, ctime;
	uint64_t dev, ino, rdev, flags;
	int fkind;
	char *owner, *group;
	size_t nx, na, extlen;
	pc_xattr_t *x;		/* sorted by name; names and values owned */
	pc_acl_t *a;		/* sorted by kind; data owned */
	uint8_t *subtree;	/* 32 bytes or NULL */
	uchar_t *ext;		/* unknown records, as encoded */
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

/* An owner, group or xattr name: 1-255 bytes (a C string has no NUL). */
static int
name_ok(const char *s)
{
	size_t l = s ? strlen(s) : 0;

	return (l > 0 && l <= 255);
}

/*
 * The unknown records an entry carries: well formed, tags unknown to this
 * library, not critical, and in order.
 */
static int
ext_ok(const uchar_t *p, size_t len)
{
	int last = 0;

	while (len > 0) {
		uint32_t rl;

		if (len < REC_HDR || p[0] <= T_LAST_KNOWN || (p[0] & T_CRITICAL) || p[0] < last)
			return (0);
		last = p[0];
		rl = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 |
		    (uint32_t)p[4] << 24;
		if (rl > len - REC_HDR)
			return (0);
		p += REC_HDR + rl;
		len -= REC_HDR + rl;
	}
	return (1);
}

/* The format 2 metadata: allowed for the type, canonical, in range. */
static int
meta_ok(const struct ment *e)
{
	size_t i;
	int dev = e->type == PC_ENTRY_CHARDEV || e->type == PC_ENTRY_BLOCKDEV;

	if ((e->have & ~(uint32_t)(PC_HAVE_BTIME | PC_HAVE_CTIME | PC_HAVE_INODE |
	    PC_HAVE_SPARSE)) != 0 || e->bnsec >= 1000000000 || e->cnsec >= 1000000000)
		return (0);
	if ((e->have & PC_HAVE_SPARSE) && e->type != PC_ENTRY_FILE)
		return (0);
	/* A value without its bit would not be encoded: refuse, do not drop it. */
	if ((!(e->have & PC_HAVE_BTIME) && (e->btime != 0 || e->bnsec != 0)) ||
	    (!(e->have & PC_HAVE_CTIME) && (e->ctime != 0 || e->cnsec != 0)) ||
	    (!(e->have & PC_HAVE_INODE) && (e->dev != 0 || e->ino != 0)))
		return (0);
	if (!dev && e->rdev != 0)
		return (0);
	if (e->subtree != NULL && e->type != PC_ENTRY_DIR)
		return (0);
	if (e->fkind < 0 || e->fkind > 255 || (e->fkind == 0 && e->flags != 0))
		return (0);
	if ((e->owner && !name_ok(e->owner)) || (e->group && !name_ok(e->group)))
		return (0);
	for (i = 0; i < e->nx; i++) {
		if (!name_ok(e->x[i].name) || e->x[i].len > UINT32_MAX - 256 ||
		    (e->x[i].len > 0 && e->x[i].value == NULL) ||
		    (i > 0 && strcmp(e->x[i - 1].name, e->x[i].name) >= 0))
			return (0);
	}
	for (i = 0; i < e->na; i++) {
		if (e->a[i].kind < 1 || e->a[i].kind > 255 || e->a[i].len == 0 ||
		    e->a[i].len > UINT32_MAX - 1 || e->a[i].data == NULL ||
		    (i > 0 && e->a[i - 1].kind >= e->a[i].kind))
			return (0);
	}
	return (ext_ok(e->ext, e->extlen));
}

static int
entry_ok(const struct ment *e)
{
	uint64_t sum = 0;
	size_t i;

	if (!path_ok(e->path, e->plen) || e->nsec >= 1000000000 || !meta_ok(e))
		return (0);
	if ((e->type != PC_ENTRY_FILE && (e->n != 0 || e->size != 0)) || e->n > UINT32_MAX)
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
	case PC_ENTRY_SYMLINK:
		return (e->tlen > 0 && e->tlen <= 65535 &&
		    memchr(e->target, '\0', e->tlen) == NULL);
	case PC_ENTRY_HARDLINK:
		return (path_ok(e->target, e->tlen));
	case PC_ENTRY_DIR:
	case PC_ENTRY_CHARDEV:
	case PC_ENTRY_BLOCKDEV:
	case PC_ENTRY_FIFO:
		return (e->tlen == 0);
	}
	return (0);
}

static void
ment_free(struct ment *e)
{
	size_t i;

	free(e->path);
	free(e->target);
	free(e->ids);
	free(e->lens);
	free(e->owner);
	free(e->group);
	for (i = 0; i < e->nx; i++) {
		free((void *)e->x[i].name);
		free((void *)e->x[i].value);
	}
	free(e->x);
	for (i = 0; i < e->na; i++)
		free((void *)e->a[i].data);
	free(e->a);
	free(e->subtree);
	free(e->ext);
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
 * Nothing under a directory with a subtree either: its contents are in
 * that other manifest, and two sources for one path would be ambiguous.
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

		if (l->type != PC_ENTRY_SYMLINK && l->subtree == NULL)
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

/*
 * pc_entry_t as of the first API (4.2), ending with lens: what programs
 * built before the _sz functions pass. Version 2 (4.4) is the whole of
 * today's pc_entry_t. Fields added later go after it.
 */
#define	PC_ENTRY_V1_SIZE	(offsetof(pc_entry_t, lens) + sizeof (const uint32_t *))

/* A caller's entry of esize bytes: version 1, or at least version 2. */
static int
esize_ok(size_t esize)
{
	return (esize == PC_ENTRY_V1_SIZE || esize >= sizeof (pc_entry_t));
}

/* True if the bytes of a newer caller's entry beyond ours are all zero. */
static int
tail_zero(const pc_entry_t *in, size_t esize)
{
	const uchar_t *p = (const uchar_t *)in;
	size_t i;

	for (i = sizeof (pc_entry_t); i < esize; i++)
		if (p[i] != 0)
			return (0);
	return (1);
}

static void *
dupmem(const void *s, size_t n)
{
	void *d;

	if (s == NULL && n > 0)
		return (NULL);
	if ((d = malloc(n ? n : 1)) != NULL && n > 0)
		memcpy(d, s, n);
	return (d);
}

static int
xattr_cmp(const void *a, const void *b)
{
	return (strcmp(((const pc_xattr_t *)a)->name, ((const pc_xattr_t *)b)->name));
}

static int
acl_cmp(const void *a, const void *b)
{
	int x = ((const pc_acl_t *)a)->kind, y = ((const pc_acl_t *)b)->kind;

	return ((x > y) - (x < y));
}

/*
 * Copy a version 2 caller's metadata into e, xattrs and ACLs sorted.
 * -1 on bad arguments or out of memory; entry_ok() checks the values.
 */
static int
copy_meta(struct ment *e, const pc_entry_t *in)
{
	size_t i;

	e->have = in->have;
	e->bnsec = in->btime_nsec;
	e->cnsec = in->ctime_nsec;
	e->btime = in->btime;
	e->ctime = in->ctime;
	e->dev = in->dev;
	e->ino = in->ino;
	e->rdev = in->rdev;
	e->fkind = in->flags_kind;
	e->flags = in->flags;
	if ((in->owner && (e->owner = dupn(in->owner, strlen(in->owner))) == NULL) ||
	    (in->group && (e->group = dupn(in->group, strlen(in->group))) == NULL))
		return (-1);
	if ((in->nxattrs > 0 && in->xattrs == NULL) || (in->nacls > 0 && in->acls == NULL) ||
	    (in->extlen > 0 && in->ext == NULL) ||
	    in->nxattrs > SIZE_MAX / sizeof (pc_xattr_t) || in->nacls > SIZE_MAX / sizeof (pc_acl_t))
		return (-1);
	if (in->nxattrs > 0) {
		if ((e->x = calloc(in->nxattrs, sizeof (*e->x))) == NULL)
			return (-1);
		for (i = 0; i < in->nxattrs; i++, e->nx++) {
			const pc_xattr_t *x = &in->xattrs[i];

			if (x->name == NULL || (x->len > 0 && x->value == NULL))
				return (-1);
			e->x[i].len = x->len;
			if ((e->x[i].name = dupn(x->name, strlen(x->name))) == NULL ||
			    (e->x[i].value = dupmem(x->value, x->len)) == NULL) {
				e->nx++;
				return (-1);
			}
		}
		qsort(e->x, e->nx, sizeof (*e->x), xattr_cmp);
	}
	if (in->nacls > 0) {
		if ((e->a = calloc(in->nacls, sizeof (*e->a))) == NULL)
			return (-1);
		for (i = 0; i < in->nacls; i++, e->na++) {
			e->a[i] = in->acls[i];
			if (in->acls[i].data == NULL ||
			    (e->a[i].data = dupmem(in->acls[i].data, in->acls[i].len)) == NULL)
				return (-1);
		}
		qsort(e->a, e->na, sizeof (*e->a), acl_cmp);
	}
	if ((in->subtree && (e->subtree = dupmem(in->subtree, 32)) == NULL) ||
	    (in->extlen > 0 && (e->ext = dupmem(in->ext, in->extlen)) == NULL))
		return (-1);
	e->extlen = in->extlen;
	return (0);
}

int DLL_EXPORT
pc_manifest_add_sz(pc_manifest_t *m, const pc_entry_t *in, size_t esize)
{
	struct ment e;

	/* A field this library does not know would be dropped: refuse. */
	if (!esize_ok(esize) || (in != NULL && !tail_zero(in, esize)))
		return (-1);
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
	if (esize >= sizeof (pc_entry_t) && copy_meta(&e, in) != 0)
		goto fail;
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

/* Parenthesised: pcompress.h maps the plain name to the _sz form. */
int DLL_EXPORT
(pc_manifest_add)(pc_manifest_t *m, const pc_entry_t *in)
{
	return (pc_manifest_add_sz(m, in, PC_ENTRY_V1_SIZE));
}

size_t DLL_EXPORT
pc_manifest_count(pc_manifest_t *m)
{
	return (m ? m->n : 0);
}

/* Fill a caller's entry of esize bytes; fields it has that we lack read 0. */
static void
fill(const struct ment *e, pc_entry_t *out, size_t esize)
{
	if (esize > sizeof (pc_entry_t))
		memset((uchar_t *)out + sizeof (pc_entry_t), 0, esize - sizeof (pc_entry_t));
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
	if (esize < sizeof (pc_entry_t))
		return;
	out->have = e->have;
	out->btime_nsec = e->bnsec;
	out->ctime_nsec = e->cnsec;
	out->btime = e->btime;
	out->ctime = e->ctime;
	out->dev = e->dev;
	out->ino = e->ino;
	out->rdev = e->rdev;
	out->flags_kind = e->fkind;
	out->flags = e->flags;
	out->owner = e->owner;
	out->group = e->group;
	out->nxattrs = e->nx;
	out->xattrs = e->x;
	out->nacls = e->na;
	out->acls = e->a;
	out->subtree = e->subtree;
	out->ext = e->ext;
	out->extlen = e->extlen;
}

int DLL_EXPORT
pc_manifest_get_sz(pc_manifest_t *m, size_t i, pc_entry_t *out, size_t esize)
{
	if (m == NULL || out == NULL || i >= m->n || !esize_ok(esize))
		return (-1);
	msort(m);
	fill(&m->e[i], out, esize);
	return (0);
}

int DLL_EXPORT
(pc_manifest_get)(pc_manifest_t *m, size_t i, pc_entry_t *out)
{
	return (pc_manifest_get_sz(m, i, out, PC_ENTRY_V1_SIZE));
}

int64_t DLL_EXPORT
pc_manifest_find_sz(pc_manifest_t *m, const char *path, pc_entry_t *out, size_t esize)
{
	int64_t k;

	if (m == NULL || path == NULL || (out != NULL && !esize_ok(esize)))
		return (-1);
	msort(m);
	k = lookup(m, path, strlen(path));
	if (k >= 0 && out != NULL)
		fill(&m->e[k], out, esize);
	return (k);
}

int64_t DLL_EXPORT
(pc_manifest_find)(pc_manifest_t *m, const char *path, pc_entry_t *out)
{
	return (pc_manifest_find_sz(m, path, out, PC_ENTRY_V1_SIZE));
}

/*
 * Output for the encoder: with p NULL it only counts, so sizing and
 * writing run the same code and cannot disagree. ovf: the size overflowed.
 */
struct wr {
	uchar_t *p;
	size_t n;
	int ovf;
};

static void
wbytes(struct wr *w, const void *s, size_t len)
{
	if (len > SIZE_MAX - w->n) {
		w->ovf = 1;
		return;
	}
	if (w->p != NULL && len > 0)
		memcpy(w->p + w->n, s, len);
	w->n += len;
}

static void
wint(struct wr *w, uint64_t v, int n)
{
	uchar_t b[8];
	int i;

	for (i = 0; i < n; i++)
		b[i] = (uchar_t)(v >> (8 * i));
	wbytes(w, b, n);
}

/* A record header: tag and the length of what follows. */
static void
wrec(struct wr *w, int tag, size_t len)
{
	wint(w, (uint64_t)tag, 1);
	wint(w, (uint64_t)len, 4);
}

/* One entry's records, in tag order (see the top of the file). */
static void
wrecords(struct wr *w, const struct ment *e)
{
	size_t i, l;

	if (e->have & PC_HAVE_BTIME) {
		wrec(w, T_BTIME, 12);
		wint(w, (uint64_t)e->btime, 8);
		wint(w, e->bnsec, 4);
	}
	if (e->have & PC_HAVE_CTIME) {
		wrec(w, T_CTIME, 12);
		wint(w, (uint64_t)e->ctime, 8);
		wint(w, e->cnsec, 4);
	}
	if (e->have & PC_HAVE_INODE) {
		wrec(w, T_INODE, 16);
		wint(w, e->dev, 8);
		wint(w, e->ino, 8);
	}
	if (e->type == PC_ENTRY_CHARDEV || e->type == PC_ENTRY_BLOCKDEV) {
		wrec(w, T_RDEV, 8);
		wint(w, e->rdev, 8);
	}
	if (e->fkind != 0) {
		wrec(w, T_FLAGS, 9);
		wint(w, (uint64_t)e->fkind, 1);
		wint(w, e->flags, 8);
	}
	if (e->owner != NULL) {
		wrec(w, T_OWNER, l = strlen(e->owner));
		wbytes(w, e->owner, l);
	}
	if (e->group != NULL) {
		wrec(w, T_GROUP, l = strlen(e->group));
		wbytes(w, e->group, l);
	}
	for (i = 0; i < e->nx; i++) {
		l = strlen(e->x[i].name);
		wrec(w, T_XATTR, 1 + l + e->x[i].len);
		wint(w, l, 1);
		wbytes(w, e->x[i].name, l);
		wbytes(w, e->x[i].value, e->x[i].len);
	}
	for (i = 0; i < e->na; i++) {
		wrec(w, T_ACL, 1 + e->a[i].len);
		wint(w, (uint64_t)e->a[i].kind, 1);
		wbytes(w, e->a[i].data, e->a[i].len);
	}
	if (e->subtree != NULL) {
		wrec(w, T_SUBTREE, 32);
		wbytes(w, e->subtree, 32);
	}
	if (e->have & PC_HAVE_SPARSE)
		wrec(w, T_SPARSE, 0);
	wbytes(w, e->ext, e->extlen);
	wint(w, T_END, 1);
}

static void
wmanifest(struct wr *w, const pc_manifest_t *m)
{
	size_t i, j;

	wbytes(w, PCM_MAGIC2, 8);
	wint(w, m->n, 8);
	for (i = 0; i < m->n; i++) {
		const struct ment *e = &m->e[i];

		wint(w, e->plen, 2);
		wbytes(w, e->path, e->plen);
		wint(w, (uint64_t)e->type, 1);
		wint(w, e->mode, 4);
		wint(w, e->uid, 4);
		wint(w, e->gid, 4);
		wint(w, (uint64_t)e->mtime, 8);
		wint(w, e->nsec, 4);
		wint(w, e->size, 8);
		wint(w, e->tlen, 2);
		wbytes(w, e->target, e->tlen);
		wint(w, e->n, 4);
		for (j = 0; j < e->n && !w->ovf; j++) {
			wbytes(w, e->ids[j], 32);
			wint(w, e->lens[j], 4);
		}
		wrecords(w, e);
	}
}

int DLL_EXPORT
pc_manifest_encode(pc_manifest_t *m, void *buf, size_t *len)
{
	struct wr w = { NULL, 0, 0 };

	if (m == NULL || len == NULL)
		return (-1);
	msort(m);
	if (!check_tree(m))
		return (-1);	/* duplicate path, or an entry under a symlink or subtree */
	wmanifest(&w, m);
	if (w.ovf)
		return (-1);
	if (buf == NULL) {
		*len = w.n;
		return (0);
	}
	if (*len < w.n)
		return (-1);
	w.p = buf;
	w.n = 0;
	wmanifest(&w, m);
	*len = w.n;
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

/* Read a NUL-free name of n bytes, 1-255, as a C string. */
static char *
rname(struct rd *r, size_t n)
{
	const uchar_t *s = take(r, n);

	if (s == NULL || n == 0 || n > 255 || memchr(s, '\0', n) != NULL)
		return (NULL);
	return (dupn((const char *)s, n));
}

/* Append one more element of sz bytes to *arr (holding *n). */
static void *
push(void *arr, size_t n, size_t sz)
{
	if (n >= SIZE_MAX / sz - 1)
		return (NULL);
	return (realloc(arr, (n + 1) * sz));
}

/*
 * One known record's value of rl bytes into e. 0 or -1. Order, repeats
 * and what each type allows are checked by the caller and meta_ok().
 */
static int
rrecord(struct rd *r, struct ment *e, int tag, size_t rl)
{
	void *p;
	size_t nl;

	switch (tag) {
	case T_BTIME:
	case T_CTIME:
		if (rl != 12)
			return (-1);
		e->have |= tag == T_BTIME ? PC_HAVE_BTIME : PC_HAVE_CTIME;
		*(tag == T_BTIME ? &e->btime : &e->ctime) = (int64_t)get(r, 8);
		*(tag == T_BTIME ? &e->bnsec : &e->cnsec) = (uint32_t)get(r, 4);
		return (0);
	case T_INODE:
		if (rl != 16)
			return (-1);
		e->have |= PC_HAVE_INODE;
		e->dev = get(r, 8);
		e->ino = get(r, 8);
		return (0);
	case T_RDEV:
		if (rl != 8 || (e->type != PC_ENTRY_CHARDEV && e->type != PC_ENTRY_BLOCKDEV))
			return (-1);
		e->rdev = get(r, 8);
		return (0);
	case T_FLAGS:
		if (rl != 9 || (e->fkind = (int)get(r, 1)) == 0)
			return (-1);
		e->flags = get(r, 8);
		return (0);
	case T_OWNER:
		return ((e->owner = rname(r, rl)) == NULL ? -1 : 0);
	case T_GROUP:
		return ((e->group = rname(r, rl)) == NULL ? -1 : 0);
	case T_XATTR:
		if (rl < 2 || (nl = (size_t)get(r, 1)) == 0 || nl > rl - 1 ||
		    (p = push(e->x, e->nx, sizeof (*e->x))) == NULL)
			return (-1);
		e->x = p;
		memset(&e->x[e->nx], 0, sizeof (*e->x));
		e->x[e->nx].len = rl - 1 - nl;
		if ((e->x[e->nx++].name = rname(r, nl)) == NULL ||
		    (e->x[e->nx - 1].value = dupmem(take(r, rl - 1 - nl), rl - 1 - nl)) == NULL)
			return (-1);
		return (0);
	case T_ACL:
		if (rl < 2 || (p = push(e->a, e->na, sizeof (*e->a))) == NULL)
			return (-1);
		e->a = p;
		memset(&e->a[e->na], 0, sizeof (*e->a));
		e->a[e->na].kind = (int)get(r, 1);
		e->a[e->na].len = rl - 1;
		if ((e->a[e->na++].data = dupmem(take(r, rl - 1), rl - 1)) == NULL)
			return (-1);
		return (0);
	case T_SUBTREE:
		if (rl != 32 || (e->subtree = dupmem(take(r, 32), 32)) == NULL)
			return (-1);
		return (0);
	case T_SPARSE:
		if (rl != 0)
			return (-1);
		e->have |= PC_HAVE_SPARSE;
		return (0);
	}
	return (-1);
}

/*
 * An entry's records, up to its 0 byte. Known tags strictly increase
 * (XATTR and ACL may repeat), then unknown ones, kept as ext. 0 or -1.
 */
static int
rrecords(struct rd *r, struct ment *e)
{
	const uchar_t *start = NULL;
	int last = 0, tag, saw_rdev = 0;

	while ((tag = (int)get(r, 1)) != T_END && !r->bad) {
		size_t rl = (size_t)get(r, 4);
		int repeats = tag == T_XATTR || tag == T_ACL;

		if (r->bad || rl > r->left || tag < last ||
		    (tag == last && !repeats && tag <= T_LAST_KNOWN))
			return (-1);
		last = tag;
		if (tag > T_LAST_KNOWN) {
			/* Unknown: keep it, header and all; ext_ok() rejects critical. */
			if (start == NULL)
				start = r->p - REC_HDR;
			(void) take(r, rl);
			e->extlen = (size_t)(r->p - start);
		} else if (rrecord(r, e, tag, rl) != 0) {
			return (-1);
		}
		saw_rdev |= tag == T_RDEV;
	}
	if (r->bad)
		return (-1);
	if (e->extlen > 0 && (e->ext = dupmem(start, e->extlen)) == NULL)
		return (-1);
	/* The encoder always writes a device's RDEV: required, so bytes round-trip. */
	if ((e->type == PC_ENTRY_CHARDEV || e->type == PC_ENTRY_BLOCKDEV) && !saw_rdev)
		return (-1);
	return (0);
}

pc_manifest_t DLL_EXPORT *
pc_manifest_decode(const void *buf, size_t len)
{
	struct rd r = { buf, len, 0 };
	pc_manifest_t *m;
	uint64_t count, i, j;
	const uchar_t *s;
	int v2;

	if (buf == NULL || len < 16)
		return (NULL);
	if (memcmp(buf, PCM_MAGIC2, 8) == 0)
		v2 = 1;
	else if (memcmp(buf, PCM_MAGIC1, 8) == 0)
		v2 = 0;
	else
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
		if (r.bad || e->n > r.left / 36 || (!v2 && e->type > PC_ENTRY_SYMLINK))
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
		if (v2 && rrecords(&r, e) != 0)
			goto bad;
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
