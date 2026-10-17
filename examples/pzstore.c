/*
 * pzstore: a minimal content-addressed, deduplicating, encrypted store on
 * the public libpcompress chunk API. Shows the pieces a backup tool needs;
 * not a backup tool.
 *
 *   pzstore put <store> <keyfile> <file>   > manifest   (one id per line)
 *   pzstore get <store> <keyfile> < manifest > file
 *
 * <keyfile> holds 32 raw key bytes. Each chunk is stored once, as
 * <store>/<keyed id>; the id is keyed so object names do not reveal content
 * hashes. <store>/index (pc_index_*) remembers what is stored, so a second
 * "put" of the same file stores nothing and asks the store nothing.
 *
 *   cc $(pkg-config --cflags libpcompress) pzstore.c -o pzstore $(pkg-config --libs libpcompress)
 */
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcompress/pcompress.h>

#define	MAXCHUNK	(256 * 1024)

static int
load_key(const char *path, unsigned char key[32])
{
	FILE *f = fopen(path, "rb");
	int ok = f != NULL && fread(key, 1, 32, f) == 32;

	if (f != NULL)
		fclose(f);
	return (ok ? 0 : -1);
}

static void
hex(const unsigned char id[32], char out[65])
{
	int i;

	for (i = 0; i < 32; i++)
		sprintf(out + 2 * i, "%02x", id[i]);
}

/* Store one chunk unless the index has it. 1 stored, 0 dedup, -1 error. */
static int
put_chunk(pc_ctx_t *c, pc_index_t *idx, const char *store, const unsigned char *key,
    const unsigned char *buf, size_t len, unsigned char *obj)
{
	unsigned char id[32];
	char name[65], path[4096], tmp[4200];
	size_t ol = pc_chunk_seal_bound(len);
	FILE *f;

	if (pc_chunk_id(buf, len, key, 32, id) != 0)
		return (-1);
	hex(id, name);
	printf("%s %zu\n", name, len);
	if (pc_index_has(idx, id) == 1)
		return (0);
	snprintf(path, sizeof (path), "%s/%s", store, name);
	if (pc_chunk_seal(c, buf, len, obj, &ol) != 0) {
		fprintf(stderr, "pzstore: seal: %s\n", pc_last_error(c));
		return (-1);
	}
	snprintf(tmp, sizeof (tmp), "%s.tmp", path);
	if ((f = fopen(tmp, "wb")) == NULL || fwrite(obj, 1, ol, f) != ol || fclose(f) != 0 ||
	    rename(tmp, path) != 0) {
		fprintf(stderr, "pzstore: %s: %s\n", path, strerror(errno));
		return (-1);
	}
	/* Only now: the index must never claim an object that is not there. */
	return (pc_index_add(idx, id) < 0 ? -1 : 1);
}

static int
put(pc_ctx_t *c, const char *store, const unsigned char *key, const char *file)
{
	pc_chunker_t *ch = pc_chunker_new(64 * 1024, 16 * 1024, MAXCHUNK);
	unsigned char *buf = malloc(4 * MAXCHUNK), *obj = malloc(pc_chunk_seal_bound(MAXCHUNK));
	size_t have = 0, lens[64], off, r;
	FILE *in = fopen(file, "rb");
	long stored = 0, dedup = 0;
	int n, k, rv = 0, eof = 0;
	char ipath[4096];
	pc_index_t *idx;

	snprintf(ipath, sizeof (ipath), "%s/index", store);
	idx = pc_index_open(ipath);
	if (ch == NULL || buf == NULL || obj == NULL || in == NULL || idx == NULL)
		return (-1);
	while (rv == 0 && !eof) {
		r = fread(buf + have, 1, 4 * MAXCHUNK - have, in);
		eof = r == 0;
		n = eof ? pc_chunker_finish(ch, lens, 64) : pc_chunker_feed(ch, buf + have, r, lens, 64);
		have += r;
		if (n < 0)
			rv = -1;
		for (off = 0, k = 0; rv == 0 && k < n; off += lens[k++]) {
			int s = put_chunk(c, idx, store, key, buf + off, lens[k], obj);

			if (s < 0)
				rv = -1;
			else
				s ? stored++ : dedup++;
		}
		/* Keep the unfinished chunk's bytes at the front. */
		memmove(buf, buf + off, have - off);
		have -= off;
	}
	fprintf(stderr, "pzstore: %ld chunks stored, %ld already present\n", stored, dedup);
	if (pc_index_close(idx) != 0)
		rv = -1;
	fclose(in);
	pc_chunker_free(ch);
	free(buf);
	free(obj);
	return (rv);
}

static int
get(pc_ctx_t *c, const char *store, const unsigned char *key)
{
	unsigned char *obj = malloc(pc_chunk_seal_bound(MAXCHUNK)), *buf = malloc(MAXCHUNK), id[32];
	char name[80], path[4096], want[65];
	size_t len, ol, pl;
	FILE *f;

	while (scanf("%64s %zu", name, &len) == 2) {
		snprintf(path, sizeof (path), "%s/%s", store, name);
		if ((f = fopen(path, "rb")) == NULL ||
		    (ol = fread(obj, 1, pc_chunk_seal_bound(MAXCHUNK), f)) == 0) {
			fprintf(stderr, "pzstore: missing object %s\n", name);
			return (-1);
		}
		fclose(f);
		pl = MAXCHUNK;
		if (pc_chunk_open(c, obj, ol, buf, &pl) != 0 || pl != len) {
			fprintf(stderr, "pzstore: %s: %s\n", name, pc_last_error(c));
			return (-1);
		}
		/* The object must be the chunk its name says. */
		pc_chunk_id(buf, pl, key, 32, id);
		hex(id, want);
		if (strcmp(want, name) != 0) {
			fprintf(stderr, "pzstore: %s: content does not match its id\n", name);
			return (-1);
		}
		fwrite(buf, 1, pl, stdout);
	}
	free(obj);
	free(buf);
	return (0);
}

int
main(int argc, char **argv)
{
	unsigned char key[32];
	pc_ctx_t *c;
	int rv;

	if (argc < 4 || (strcmp(argv[1], "put") == 0 && argc != 5) ||
	    load_key(argv[3], key) != 0) {
		fprintf(stderr, "usage: pzstore put|get <store> <keyfile> [file]\n");
		return (2);
	}
	mkdir(argv[2], 0700);
	c = pc_new();
	pc_set_algorithm(c, "zstd");
	pc_set_level(c, 6);
	pc_set_key(c, key, 32);
	pc_set_log(c, NULL, NULL, PC_LOG_NONE);
	rv = strcmp(argv[1], "put") == 0 ? put(c, argv[2], key, argv[4]) : get(c, argv[2], key);
	pc_free(c);
	return (rv != 0);
}
