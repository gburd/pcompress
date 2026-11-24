/*
 * Driver for ci/difffuzz.sh: the library's chunk, id, manifest, index and
 * object code behind a small command line, so one source built against
 * two releases can be compared with each other and with the Python model
 * (test/tools/xver_model.py). Public API only, as of 4.4.0.
 *
 *   xver_drv model <spec> <in> <dir>   gear chunks + ids, manifest, index
 *   xver_drv reman <in> <out>          decode a manifest and encode it again
 *   xver_drv seal <in> <objs> <alg> <level> <keyed 0|1>
 *   xver_drv open <objs> <out> <keyed 0|1>
 *
 * Objects are written as a u32 length (little-endian) and the object.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pcompress/pcompress.h>

#define	MAXCH	16384
#define	MAXE	256

static const uint8_t KEY[32] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

static void die(const char *what) __attribute__((noreturn));

static void
die(const char *what)
{
	fprintf(stderr, "xver_drv: %s\n", what);
	exit(2);
}

/* The whole of a file, in malloc()ed memory; *n its size. */
static uint8_t *
slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long sz;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0)
		die(path);
	rewind(f);
	if ((b = malloc((size_t)sz + 1)) == NULL || fread(b, 1, (size_t)sz, f) != (size_t)sz)
		die(path);
	fclose(f);
	*n = (size_t)sz;
	return (b);
}

static void
spit(const char *path, const void *b, size_t n)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL || fwrite(b, 1, n, f) != n || fclose(f) != 0)
		die(path);
}

/* Gear chunk lengths of buf at avg 4096, min 1024, max 16384; count. */
static size_t
chunks(const uint8_t *buf, size_t n, size_t **lens)
{
	pc_chunker_t *c = pc_chunker_new2(PC_CHUNKER_GEAR, 4096, 1024, MAXCH);
	size_t cap = n / 1024 + 2;
	int k, r;

	*lens = malloc(cap * sizeof (**lens));
	if (c == NULL || *lens == NULL || (k = pc_chunker_feed(c, buf, n, *lens, (int)cap)) < 0 ||
	    (r = pc_chunker_finish(c, *lens + k, (int)cap - k)) < 0)
		die("chunker");
	pc_chunker_free(c);
	return ((size_t)(k + r));
}

static char *
unhex(const char *s, size_t *n)
{
	size_t i, l = strlen(s) / 2;
	char *o;

	if (strcmp(s, "-") == 0) {
		*n = 0;
		return (NULL);
	}
	if ((o = calloc(1, l + 1)) == NULL)
		die("malloc");
	for (i = 0; i < l; i++) {
		unsigned v;

		if (sscanf(s + 2 * i, "%2x", &v) != 1)
			die("hex");
		o[i] = (char)v;
	}
	*n = l;
	return (o);
}

/*
 * One spec line (xver_model.py write_spec) into e. The strings it points
 * to are leaked: the driver is a short-lived process.
 */
static void
parse_entry(char *line, pc_entry_t *e)
{
	char *f[13], *s, *save = NULL, *t;
	int i;
	size_t n;
	long long bt;
	unsigned long bn;

	for (i = 0, s = strtok_r(line, " \n", &save); s && i < 13; s = strtok_r(NULL, " \n", &save))
		f[i++] = s;
	if (i != 13)
		die("spec line");
	memset(e, 0, sizeof (*e));
	e->path = unhex(f[0], &n);
	e->type = atoi(f[1]);
	e->mode = (uint32_t)strtoul(f[2], NULL, 10);
	e->uid = (uint32_t)strtoul(f[3], NULL, 10);
	e->gid = (uint32_t)strtoul(f[4], NULL, 10);
	e->mtime = strtoll(f[5], NULL, 10);
	e->mtime_nsec = (uint32_t)strtoul(f[6], NULL, 10);
	e->target = unhex(f[7], &n);
	e->rdev = strtoull(f[8], NULL, 10);
	e->owner = unhex(f[9], &n);
	if (sscanf(f[10], "%lld:%lu", &bt, &bn) == 2) {
		e->have |= PC_HAVE_BTIME;
		e->btime = bt;
		e->btime_nsec = (uint32_t)bn;
	}
	if (strcmp(f[11], "-") != 0) {
		pc_xattr_t *x = calloc(8, sizeof (*x));

		for (t = strtok_r(f[11], ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			char *eq = strchr(t, '=');

			*eq = '\0';
			x[e->nxattrs].name = unhex(t, &n);
			x[e->nxattrs].value = unhex(eq + 1, &x[e->nxattrs].len);
			e->nxattrs++;
		}
		e->xattrs = x;
	}
	if (strcmp(f[12], "-") != 0) {
		uint8_t *ids = malloc(32 * 8);
		uint32_t *lens = malloc(4 * 8);

		for (t = strtok_r(f[12], ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			char *id = unhex(strtok(t, ":"), &n);

			memcpy(ids + 32 * e->nchunks, id, 32);
			lens[e->nchunks] = (uint32_t)strtoul(strtok(NULL, ":"), NULL, 10);
			e->size += lens[e->nchunks++];
		}
		e->ids = ids;
		e->lens = lens;
	}
}

/* chunks.txt (length, id, keyed id per chunk) and index.bin. */
static void
model_chunks(const char *in, const char *dir)
{
	size_t n, nc, i, off = 0, *lens;
	uint8_t *buf = slurp(in, &n), id[32], kid[32], (*ids)[32];
	char path[4096];
	FILE *f;
	pc_index_t *x;
	int j;

	nc = chunks(buf, n, &lens);
	snprintf(path, sizeof (path), "%s/chunks.txt", dir);
	if ((f = fopen(path, "w")) == NULL || (ids = malloc((nc + 1) * 32)) == NULL)
		die(path);
	for (i = 0; i < nc; off += lens[i++]) {
		if (pc_chunk_id(buf + off, lens[i], NULL, 0, id) != 0 ||
		    pc_chunk_id(buf + off, lens[i], KEY, 32, kid) != 0)
			die("chunk id");
		memcpy(ids[i], id, 32);
		fprintf(f, "%zu ", lens[i]);
		for (j = 0; j < 32; j++)
			fprintf(f, "%02x", id[j]);
		fputc(' ', f);
		for (j = 0; j < 32; j++)
			fprintf(f, "%02x", kid[j]);
		fputc('\n', f);
	}
	fclose(f);
	/* Every id, then the first half again: repeats must not be stored. */
	snprintf(path, sizeof (path), "%s/index.bin", dir);
	unlink(path);
	if ((x = pc_index_open(path)) == NULL)
		die("index");
	for (i = 0; i < nc + nc / 2; i++) {
		/* New only the first time: equal chunks have equal ids. */
		int want = pc_index_has(x, ids[i % nc]) == 1 ? 0 : 1;

		if (pc_index_add(x, ids[i % nc]) != want || (i >= nc && want != 0))
			die("index add");
	}
	if (pc_index_close(x) != 0)
		die("index close");
}

static void
model(const char *spec, const char *in, const char *dir)
{
	pc_manifest_t *m = pc_manifest_new();
	char line[1 << 16], path[4096];
	FILE *f = fopen(spec, "r");
	pc_entry_t e;
	size_t len;
	uint8_t *b;

	if (f == NULL || m == NULL)
		die(spec);
	while (fgets(line, sizeof (line), f) != NULL) {
		parse_entry(line, &e);
		if (pc_manifest_add(m, &e) != 0)
			die("manifest add");
	}
	fclose(f);
	if (pc_manifest_encode(m, NULL, &len) != 0 || (b = malloc(len + 1)) == NULL ||
	    pc_manifest_encode(m, b, &len) != 0)
		die("manifest encode");
	snprintf(path, sizeof (path), "%s/manifest.bin", dir);
	spit(path, b, len);
	pc_manifest_free(m);
	model_chunks(in, dir);
}

static void
reman(const char *in, const char *out)
{
	size_t n, len;
	uint8_t *b = slurp(in, &n), *o;
	pc_manifest_t *m = pc_manifest_decode(b, n);

	if (m == NULL || pc_manifest_encode(m, NULL, &len) != 0 || (o = malloc(len + 1)) == NULL ||
	    pc_manifest_encode(m, o, &len) != 0)
		die("manifest decode/encode");
	spit(out, o, len);
}

static pc_ctx_t *
ctx(const char *alg, int level, int keyed)
{
	pc_ctx_t *c = pc_new();

	if (c == NULL || (alg && pc_set_algorithm(c, alg) != 0) ||
	    (alg && pc_set_level(c, level) != 0) || (keyed && pc_set_key(c, KEY, 32) != 0))
		die("context");
	return (c);
}

static void
seal(const char *in, const char *objs, const char *alg, int level, int keyed)
{
	size_t n, nc, i, off = 0, *lens, ol;
	uint8_t *buf = slurp(in, &n), *obj = malloc(pc_chunk_seal_bound(MAXCH)), hdr[4];
	pc_ctx_t *c = ctx(alg, level, keyed);
	FILE *f = fopen(objs, "wb");

	if (f == NULL || obj == NULL)
		die(objs);
	nc = chunks(buf, n, &lens);
	for (i = 0; i < nc; off += lens[i++]) {
		ol = pc_chunk_seal_bound(MAXCH);
		if (pc_chunk_seal(c, buf + off, lens[i], obj, &ol) != 0)
			die(pc_last_error(c));
		hdr[0] = (uint8_t)ol; hdr[1] = (uint8_t)(ol >> 8);
		hdr[2] = (uint8_t)(ol >> 16); hdr[3] = (uint8_t)(ol >> 24);
		if (fwrite(hdr, 1, 4, f) != 4 || fwrite(obj, 1, ol, f) != ol)
			die(objs);
	}
	if (fclose(f) != 0)
		die(objs);
	pc_free(c);
}

static void
open_objs(const char *objs, const char *out, int keyed)
{
	size_t n, pos = 0, ol;
	uint8_t *b = slurp(objs, &n), *pt = malloc(MAXCH);
	pc_ctx_t *c = ctx(NULL, 0, keyed);
	FILE *f = fopen(out, "wb");

	if (f == NULL || pt == NULL)
		die(out);
	while (pos + 4 <= n) {
		size_t l = b[pos] | (size_t)b[pos + 1] << 8 | (size_t)b[pos + 2] << 16 |
		    (size_t)b[pos + 3] << 24;

		pos += 4;
		ol = MAXCH;
		if (l > n - pos || pc_chunk_open(c, b + pos, l, pt, &ol) != 0)
			die(pc_last_error(c));
		if (fwrite(pt, 1, ol, f) != ol)
			die(out);
		pos += l;
	}
	if (pos != n || fclose(f) != 0)
		die("trailing bytes");
	pc_free(c);
}

int
main(int argc, char **argv)
{
	if (argc == 5 && strcmp(argv[1], "model") == 0)
		model(argv[2], argv[3], argv[4]);
	else if (argc == 4 && strcmp(argv[1], "reman") == 0)
		reman(argv[2], argv[3]);
	else if (argc == 7 && strcmp(argv[1], "seal") == 0)
		seal(argv[2], argv[3], argv[4], atoi(argv[5]), atoi(argv[6]));
	else if (argc == 5 && strcmp(argv[1], "open") == 0)
		open_objs(argv[2], argv[3], atoi(argv[4]));
	else
		die("usage: see the top of test/tools/xver_drv.c");
	return (0);
}
