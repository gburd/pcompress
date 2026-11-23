/*
 * Seed corpora for fuzz_chunk and fuzz_manifest, written with the public
 * API so they follow the formats as they change:
 *
 *   fuzz_seeds <dir>
 *
 * writes <dir>/chunk/ (forged-mode and raw-mode inputs for every
 * algorithm this build has, an encrypted object, a key file) and
 * <dir>/manifest/ (an empty manifest, one with a file, a directory and
 * a symlink, and one with every kind of metadata record). A fuzzer that starts from valid inputs reaches the
 * decoders' inner paths far sooner than one that must find the magic
 * bytes first. <dir>/seek_index/ holds seek index bodies for
 * fuzz_seek_index, built byte by byte (the index has no public API).
 * Run by "make fuzz-build".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>

static void
put(const char *dir, const char *name, int prefix, const void *b, size_t n)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof (path), "%s/%s", dir, name);
	if ((f = fopen(path, "wb")) == NULL) {
		perror(path);
		exit(1);
	}
	if (prefix >= 0)
		fputc(prefix, f);
	if (fwrite(b, 1, n, f) != n || fclose(f) != 0) {
		perror(path);
		exit(1);
	}
}

static void
quiet(void *a, int l, const char *m)
{
	(void) a; (void) l; (void) m;
}

/*
 * fuzz_chunk's first byte picks the mode: 0 builds a forged object from
 * (algorithm id, level, 20-bit length, payload); 2 opens the rest as is,
 * with a key when bit 2 is set (6); 3 opens a key file.
 */
static void
chunk_seeds(const char *dir)
{
	static const char *algos[] = { "zstd", "lz4", "lzma", "zlib", "bzip2",
	    "ppmd", "lzfx", "libbsc", "adapt", "adapt2", "lzmaMt", NULL };
	static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t in[3000], obj[4096], kf[256], k[32];
	char name[64];
	size_t i, ol;
	int a;

	for (i = 0; i < sizeof (in); i++)
		in[i] = (uint8_t)("the quick brown fox "[(i * 7 + i / 200) % 20]);
	for (a = 0; algos[a] != NULL; a++) {
		pc_ctx_t *c = pc_new();

		pc_set_log(c, quiet, NULL, PC_LOG_NONE);
		if (pc_set_algorithm(c, algos[a]) != 0 || pc_set_level(c, 6) != 0) {
			pc_free(c);
			continue;	/* not in this build */
		}
		ol = sizeof (obj);
		if (pc_chunk_seal(c, in, sizeof (in), obj, &ol) == 0) {
			uint8_t f[4096];

			/* Forged mode: header fields from the object, then its payload. */
			f[0] = obj[6] | (obj[5] & 0x02 ? 0x80 : 0);
			f[1] = obj[7];
			f[2] = obj[8];
			f[3] = obj[9];
			f[4] = obj[10];
			memcpy(f + 5, obj + 40, ol - 72);
			snprintf(name, sizeof (name), "forged-%s", algos[a]);
			put(dir, name, 0, f, 5 + ol - 72);
			snprintf(name, sizeof (name), "object-%s", algos[a]);
			put(dir, name, 2, obj, ol);
		}
		pc_free(c);
	}

	{
		pc_ctx_t *c = pc_new();

		pc_set_log(c, quiet, NULL, PC_LOG_NONE);
		pc_set_key(c, key, sizeof (key));
		ol = sizeof (obj);
		if (pc_chunk_seal(c, in, sizeof (in), obj, &ol) == 0)
			put(dir, "object-encrypted", 6, obj, ol);
		pc_free(c);
	}
	if (pc_keyfile_size() <= sizeof (kf) && pc_keyfile_create("pw", 2, k, kf) == 0)
		put(dir, "keyfile", 3, kf, pc_keyfile_size());
}

static void
manifest_seeds(const char *dir)
{
	static const uint8_t ids[2][32] = { { 1 }, { 2 } };
	static const uint32_t lens[2] = { 100, 23 };
	pc_entry_t e[3];
	pc_manifest_t *m;
	size_t len = 0;
	uint8_t *b;
	int i;

	m = pc_manifest_new();
	if (m == NULL || pc_manifest_encode(m, NULL, &len) != 0 ||
	    (b = malloc(len)) == NULL || pc_manifest_encode(m, b, &len) != 0)
		exit(1);
	put(dir, "empty", -1, b, len);
	free(b);
	pc_manifest_free(m);

	memset(e, 0, sizeof (e));
	e[0].path = "dir";
	e[0].type = PC_ENTRY_DIR;
	e[0].mode = 0755;
	e[1].path = "dir/file";
	e[1].type = PC_ENTRY_FILE;
	e[1].mode = 0644;
	e[1].mtime = 1700000000;
	e[1].size = 123;
	e[1].nchunks = 2;
	e[1].ids = &ids[0][0];
	e[1].lens = lens;
	e[2].path = "link";
	e[2].type = PC_ENTRY_SYMLINK;
	e[2].mode = 0777;
	e[2].target = "dir/file";
	m = pc_manifest_new();
	for (i = 0; i < 3; i++)
		if (pc_manifest_add(m, &e[i]) != 0)
			exit(1);
	len = 0;
	if (pc_manifest_encode(m, NULL, &len) != 0 || (b = malloc(len)) == NULL ||
	    pc_manifest_encode(m, b, &len) != 0)
		exit(1);
	put(dir, "three-entries", -1, b, len);
	free(b);
	pc_manifest_free(m);

	/* Every record type, so mutations start from each. */
	{
		static const uint8_t sub[32] = { 3 };
		static const uint8_t ext[7] = { 77, 2, 0, 0, 0, 'h', 'i' };
		pc_xattr_t x[2] = { { "user.a", "1", 1 }, { "user.b", "", 0 } };
		pc_acl_t a[1] = { { PC_ACL_NFS4, "owner@:rw::allow", 16 } };

		e[1].have = PC_HAVE_BTIME | PC_HAVE_CTIME | PC_HAVE_INODE | PC_HAVE_SPARSE;
		e[1].btime = 1;
		e[1].ctime = 2;
		e[1].dev = 3;
		e[1].ino = 4;
		e[1].flags_kind = PC_FLAGS_BSD;
		e[1].flags = 0x20000;
		e[1].owner = "root";
		e[1].group = "wheel";
		e[1].nxattrs = 2;
		e[1].xattrs = x;
		e[1].nacls = 1;
		e[1].acls = a;
		e[1].ext = ext;
		e[1].extlen = sizeof (ext);
		e[0].path = "a-dir";
		e[0].subtree = sub;
		e[2].path = "dir/hard";
		e[2].type = PC_ENTRY_HARDLINK;
		e[2].target = "dir/file";
		m = pc_manifest_new();
		for (i = 0; i < 3; i++)
			if (pc_manifest_add(m, &e[i]) != 0)
				exit(1);
		e[0].path = "dev";
		e[0].type = PC_ENTRY_BLOCKDEV;
		e[0].subtree = NULL;
		e[0].rdev = 0x801;
		if (pc_manifest_add(m, &e[0]) != 0)
			exit(1);
		len = 0;
		if (pc_manifest_encode(m, NULL, &len) != 0 || (b = malloc(len)) == NULL ||
		    pc_manifest_encode(m, b, &len) != 0)
			exit(1);
		put(dir, "records", -1, b, len);
		free(b);
		pc_manifest_free(m);
	}
}

static void
le64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/*
 * Seek index bodies (DESIGN.md) that fuzz_seek_index's limits accept:
 * end marker at 2^20, two chunks with 4-byte tags, one member. Then the
 * same body with chunk 0 at 2^64 - 4, where an "off + 8" bound wraps.
 */
static void
seek_seeds(const char *dir)
{
	uint8_t b[40 + 2 * 20 + 18 + 5];

	memset(b, 0, sizeof (b));
	memcpy(b, "PCSXIDX1", 8);
	b[8] = 4;
	le64(b + 16, 1 << 20);
	le64(b + 24, 2);
	le64(b + 32, 1);
	le64(b + 40, 30);		/* chunk 0: file offset, length, tag */
	le64(b + 48, 600);
	memcpy(b + 56, "\x01\x02\x03\x04", 4);
	le64(b + 60, 500);		/* chunk 1 */
	le64(b + 68, 400);
	memcpy(b + 76, "\x05\x06\x07\x08", 4);
	le64(b + 80, 512);		/* member: offset, length, name */
	le64(b + 88, 300);
	b[96] = 5;
	memcpy(b + 98, "a/b.c", 5);
	put(dir, "two-chunks", -1, b, sizeof (b));
	le64(b + 40, UINT64_MAX - 3);
	put(dir, "chunk-offset-wraps", -1, b, sizeof (b));
}

int
main(int argc, char **argv)
{
	char d[1024];

	if (argc != 2) {
		fprintf(stderr, "usage: fuzz_seeds <dir>\n");
		return (2);
	}
	snprintf(d, sizeof (d), "%s/chunk", argv[1]);
	chunk_seeds(d);
	snprintf(d, sizeof (d), "%s/manifest", argv[1]);
	manifest_seeds(d);
	snprintf(d, sizeof (d), "%s/seek_index", argv[1]);
	seek_seeds(d);
	return (0);
}
