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
 * bytes first. Run by "make fuzz-build".
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
 * (algorithm id, level, 20-bit length, payload), 8 the same as a
 * dictionary object (zstd with fuzz_chunk's dictionary); 2 opens the rest as is,
 * with a key when bit 2 is set (6); 3 opens a key file, 19 sets the rest
 * as a dictionary.
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
	/* Forged dictionary mode (8): fuzz_chunk's dictionary, as raw content. */
	{
		static uint8_t dict[4096];
		pc_ctx_t *c = pc_new();

		for (i = 0; i < sizeof (dict); i++)
			dict[i] = (uint8_t)("the quick brown fox jumps "[i % 26] + (i % 97 == 0));
		pc_set_log(c, quiet, NULL, PC_LOG_NONE);
		ol = sizeof (obj);
		if (pc_set_dict(c, dict, sizeof (dict)) == 0 &&
		    pc_chunk_seal(c, in, sizeof (in), obj, &ol) == 0 && (obj[5] & 0x04)) {
			uint8_t f[4096];

			f[0] = 1;
			f[1] = obj[7];
			f[2] = obj[8];
			f[3] = obj[9];
			f[4] = obj[10];
			memcpy(f + 5, obj + 72, ol - 104);
			put(dir, "forged-zstd-dict", 8, f, 5 + ol - 104);
		}
		pc_free(c);
	}
	/* Dictionary mode (19): a trained zstd dictionary, entropy tables and all. */
	{
		static char samples[400 * 64];
		static uint8_t dict[2048];
		size_t sizes[400], dl = sizeof (dict), sn = 0;

		for (i = 0; i < 400; i++) {
			sizes[i] = (size_t)snprintf(samples + sn, 64, "{\"id\": %u, \"name\": \"user%u\"}\n",
			    (unsigned)i, (unsigned)(i * 7919 % 10007));
			sn += sizes[i];
		}
		if (pc_dict_train(samples, sizes, 400, dict, &dl) == 0)
			put(dir, "dict-trained", 19, dict, dl);
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
	return (0);
}
