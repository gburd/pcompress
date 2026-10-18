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
	b[4] = '2';
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

MU_TEST_SUITE(suite_manifest)
{
	MU_RUN_TEST(test_roundtrip);
	MU_RUN_TEST(test_add_refuses);
	MU_RUN_TEST(test_decode_hostile);
	MU_RUN_TEST(test_decode_tree);
	MU_RUN_TEST(test_decode_flips);
}

int
main(void)
{
	MU_RUN_SUITE(suite_manifest);
	MU_REPORT();
	return (MU_EXIT_CODE);
}
