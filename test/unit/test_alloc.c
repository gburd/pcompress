/*
 * pc_set_allocator(): every allocation the library makes goes through the
 * host's functions, and comes back to them. Includes only the public
 * header, as a program would.
 *
 * The allocator installed here puts a 16-byte header (a magic number and
 * the size) in front of each block and hands out the address past it. A
 * block of ours given to the C library's free(), or a C library block
 * given to our free(), is then a crash (and an ASan report): the magic is
 * checked on every free and realloc. It also counts live blocks, which
 * must be back to zero once everything is freed.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pcompress/pcompress.h>
#include "minunit.h"

#define	HDR		16
#define	MAGIC_LIVE	0x70634d454d4c4956ULL	/* "pcMEMLIV" */
#define	MAGIC_DEAD	0x70634d454d444541ULL

static long live, calls;

/*
 * pc_set_thread_hooks() is installed too: on a library thread every call
 * into the allocator must come after its start hook and before its stop
 * hook. thr_state is 0 on a thread the hooks never saw (main, OpenMP),
 * 1 between start and stop, 2 after stop; thr_calls counts the thread's
 * allocator calls.
 */
static __thread int thr_state;
static __thread long thr_calls;
static long thr_starts, thr_stops, thr_outside;

static void
thr_note_call(void)
{
	thr_calls++;
	if (thr_state == 2)
		__atomic_add_fetch(&thr_outside, 1, __ATOMIC_RELAXED);
}

static void
t_thr_start(void *ud, const char *name)
{
	(void) ud;
	(void) name;
	if (thr_calls != 0)
		__atomic_add_fetch(&thr_outside, 1, __ATOMIC_RELAXED);
	thr_state = 1;
	__atomic_add_fetch(&thr_starts, 1, __ATOMIC_RELAXED);
}

static void
t_thr_stop(void *ud)
{
	(void) ud;
	thr_state = 2;
	__atomic_add_fetch(&thr_stops, 1, __ATOMIC_RELAXED);
}
static unsigned long long bytes;
static char dir[200], src[300], cmp_[300], out[300];
static unsigned char *data;
static size_t data_len;

/* Header of a block we handed out; aborts if p is not one of ours. */
static unsigned long long *
hdr_of(void *p)
{
	unsigned long long *h = (unsigned long long *)((char *)p - HDR);

	if (h[0] != MAGIC_LIVE) {
		fprintf(stderr, "test_alloc: %p was not allocated by the hook (%s)\n", p,
		    h[0] == MAGIC_DEAD ? "freed twice" : "foreign pointer");
		abort();
	}
	return (h);
}

static void *
t_malloc(void *ud, size_t n)
{
	unsigned long long *h;

	(void) ud;
	thr_note_call();
	if ((h = malloc(HDR + n)) == NULL)
		return (NULL);
	h[0] = MAGIC_LIVE;
	h[1] = n;
	__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&bytes, n, __ATOMIC_RELAXED);
	return ((char *)h + HDR);
}

static void *
t_realloc(void *ud, void *p, size_t n)
{
	unsigned long long *h = hdr_of(p), *nh;

	(void) ud;
	thr_note_call();
	if ((nh = realloc(h, HDR + n)) == NULL)
		return (NULL);
	nh[1] = n;
	__atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&bytes, n, __ATOMIC_RELAXED);
	return ((char *)nh + HDR);
}

static void
t_free(void *ud, void *p)
{
	unsigned long long *h = hdr_of(p);

	(void) ud;
	thr_note_call();
	h[0] = MAGIC_DEAD;
	__atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
	free(h);
}

static long
nlive(void)
{
	return (__atomic_load_n(&live, __ATOMIC_RELAXED));
}

static int
write_file(const char *path, const void *p, size_t n)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL || fwrite(p, 1, n, f) != n)
		return (-1);
	return (fclose(f));
}

static int
same(const char *path, const void *p, size_t n)
{
	unsigned char *b = malloc(n + 1);
	FILE *f = fopen(path, "rb");
	int eq = (b != NULL && f != NULL && fread(b, 1, n + 1, f) == n &&
	    memcmp(b, p, n) == 0);

	if (f != NULL)
		fclose(f);
	free(b);
	return (eq);
}

/*
 * Compress src to cmp_ and back to out through descriptors, with 4 threads.
 * 0, a negative step number, or 1 if this build lacks the algorithm.
 */
static int
roundtrip(const char *algo, int dedupe, const char *enc)
{
	const char *pw = "allocator test password";
	pc_ctx_t *c = pc_new();
	int in, o, rv;

	if (c == NULL)
		return (-1);
	if (pc_set_algorithm(c, algo) != 0) {
		pc_free(c);
		return (1);
	}
	pc_set_log(c, NULL, NULL, PC_LOG_ERR);
	pc_set_level(c, 6);
	pc_set_threads(c, 4);
	pc_set_dedupe(c, dedupe);
	pc_set_chunk_size(c, dedupe ? 1024 * 1024 : 256 * 1024);
	if (enc != NULL && (pc_set_encryption(c, enc, 0) != 0 ||
	    pc_set_password(c, pw, strlen(pw)) != 0))
		return (-2);
	in = open(src, O_RDONLY);
	o = open(cmp_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_compress_fd(c, in, o);
	close(in);
	close(o);
	if (rv != 0)
		fprintf(stderr, "compress %s: %s\n", algo, pc_last_error(c));
	pc_free(c);
	if (rv != 0)
		return (-3);

	c = pc_new();
	if (enc != NULL && pc_set_password(c, pw, strlen(pw)) != 0)
		return (-4);
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_decompress_fd(c, in, o);
	close(in);
	close(o);
	if (rv != 0)
		fprintf(stderr, "decompress %s: %s\n", algo, pc_last_error(c));
	pc_free(c);
	if (rv != 0)
		return (-5);
	return (same(out, data, data_len) ? 0 : -6);
}

/* Refused before any use: bad structs. Then installed. */
MU_TEST(test_alloc_install)
{
	pc_allocator_t a = { sizeof (a), t_malloc, t_realloc, t_free, NULL };
	pc_allocator_t bad = a;
	pc_thread_hooks_t h = { sizeof (h), t_thr_start, t_thr_stop, NULL };

	bad.size = sizeof (a) - 1;
	mu_assert_int_eq(-1, pc_set_allocator(&bad));
	bad = a;
	bad.free = NULL;
	mu_assert_int_eq(-1, pc_set_allocator(&bad));
	bad = a;
	bad.realloc = NULL;
	mu_assert_int_eq(-1, pc_set_allocator(&bad));
	mu_assert_int_eq(0, pc_set_allocator(NULL));
	mu_assert_int_eq(0, pc_set_allocator(&a));
	mu_assert_int_eq(0, pc_set_thread_hooks(&h));
	mu_assert_int_eq(0, (int)calls);
}

MU_TEST(test_alloc_context)
{
	pc_ctx_t *c = pc_new();

	mu_assert(c != NULL, "pc_new");
	mu_assert(nlive() > 0, "pc_new did not allocate through the hook");
	pc_free(c);
	mu_assert_int_eq(0, (int)nlive());
	/* Something was allocated: no more changes. */
	mu_assert_int_eq(-1, pc_set_allocator(NULL));
}

MU_TEST(test_alloc_roundtrips)
{
	static const char *algos[] = { "zstd", "lz4", "lzma", "lzmaMt", "ppmd",
	    "bzip2", "zlib", "libbsc", "adapt", "adapt2", "lzfx", "none", NULL };
	int i, d, rv;
	char msg[64];

	for (i = 0; algos[i] != NULL; i++) {
		for (d = 0; d < 2; d++) {
			rv = roundtrip(algos[i], d ? PC_DEDUPE_RABIN : PC_DEDUPE_NONE, NULL);
			snprintf(msg, sizeof (msg), "%s dedupe %d: %d", algos[i], d, rv);
			mu_assert(rv == 0 || rv == 1, msg);
			mu_assert(nlive() == 0, msg);
		}
	}
	mu_assert_int_eq(0, roundtrip("zstd", PC_DEDUPE_NONE, "AES"));
	mu_assert_int_eq(0, roundtrip("lz4", PC_DEDUPE_RABIN, "SALSA20"));
	mu_assert_int_eq(0, (int)nlive());
}

/*
 * Seal and open one chunk per algorithm, keyed and not. For zstd and
 * libbsc the codecs' own buffers (several times the chunk) must come
 * through the hook: their allocators are routed to it.
 */
MU_TEST(test_alloc_chunks)
{
	static const char *algos[] = { "zstd", "lz4", "lzma", "zlib", "bzip2",
	    "ppmd", "lzfx", "libbsc", "adapt", "adapt2", "none", NULL };
	size_t n = 1024 * 1024, cap = pc_chunk_seal_bound(n), ol, pl;
	unsigned char key[32], *obj = malloc(cap), *back = malloc(n);
	unsigned long long before;
	int i, k;

	memset(key, 7, sizeof (key));
	mu_assert(obj != NULL && back != NULL, "setup");
	for (i = 0; algos[i] != NULL; i++) {
		for (k = 0; k < 2; k++) {
			pc_ctx_t *c = pc_new();

			if (pc_set_algorithm(c, algos[i]) != 0) {
				pc_free(c);
				continue;
			}
			pc_set_level(c, 6);
			if (k)
				mu_assert_int_eq(0, pc_set_key(c, key, sizeof (key)));
			before = bytes;
			ol = cap;
			mu_assert(pc_chunk_seal(c, data, n, obj, &ol) == 0, algos[i]);
			if (strcmp(algos[i], "zstd") == 0 || strcmp(algos[i], "libbsc") == 0)
				mu_assert(bytes - before > n, algos[i]);
			pl = n;
			mu_assert(pc_chunk_open(c, obj, ol, back, &pl) == 0 && pl == n &&
			    memcmp(back, data, n) == 0, algos[i]);
			pc_free(c);
			mu_assert(nlive() == 0, algos[i]);
		}
	}
	free(obj);
	free(back);
}

MU_TEST(test_alloc_chunker_manifest_index_keyfile)
{
	static const int algs[] = { PC_CHUNKER_RABIN, PC_CHUNKER_GEAR };
	pc_xattr_t xa = { "user.test", "value", 5 };
	unsigned char id[32], key[32], back[32], *kf = malloc(pc_keyfile_size()), *buf;
	uint32_t lens[1] = { 1000 };
	size_t lv[512], len;
	pc_manifest_t *m, *d;
	pc_index_t *x;
	pc_entry_t e;
	char ipath[320];
	int i;

	for (i = 0; i < 2; i++) {
		pc_chunker_t *ch = pc_chunker_new2(algs[i], 8192, 2048, 65536);

		mu_assert(ch != NULL, "chunker");
		mu_assert(pc_chunker_feed(ch, data, data_len, lv, 512) > 0, "feed");
		mu_assert(pc_chunker_finish(ch, lv, 512) >= 0, "finish");
		pc_chunker_free(ch);
	}
	mu_assert_int_eq(0, (int)nlive());

	pc_chunk_id(data, 1000, NULL, 0, id);
	m = pc_manifest_new();
	memset(&e, 0, sizeof (e));
	e.path = "a/file";
	e.type = PC_ENTRY_FILE;
	e.mode = 0100644;
	e.size = 1000;
	e.nchunks = 1;
	e.ids = id;
	e.lens = lens;
	e.owner = "someone";
	e.nxattrs = 1;
	e.xattrs = &xa;
	mu_assert_int_eq(0, pc_manifest_add_sz(m, &e, sizeof (e)));
	mu_assert_int_eq(0, pc_manifest_encode(m, NULL, &len));
	buf = malloc(len);
	mu_assert_int_eq(0, pc_manifest_encode(m, buf, &len));
	d = pc_manifest_decode(buf, len);
	mu_assert(d != NULL && pc_manifest_count(d) == 1, "decode");
	pc_manifest_free(d);
	pc_manifest_free(m);
	free(buf);
	mu_assert_int_eq(0, (int)nlive());

	snprintf(ipath, sizeof (ipath), "%s/idx", dir);
	x = pc_index_open(ipath);
	mu_assert(x != NULL, "index open");
	mu_assert_int_eq(1, pc_index_add(x, id));	/* 1: new */
	mu_assert_int_eq(1, pc_index_has(x, id));
	mu_assert_int_eq(0, pc_index_close(x));
	x = pc_index_open(ipath);
	mu_assert(x != NULL && pc_index_has(x, id) == 1, "index reopen");
	mu_assert_int_eq(0, pc_index_close(x));
	unlink(ipath);
	mu_assert_int_eq(0, (int)nlive());

	mu_assert(kf != NULL, "setup");
	mu_assert_int_eq(0, pc_keyfile_create("pw", 2, key, kf));
	mu_assert_int_eq(0, pc_keyfile_open(kf, pc_keyfile_size(), "pw", 2, back));
	mu_assert(memcmp(key, back, 32) == 0, "keyfile key");
	free(kf);
	mu_assert_int_eq(0, (int)nlive());
}

/* Run one pcompress command line through the command-line style API. */
static int
run(const char *fmt, const char *a, const char *b)
{
	char args[1024];
	pc_ctx_t *p = create_pc_context();
	int r;

	if (p == NULL)
		return (-1);
	snprintf(args, sizeof (args), fmt, a, b);
	pc_set_log(p, NULL, NULL, PC_LOG_ERR);
	r = init_pc_context_argstr(p, args);
	if (r == 0)
		r = start_pcompress(p);
	destroy_pc_context(p);
	return (r);
}

/*
 * File names: the library resolves them with realpath() and keeps copies
 * (and, archiving, a list of the paths), which it frees with the hook.
 */
MU_TEST(test_alloc_cli)
{
	char base[320], pz[330], x[320], f[640 + PATH_MAX], jpg[PATH_MAX];

	snprintf(base, sizeof (base), "%s/cli", dir);
	snprintf(pz, sizeof (pz), "%s.pz", base);
	unlink(pz);
	mu_assert_int_eq(0, run("pcompress -c zstd -l3 -s 1m %s %s", src, base));
	unlink(out);
	mu_assert_int_eq(0, run("pcompress -d %s %s", pz, out));
	mu_assert(same(out, data, data_len), "file round trip");
	unlink(pz);
	mu_assert_int_eq(0, (int)nlive());

#ifndef _WIN32
	/* Members keep their full path below x: remove it bottom up. */
	snprintf(x, sizeof (x), "%s/x", dir);
	mkdir(x, 0700);
	mu_assert_int_eq(0, run("pcompress -a -c lz4 -l1 -s 1m %s %s", src, base));
	mu_assert_int_eq(0, run("pcompress -d %s %s", pz, x));
	snprintf(f, sizeof (f), "%s%s", x, src);
	mu_assert(same(f, data, data_len), "archive round trip");
	unlink(f);
	unlink(pz);
	while (strlen(f) > strlen(x) && strrchr(f, '/') != NULL) {
		*strrchr(f, '/') = '\0';
		rmdir(f);
	}
	mu_assert_int_eq(0, (int)nlive());

	/*
	 * A JPEG with packJPG (-j): the extractor thread decodes it in its
	 * filter scratch buffer, which must go back before its stop hook.
	 * The test runs from the top of the source tree.
	 */
	if (realpath("test/res/jpg/screen.jpg", jpg) != NULL) {
		mu_assert_int_eq(0, run("pcompress -a -j -c lz4 -l1 %s %s", jpg, base));
		mu_assert_int_eq(0, run("pcompress -d %s %s", pz, x));
		snprintf(f, sizeof (f), "%s%s", x, jpg);
		mu_assert(access(f, F_OK) == 0, "jpeg archive round trip");
		unlink(f);
		unlink(pz);
		while (strlen(f) > strlen(x) && strrchr(f, '/') != NULL) {
			*strrchr(f, '/') = '\0';
			rmdir(f);
		}
		mu_assert_int_eq(0, (int)nlive());
	}
#endif
}


MU_TEST(test_alloc_done)
{
	pc_allocator_t a = { sizeof (a), t_malloc, t_realloc, t_free, NULL };

	mu_assert_int_eq(-1, pc_set_allocator(&a));
	mu_assert_int_eq(-1, pc_set_allocator(NULL));
	/* Nothing the library allocated outlives the objects that hold it. */
	mu_assert_int_eq(0, (int)nlive());
	/* Library threads ran, and touched the allocator only between their hooks. */
	mu_assert(thr_starts > 0, "no library thread started");
	mu_assert_int_eq((int)thr_starts, (int)thr_stops);
	mu_assert_int_eq(0, (int)thr_outside);
	printf("test_alloc: %ld calls through the hook, %ld live at the end\n",
	    calls, nlive());
}

MU_TEST_SUITE(suite_alloc)
{
	MU_RUN_TEST(test_alloc_install);
	MU_RUN_TEST(test_alloc_context);
	MU_RUN_TEST(test_alloc_roundtrips);
	MU_RUN_TEST(test_alloc_chunks);
	MU_RUN_TEST(test_alloc_chunker_manifest_index_keyfile);
	MU_RUN_TEST(test_alloc_cli);
	MU_RUN_TEST(test_alloc_done);
}

int
main(void)
{
	const char *t = getenv("TMPDIR");
	unsigned int s = 12345;
	size_t i;

	snprintf(dir, sizeof (dir), "%s/pc_alloc_XXXXXX", t ? t : "/tmp");
	if (mkdtemp(dir) == NULL)
		return (1);
	snprintf(src, sizeof (src), "%s/in", dir);
	snprintf(cmp_, sizeof (cmp_), "%s/in.pz", dir);
	snprintf(out, sizeof (out), "%s/out", dir);
	/* Text with repeats (dedupe finds some) and stretches of noise. */
	data_len = 3 * 1024 * 1024 + 777;
	if ((data = malloc(data_len)) == NULL)
		return (1);
	for (i = 0; i < data_len; i++) {
		s = s * 1103515245u + 12345u;
		data[i] = (i / 8192) % 3 ? (unsigned char)"pcompress allocator "[(i / 3) % 20] :
		    (unsigned char)(s >> 16);
	}
	if (write_file(src, data, data_len) != 0)
		return (1);
	MU_RUN_SUITE(suite_alloc);
	unlink(src);
	unlink(cmp_);
	unlink(out);
	rmdir(dir);
	free(data);
	MU_REPORT();
	return MU_EXIT_CODE;
}
