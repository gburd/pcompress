/*
 * Snapshot manifests (pc_manifest_*), through the public header only.
 * The decoder is fed hostile input: every refusal below is a way a
 * crafted manifest could otherwise steer a restore.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>
#include "minunit.h"

static uint8_t ids[4 * 32];
static uint32_t lens[4] = { 100, 200, 300, 400 };

static pc_entry_t
ent(const char *path, int type)
{
	pc_entry_t e;

	memset(&e, 0, sizeof (e));
	e.path = path;
	e.type = type;
	e.mode = type == PC_ENTRY_DIR ? 040755 : 0100644;
	e.uid = 1000;
	e.gid = 100;
	e.mtime = 1700000000;
	e.mtime_nsec = 123456789;
	if (type == PC_ENTRY_FILE) {
		e.nchunks = 4;
		e.ids = ids;
		e.lens = lens;
		e.size = 1000;
	}
	if (type == PC_ENTRY_SYMLINK)
		e.target = "../elsewhere";
	return (e);
}

static unsigned char *
encode(pc_manifest_t *m, size_t *len)
{
	unsigned char *b;

	if (pc_manifest_encode(m, NULL, len) != 0 || (b = malloc(*len)) == NULL)
		return (NULL);
	if (pc_manifest_encode(m, b, len) != 0) {
		free(b);
		return (NULL);
	}
	return (b);
}

MU_TEST(test_roundtrip)
{
	pc_manifest_t *m = pc_manifest_new(), *d;
	pc_entry_t e;
	unsigned char *b, *b2;
	size_t len, len2, i;

	for (i = 0; i < sizeof (ids); i++)
		ids[i] = (uint8_t)(i * 13);
	/* Added out of order: kept sorted by path bytes. */
	e = ent("src/main.c", PC_ENTRY_FILE);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	e = ent("src", PC_ENTRY_DIR);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	e = ent("README", PC_ENTRY_FILE);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	e = ent("link", PC_ENTRY_SYMLINK);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	e = ent("empty", PC_ENTRY_FILE);
	e.nchunks = 0;
	e.size = 0;
	e.target = "";	/* no target: read back as NULL */
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	mu_assert(pc_manifest_find(m, "empty", &e) >= 0 && e.target == NULL, "empty target is NULL");
	mu_assert(pc_manifest_count(m) == 5, "count");
	b = encode(m, &len);
	mu_assert(b != NULL, "encode");
	d = pc_manifest_decode(b, len);
	mu_assert(d != NULL && pc_manifest_count(d) == 5, "decode");
	mu_assert_int_eq(0, pc_manifest_get(d, 0, &e));
	mu_assert(strcmp(e.path, "README") == 0, "sorted: README first");
	mu_assert_int_eq(0, pc_manifest_get(d, 4, &e));
	mu_assert(strcmp(e.path, "src/main.c") == 0, "sorted: src/main.c last");
	mu_assert(e.nchunks == 4 && e.size == 1000 && memcmp(e.ids, ids, sizeof (ids)) == 0 &&
	    e.lens[3] == 400 && e.mode == 0100644 && e.uid == 1000 && e.gid == 100 &&
	    e.mtime == 1700000000 && e.mtime_nsec == 123456789, "fields");
	mu_assert(pc_manifest_find(d, "link", &e) == 2 && e.type == PC_ENTRY_SYMLINK &&
	    strcmp(e.target, "../elsewhere") == 0, "find symlink");
	mu_assert(pc_manifest_find(d, "nope", NULL) == -1, "find absent");
	mu_assert(pc_manifest_find(d, "src/", NULL) == -1, "find is exact");
	/* Re-encoding the decoded manifest gives the same bytes. */
	b2 = encode(d, &len2);
	mu_assert(b2 != NULL && len2 == len && memcmp(b, b2, len) == 0, "canonical");
	/* Every truncation is refused. */
	for (i = 0; i < len; i++) {
		pc_manifest_t *t = pc_manifest_decode(b, i);

		mu_assert(t == NULL, "truncated manifest accepted");
	}
	/* Trailing bytes are refused. */
	b2 = realloc(b2, len + 1);
	b2[len] = 0;
	mu_assert(pc_manifest_decode(b2, len + 1) == NULL, "trailing byte accepted");
	free(b);
	free(b2);
	pc_manifest_free(m);
	pc_manifest_free(d);
}

MU_TEST(test_add_refuses)
{
	static const char *bad[] = { "/etc/passwd", "a/../b", "..", ".", "a//b", "a/",
	    "./a", "a/.", "", NULL };
	pc_manifest_t *m = pc_manifest_new();
	pc_entry_t e;
	int i;

	for (i = 0; bad[i] != NULL; i++) {
		e = ent(bad[i], PC_ENTRY_FILE);
		mu_assert(pc_manifest_add(m, &e) == -1, bad[i]);
	}
	e = ent("ok", PC_ENTRY_FILE);
	e.size = 999;
	mu_assert(pc_manifest_add(m, &e) == -1, "size != sum of lens");
	e = ent("ok", PC_ENTRY_DIR);
	e.size = 1;
	mu_assert(pc_manifest_add(m, &e) == -1, "dir with size");
	e = ent("ok", PC_ENTRY_SYMLINK);
	e.target = NULL;
	mu_assert(pc_manifest_add(m, &e) == -1, "symlink without target");
	e.target = "";
	mu_assert(pc_manifest_add(m, &e) == -1, "symlink with empty target");
	e = ent("ok", 9);
	mu_assert(pc_manifest_add(m, &e) == -1, "unknown type");
	e = ent("ok", PC_ENTRY_FILE);
	e.mtime_nsec = 1000000000;
	mu_assert(pc_manifest_add(m, &e) == -1, "nsec out of range");
	mu_assert(pc_manifest_count(m) == 0, "nothing added");
	/* Duplicates and entries under a symlink: refused at encode. */
	e = ent("a", PC_ENTRY_FILE);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	{
		size_t len;

		mu_assert(pc_manifest_encode(m, NULL, &len) == -1, "duplicate encoded");
		pc_manifest_free(m);
		m = pc_manifest_new();
		e = ent("l", PC_ENTRY_SYMLINK);
		pc_manifest_add(m, &e);
		e = ent("l/x", PC_ENTRY_FILE);
		pc_manifest_add(m, &e);
		mu_assert(pc_manifest_encode(m, NULL, &len) == -1, "entry under a symlink encoded");
	}
	pc_manifest_free(m);
}

/* Patch one entry's path in an encoded single-entry manifest. */
static unsigned char *
one(const char *path, int type, size_t *len)
{
	pc_manifest_t *m = pc_manifest_new();
	pc_entry_t e = ent("x", type);
	unsigned char *b;
	size_t pl = strlen(path);

	pc_manifest_add(m, &e);
	b = encode(m, len);
	pc_manifest_free(m);
	/* Header 16, then u16 path length = 1, then "x": splice the new path. */
	b = realloc(b, *len + pl);
	memmove(b + 16 + 2 + pl, b + 16 + 2 + 1, *len - 16 - 2 - 1);
	b[16] = (unsigned char)pl;
	b[17] = (unsigned char)(pl >> 8);
	memcpy(b + 18, path, pl);
	*len = *len - 1 + pl;
	return (b);
}

MU_TEST(test_decode_hostile)
{
	static const char *bad[] = { "/abs", "../up", "a/../../b", "a//b", "a/", ".",
	    NULL };
	unsigned char *b;
	size_t len, i;
	pc_manifest_t *d;
	uint64_t big = (uint64_t)1 << 40;

	/* The splice itself works (control). */
	b = one("fine/name", PC_ENTRY_FILE, &len);
	d = pc_manifest_decode(b, len);
	mu_assert(d != NULL, "control: spliced path decodes");
	pc_manifest_free(d);
	free(b);
	for (i = 0; bad[i] != NULL; i++) {
		b = one(bad[i], PC_ENTRY_FILE, &len);
		mu_assert(pc_manifest_decode(b, len) == NULL, bad[i]);
		free(b);
	}
	/* NUL inside a path. */
	b = one("a\0b", PC_ENTRY_FILE, &len);
	b[16] = 3;
	memcpy(b + 18, "a\0b", 3);
	mu_assert(pc_manifest_decode(b, len) == NULL, "NUL in path");
	free(b);
	/* Absurd entry count: refused without allocating it. */
	b = one("x", PC_ENTRY_FILE, &len);
	for (i = 0; i < 8; i++)
		b[8 + i] = (unsigned char)(big >> (8 * i));
	mu_assert(pc_manifest_decode(b, len) == NULL, "huge count");
	free(b);
	/* Absurd chunk count: the u32 before the first id. */
	b = one("x", PC_ENTRY_FILE, &len);
	{
		size_t at = 16 + 2 + 1 + 1 + 12 + 12 + 8 + 2;

		b[at] = b[at + 1] = b[at + 2] = b[at + 3] = 0xff;
	}
	mu_assert(pc_manifest_decode(b, len) == NULL, "huge chunk count");
	free(b);
	/* Wrong magic, NULL, too short. */
	b = one("x", PC_ENTRY_FILE, &len);
	b[4] = '9';
	mu_assert(pc_manifest_decode(b, len) == NULL, "magic");
	free(b);
	mu_assert(pc_manifest_decode(NULL, 100) == NULL, "NULL");
	mu_assert(pc_manifest_decode("PCMF1", 5) == NULL, "short");
}

/* Two entries spliced by hand: out of order, duplicate, under a symlink. */
MU_TEST(test_decode_tree)
{
	pc_manifest_t *m = pc_manifest_new();
	pc_entry_t e;
	unsigned char *b;
	size_t len;
	char *p;

	e = ent("l", PC_ENTRY_SYMLINK);
	pc_manifest_add(m, &e);
	e = ent("m", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	b = encode(m, &len);
	pc_manifest_free(m);
	m = pc_manifest_decode(b, len);
	mu_assert(m != NULL, "control decodes");
	pc_manifest_free(m);
	/* Rename "m" to "l" (duplicate) and then to "a" (out of order). */
	p = memchr(b + 19, 'm', len - 19);
	mu_assert(p != NULL, "find second path");
	*p = 'l';
	mu_assert(pc_manifest_decode(b, len) == NULL, "duplicate path decoded");
	*p = 'a';
	mu_assert(pc_manifest_decode(b, len) == NULL, "unsorted decoded");
	free(b);
	/* Under a symlink with entries between: "l", "l-x", "l.y", "l/m". */
	m = pc_manifest_new();
	e = ent("l", PC_ENTRY_SYMLINK);
	pc_manifest_add(m, &e);
	e = ent("l-x", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	e = ent("l.y", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	e = ent("l0m", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	b = encode(m, &len);
	mu_assert(b != NULL, "control: siblings of a symlink are fine");
	pc_manifest_free(m);
	p = memchr(b + 19, '0', len - 19);
	*p = '/';	/* "l0m" -> "l/m", still sorted last */
	mu_assert(pc_manifest_decode(b, len) == NULL, "entry under a symlink, after siblings");
	free(b);
	/* "l" is a symlink: "l/m" would be written through it. */
	m = pc_manifest_new();
	e = ent("l", PC_ENTRY_SYMLINK);
	pc_manifest_add(m, &e);
	e = ent("lxm", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	b = encode(m, &len);
	pc_manifest_free(m);
	p = memchr(b + 19, 'x', len - 19);
	*p = '/';
	mu_assert(pc_manifest_decode(b, len) == NULL, "entry under a symlink decoded");
	free(b);
}

/* Independent check: relative, no empty, "." or ".." component. */
static int
safe_path(const char *p)
{
	const char *s = p, *q;

	if (*p == '\0')
		return (0);
	for (;;) {
		size_t n;

		q = strchr(s, '/');
		n = q ? (size_t)(q - s) : strlen(s);
		if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
			return (0);
		if (q == NULL)
			return (1);
		s = q + 1;
	}
}

/* Every single-byte change decodes to something valid or is refused, and
 * never crashes (ASan run). */
MU_TEST(test_decode_flips)
{
	pc_manifest_t *m = pc_manifest_new(), *d;
	pc_entry_t e;
	unsigned char *b;
	size_t len, i;
	int bit;

	e = ent("dir", PC_ENTRY_DIR);
	pc_manifest_add(m, &e);
	e = ent("dir/file", PC_ENTRY_FILE);
	pc_manifest_add(m, &e);
	e = ent("ln", PC_ENTRY_SYMLINK);
	pc_manifest_add(m, &e);
	b = encode(m, &len);
	pc_manifest_free(m);
	for (i = 0; i < len; i++)
		for (bit = 0; bit < 8; bit++) {
			b[i] ^= (unsigned char)(1 << bit);
			d = pc_manifest_decode(b, len);
			if (d != NULL) {
				size_t k;

				for (k = 0; k < pc_manifest_count(d); k++) {
					mu_assert_int_eq(0, pc_manifest_get(d, k, &e));
					mu_assert(safe_path(e.path), "unsafe path decoded");
				}
				pc_manifest_free(d);
			}
			b[i] ^= (unsigned char)(1 << bit);
		}
	free(b);
}

/*
 * Entries of another size: a program built against a newer header (with
 * fields this library lacks) or an older one. Unknown fields read as zero;
 * an entry that sets one is refused rather than silently dropped.
 */
MU_TEST(test_entry_size)
{
	struct { pc_entry_t e; uint64_t future[2]; } big;
	pc_manifest_t *m = pc_manifest_new();

	memset(&big, 0, sizeof (big));
	big.e = ent("a", PC_ENTRY_FILE);
	mu_assert_int_eq(0, pc_manifest_add_sz(m, &big.e, sizeof (big)));
	big.e.path = "b";
	big.future[1] = 1;
	mu_assert_int_eq(-1, pc_manifest_add_sz(m, &big.e, sizeof (big)));
	mu_assert_int_eq(-1, pc_manifest_add_sz(m, &big.e, sizeof (pc_entry_t) - 1));

	memset(&big, 0xff, sizeof (big));
	mu_assert_int_eq(0, pc_manifest_get_sz(m, 0, &big.e, sizeof (big)));
	mu_assert(strcmp(big.e.path, "a") == 0 && big.e.size == 1000, "get_sz fields");
	mu_assert(big.future[0] == 0 && big.future[1] == 0, "unknown fields not zeroed");
	memset(&big, 0xff, sizeof (big));
	mu_assert(pc_manifest_find_sz(m, "a", &big.e, sizeof (big)) == 0 &&
	    big.future[0] == 0 && big.e.nchunks == 4, "find_sz");
	mu_assert_int_eq(-1, pc_manifest_get_sz(m, 0, &big.e, 8));

	/* The plain names, as old binaries call them, still work. */
	mu_assert_int_eq(0, (pc_manifest_get)(m, 0, &big.e));
	mu_assert(strcmp(big.e.path, "a") == 0, "old pc_manifest_get");
	pc_manifest_free(m);
}

/* Little-endian n-byte v at p; returns the end. */
static unsigned char *
le(unsigned char *p, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(v >> (8 * i));
	return (p + n);
}

/*
 * Every format 2 field survives encode and decode, xattrs and ACLs come
 * back sorted, and the bytes are canonical: adding in another order, or
 * re-encoding what was decoded, gives the same bytes.
 */
MU_TEST(test_v2_roundtrip)
{
	static const uint8_t sub[32] = { 9, 8, 7 };
	pc_xattr_t x[3] = { { "user.z", "zz", 2 }, { "security.selinux", "ctx", 3 },
	    { "user.empty", NULL, 0 } };
	pc_acl_t a[2] = { { PC_ACL_POSIX_DEFAULT, "d:u::rwx", 8 },
	    { PC_ACL_POSIX_ACCESS, "u::rw-", 6 } };
	pc_manifest_t *m = pc_manifest_new(), *m2 = pc_manifest_new(), *d;
	pc_entry_t e, f;
	unsigned char *b, *b2;
	size_t len, len2, i;

	e = ent("f", PC_ENTRY_FILE);
	e.have = PC_HAVE_BTIME | PC_HAVE_CTIME | PC_HAVE_INODE | PC_HAVE_SPARSE;
	e.btime = -5;
	e.btime_nsec = 1;
	e.ctime = 1700000001;
	e.ctime_nsec = 999999999;
	e.dev = 0x1234;
	e.ino = 0xfedcba9876543210ULL;
	e.flags_kind = PC_FLAGS_LINUX;
	e.flags = 0x10;
	e.owner = "greg";
	e.group = "staff";
	e.nxattrs = 3;
	e.xattrs = x;
	e.nacls = 2;
	e.acls = a;
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	f = ent("hl", PC_ENTRY_HARDLINK);
	f.target = "f";
	mu_assert_int_eq(0, pc_manifest_add(m, &f));
	f = ent("tty", PC_ENTRY_CHARDEV);
	f.rdev = 0x0405;
	mu_assert_int_eq(0, pc_manifest_add(m, &f));
	f = ent("fifo", PC_ENTRY_FIFO);
	mu_assert_int_eq(0, pc_manifest_add(m, &f));
	f = ent("sub", PC_ENTRY_DIR);
	f.subtree = sub;
	mu_assert_int_eq(0, pc_manifest_add(m, &f));
	b = encode(m, &len);
	mu_assert(b != NULL && memcmp(b, "PCMF2\0\0\0", 8) == 0, "encodes as PCMF2");
	d = pc_manifest_decode(b, len);
	mu_assert(d != NULL && pc_manifest_count(d) == 5, "v2 decodes");
	mu_assert(pc_manifest_find(d, "f", &f) >= 0, "find f");
	mu_assert(f.have == e.have && f.btime == -5 && f.btime_nsec == 1 &&
	    f.ctime == 1700000001 && f.ctime_nsec == 999999999 && f.dev == 0x1234 &&
	    f.ino == e.ino && f.flags_kind == PC_FLAGS_LINUX && f.flags == 0x10 &&
	    strcmp(f.owner, "greg") == 0 && strcmp(f.group, "staff") == 0, "v2 fields");
	mu_assert(f.nxattrs == 3 && strcmp(f.xattrs[0].name, "security.selinux") == 0 &&
	    f.xattrs[0].len == 3 && memcmp(f.xattrs[0].value, "ctx", 3) == 0 &&
	    strcmp(f.xattrs[1].name, "user.empty") == 0 && f.xattrs[1].len == 0 &&
	    strcmp(f.xattrs[2].name, "user.z") == 0, "xattrs, sorted by name");
	mu_assert(f.nacls == 2 && f.acls[0].kind == PC_ACL_POSIX_ACCESS &&
	    f.acls[1].len == 8 && memcmp(f.acls[1].data, "d:u::rwx", 8) == 0, "acls, by kind");
	mu_assert(f.subtree == NULL && f.ext == NULL && f.extlen == 0 && f.rdev == 0, "absent");
	mu_assert(pc_manifest_find(d, "hl", &f) >= 0 && f.type == PC_ENTRY_HARDLINK &&
	    strcmp(f.target, "f") == 0, "hardlink");
	mu_assert(pc_manifest_find(d, "tty", &f) >= 0 && f.rdev == 0x0405, "rdev");
	mu_assert(pc_manifest_find(d, "sub", &f) >= 0 && f.subtree != NULL &&
	    memcmp(f.subtree, sub, 32) == 0, "subtree");
	b2 = encode(d, &len2);
	mu_assert(b2 != NULL && len2 == len && memcmp(b, b2, len) == 0, "v2 canonical");
	free(b2);
	/* The same entries added in another order, xattrs reversed: same bytes. */
	for (i = 5; i-- > 0;) {
		mu_assert_int_eq(0, pc_manifest_get(d, i, &f));
		if (strcmp(f.path, "f") == 0) {
			pc_xattr_t rx[3] = { x[2], x[0], x[1] };

			f.xattrs = rx;
			mu_assert_int_eq(0, pc_manifest_add(m2, &f));
		} else {
			mu_assert_int_eq(0, pc_manifest_add(m2, &f));
		}
	}
	b2 = encode(m2, &len2);
	mu_assert(b2 != NULL && len2 == len && memcmp(b, b2, len) == 0, "order-independent bytes");
	free(b2);
	for (i = 0; i < len; i++)
		mu_assert(pc_manifest_decode(b, i) == NULL, "truncated v2 accepted");
	/* A program built before 4.4 sees the version 1 fields only. */
	memset(&f, 0x55, sizeof (f));
	mu_assert_int_eq(0, (pc_manifest_get)(d, 0, &f));
	mu_assert(strcmp(f.path, "f") == 0 && f.size == 1000 && f.have == 0x55555555,
	    "v1 caller: v2 fields untouched");
	free(b);
	pc_manifest_free(d);
	pc_manifest_free(m);
	pc_manifest_free(m2);
}

/* Entries add refuses: values that would not round-trip, or unsafe ones. */
MU_TEST(test_v2_add_refuses)
{
	static const uint8_t sub[32] = { 1 };
	static const unsigned char crit[5] = { 0x90, 0, 0, 0, 0 };
	char longname[300];
	pc_xattr_t dup[2] = { { "user.a", "1", 1 }, { "user.a", "2", 1 } };
	pc_xattr_t bad[1] = { { NULL, NULL, 0 } };
	pc_manifest_t *m = pc_manifest_new();
	pc_entry_t e;

	memset(longname, 'n', sizeof (longname) - 1);
	longname[sizeof (longname) - 1] = '\0';
#define	REFUSED(what, stmt) do { e = ent("x", PC_ENTRY_FILE); stmt; \
	mu_assert(pc_manifest_add(m, &e) == -1, what); } while (0)
	REFUSED("btime without its bit", e.btime = 1);
	REFUSED("inode without its bit", e.ino = 1);
	REFUSED("unknown have bit", e.have = 0x100);
	REFUSED("btime nsec", (e.have = PC_HAVE_BTIME, e.btime_nsec = 1000000000));
	REFUSED("rdev on a file", e.rdev = 1);
	REFUSED("subtree on a file", e.subtree = sub);
	REFUSED("flags without kind", e.flags = 1);
	REFUSED("empty owner", e.owner = "");
	REFUSED("long group", e.group = longname);
	REFUSED("duplicate xattr", (e.nxattrs = 2, e.xattrs = dup));
	REFUSED("xattr without name", (e.nxattrs = 1, e.xattrs = bad));
	REFUSED("xattrs NULL", e.nxattrs = 1);
	REFUSED("critical unknown record", (e.ext = crit, e.extlen = 5));
	REFUSED("truncated unknown record", (e.ext = crit + 1, e.extlen = 3));
	REFUSED("hardlink without target", (e = ent("x", PC_ENTRY_DIR), e.type = PC_ENTRY_HARDLINK));
	REFUSED("hardlink to ..", (e = ent("x", PC_ENTRY_DIR), e.type = PC_ENTRY_HARDLINK,
	    e.target = "../x"));
	REFUSED("fifo with chunks", e.type = PC_ENTRY_FIFO);
	REFUSED("sparse dir", (e = ent("x", PC_ENTRY_DIR), e.have = PC_HAVE_SPARSE));
	REFUSED("unknown type", e.type = 9);
#undef REFUSED
	/* Nothing under a directory whose contents are another manifest. */
	e = ent("d", PC_ENTRY_DIR);
	e.subtree = sub;
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	e = ent("d/f", PC_ENTRY_FILE);
	mu_assert_int_eq(0, pc_manifest_add(m, &e));
	{
		size_t len;

		mu_assert(pc_manifest_encode(m, NULL, &len) == -1, "entry under a subtree encoded");
	}
	pc_manifest_free(m);
}

/* A single-entry manifest whose records (its last byte is their end) are rec. */
static unsigned char *
with_records(int type, const unsigned char *rec, size_t rl, size_t *len)
{
	pc_manifest_t *m = pc_manifest_new();
	pc_entry_t e = ent("x", type);
	unsigned char *b;

	if (type == PC_ENTRY_CHARDEV)
		e.rdev = 1;
	pc_manifest_add(m, &e);
	b = encode(m, len);
	pc_manifest_free(m);
	b = realloc(b, *len + rl);
	if (type == PC_ENTRY_CHARDEV)
		*len -= 13;	/* drop the RDEV record too */
	memcpy(b + *len - 1, rec, rl);
	b[*len - 1 + rl] = 0;
	*len += rl;
	return (b);
}

/*
 * Records from a newer writer: unknown ones are kept and written back;
 * critical, malformed, out-of-order or misplaced ones are refused.
 */
MU_TEST(test_v2_records)
{
	unsigned char rec[64], *b, *b2, *p;
	size_t len, len2;
	pc_manifest_t *d;
	pc_entry_t e;

	/* Unknown tags 50 and 60, non-critical: kept as ext, same bytes again. */
	p = le(rec, 50, 1);
	p = le(p, 2, 4);
	p = le(p, 0x4241, 2);
	p = le(p, 60, 1);
	p = le(p, 0, 4);
	b = with_records(PC_ENTRY_FILE, rec, (size_t)(p - rec), &len);
	d = pc_manifest_decode(b, len);
	mu_assert(d != NULL, "unknown records decode");
	mu_assert(pc_manifest_get(d, 0, &e) == 0 && e.extlen == 12 &&
	    memcmp(e.ext, rec, 12) == 0, "unknown records kept");
	b2 = encode(d, &len2);
	mu_assert(b2 != NULL && len2 == len && memcmp(b, b2, len) == 0, "unknown records written back");
	free(b2);
	pc_manifest_free(d);
	free(b);
#define	BAD(what, type, n) do { b = with_records(type, rec, n, &len); \
	mu_assert(pc_manifest_decode(b, len) == NULL, what); free(b); } while (0)
	p = le(rec, 0x90, 1);
	p = le(p, 0, 4);
	BAD("critical unknown record", PC_ENTRY_FILE, 5);
	le(le(le(rec, 60, 1), 0, 4), 50, 1);
	le(rec + 6, 0, 4);
	BAD("unknown records out of order", PC_ENTRY_FILE, 10);
	le(le(rec, 1, 1), 12, 4);
	memset(rec + 5, 0, 12);
	memcpy(rec + 17, rec, 17);
	BAD("BTIME twice", PC_ENTRY_FILE, 34);
	le(le(rec, 2, 1), 12, 4);
	memset(rec + 5, 0, 12);
	le(le(rec + 17, 1, 1), 12, 4);
	memset(rec + 22, 0, 12);
	BAD("CTIME before BTIME", PC_ENTRY_FILE, 34);
	le(le(rec, 1, 1), 11, 4);
	BAD("BTIME of the wrong length", PC_ENTRY_FILE, 16);
	le(le(rec, 1, 1), 1000, 4);
	BAD("record past the end", PC_ENTRY_FILE, 5);
	le(le(rec, 4, 1), 8, 4);
	memset(rec + 5, 0, 8);
	BAD("RDEV on a file", PC_ENTRY_FILE, 13);
	BAD("device without RDEV", PC_ENTRY_CHARDEV, 0);
	le(le(rec, 10, 1), 32, 4);
	memset(rec + 5, 1, 32);
	BAD("SUBTREE on a file", PC_ENTRY_FILE, 37);
	le(le(le(rec, 5, 1), 9, 4), 0, 1);
	memset(rec + 6, 0, 8);
	BAD("FLAGS of kind 0", PC_ENTRY_FILE, 14);
	le(le(rec, 6, 1), 3, 4);
	memcpy(rec + 5, "a\0b", 3);
	BAD("NUL in owner", PC_ENTRY_FILE, 8);
	le(le(le(rec, 8, 1), 3, 4), 5, 1);
	memcpy(rec + 6, "ab", 2);
	BAD("xattr name past its record", PC_ENTRY_FILE, 8);
	le(le(le(rec, 8, 1), 4, 4), 1, 1);
	memcpy(rec + 6, "bv", 2);
	memcpy(rec + 8, rec, 8);
	rec[14] = 'a';	/* "b" then "a" */
	BAD("xattrs out of order", PC_ENTRY_FILE, 16);
	le(le(le(rec, 9, 1), 1, 4), 1, 1);
	BAD("empty ACL", PC_ENTRY_FILE, 6);
	le(le(rec, 11, 1), 0, 4);
	BAD("SPARSE on a dir", PC_ENTRY_DIR, 5);
	/* The control: a well-formed SPARSE on a file decodes. */
	b = with_records(PC_ENTRY_FILE, rec, 5, &len);
	d = pc_manifest_decode(b, len);
	mu_assert(d != NULL && pc_manifest_get(d, 0, &e) == 0 && e.have == PC_HAVE_SPARSE,
	    "SPARSE control");
	pc_manifest_free(d);
	free(b);
#undef BAD
}

/*
 * Manifests written by 4.2 and 4.3 (PCMF1) still decode, and encode again
 * as PCMF2. Built by hand, so this does not depend on today's encoder.
 */
MU_TEST(test_v1_compat)
{
	unsigned char b[256], *p, *b2;
	size_t len2;
	pc_manifest_t *d;
	pc_entry_t e;

	memcpy(b, "PCMF1\0\0\0", 8);
	p = le(b + 8, 2, 8);
	/* "d", a directory. */
	p = le(p, 1, 2);
	*p++ = 'd';
	p = le(p, PC_ENTRY_DIR, 1);
	p = le(p, 040755, 4);
	p = le(p, 1, 4);
	p = le(p, 2, 4);
	p = le(p, 1600000000, 8);
	p = le(p, 7, 4);
	p = le(p, 0, 8);
	p = le(p, 0, 2);
	p = le(p, 0, 4);
	/* "d/f", a file of one 5-byte chunk. */
	p = le(p, 3, 2);
	memcpy(p, "d/f", 3);
	p = le(p + 3, PC_ENTRY_FILE, 1);
	p = le(p, 0100600, 4);
	p = le(p, 1, 4);
	p = le(p, 2, 4);
	p = le(p, 1600000001, 8);
	p = le(p, 0, 4);
	p = le(p, 5, 8);
	p = le(p, 0, 2);
	p = le(p, 1, 4);
	memset(p, 0xab, 32);
	p = le(p + 32, 5, 4);
	d = pc_manifest_decode(b, (size_t)(p - b));
	mu_assert(d != NULL && pc_manifest_count(d) == 2, "PCMF1 decodes");
	mu_assert(pc_manifest_find(d, "d/f", &e) == 1 && e.size == 5 && e.lens[0] == 5 &&
	    e.ids[31] == 0xab && e.mtime == 1600000001 && e.have == 0 && e.owner == NULL,
	    "PCMF1 fields");
	b2 = encode(d, &len2);
	mu_assert(b2 != NULL && memcmp(b2, "PCMF2\0\0\0", 8) == 0 &&
	    len2 == (size_t)(p - b) + 2, "PCMF1 re-encodes as PCMF2, one end byte per entry");
	free(b2);
	pc_manifest_free(d);
	/* Version 1 has no hardlinks, devices or FIFOs. */
	b[16 + 3] = PC_ENTRY_FIFO;
	mu_assert(pc_manifest_decode(b, (size_t)(p - b)) == NULL, "PCMF1 with a v2 type");
}

MU_TEST_SUITE(suite_manifest)
{
	MU_RUN_TEST(test_roundtrip);
	MU_RUN_TEST(test_add_refuses);
	MU_RUN_TEST(test_decode_hostile);
	MU_RUN_TEST(test_decode_tree);
	MU_RUN_TEST(test_decode_flips);
	MU_RUN_TEST(test_entry_size);
	MU_RUN_TEST(test_v2_roundtrip);
	MU_RUN_TEST(test_v2_add_refuses);
	MU_RUN_TEST(test_v2_records);
	MU_RUN_TEST(test_v1_compat);
}

int
main(void)
{
	MU_RUN_SUITE(suite_manifest);
	MU_REPORT();
	return (MU_EXIT_CODE);
}
