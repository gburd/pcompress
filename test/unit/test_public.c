/*
 * Public API (include/pcompress/pcompress.h): typed settings and
 * descriptor I/O. Includes only the public header, as a program would.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#ifndef _WIN32
#include <pwd.h>
#include <grp.h>
#endif
#include <pcompress/pcompress.h>
#include "minunit.h"

static char dir[200], src[300], cmp_[300], out[300];
static size_t src_len;

static int
write_src(size_t n)
{
	FILE *f = fopen(src, "wb");
	size_t i;

	if (f == NULL)
		return (-1);
	for (i = 0; i < n; i++)
		fputc((i / 777) % 3 ? (int)("pcompress "[i % 10]) : (int)(i * 131 >> 3), f);
	src_len = n;
	return (fclose(f));
}

static int
same(const char *a, const char *b)
{
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	int ca, cb, eq = (fa && fb);

	while (eq) {
		ca = fgetc(fa);
		cb = fgetc(fb);
		if (ca != cb)
			eq = 0;
		if (ca == EOF)
			break;
	}
	if (fa) fclose(fa);
	if (fb) fclose(fb);
	return (eq);
}

/* Compress src -> cmp_ and decompress cmp_ -> out with a fresh context each. */
static int
roundtrip(const char *algo, int level, int dedupe, const char *enc, const char *pw)
{
	pc_ctx_t *c;
	int in, o, rv;

	c = pc_new();
	if (algo && pc_set_algorithm(c, algo) != 0) return (-10);
	if (level && pc_set_level(c, level) != 0) return (-11);
	if (pc_set_dedupe(c, dedupe) != 0) return (-12);
	if (enc && pc_set_encryption(c, enc, 0) != 0) return (-13);
	if (pw && pc_set_password(c, pw, strlen(pw)) != 0) return (-14);
	pc_set_chunk_size(c, dedupe ? 1024 * 1024 : 256 * 1024);	/* dedupe needs >= 1 MB */
	in = open(src, O_RDONLY);
	o = open(cmp_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_compress_fd(c, in, o);
	close(in);
	close(o);
	if (rv != 0)
		fprintf(stderr, "compress %s: %s\n", algo ? algo : "(default)", pc_last_error(c));
	pc_free(c);
	if (rv != 0)
		return (-1);

	c = pc_new();
	if (pw && pc_set_password(c, pw, strlen(pw)) != 0) return (-15);
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_decompress_fd(c, in, o);
	close(in);
	close(o);
	if (rv != 0)
		fprintf(stderr, "decompress %s: %s\n", algo ? algo : "(default)", pc_last_error(c));
	pc_free(c);
	if (rv != 0)
		return (-2);
	return (same(src, out) ? 0 : -3);
}

/*
 * pc_set_algorithm() must copy the name: a caller that reads it from a
 * config file frees its buffer. Here the buffer is scribbled over and
 * freed before the job runs; it still has to compress with lz4 (and under
 * ASan the old by-reference code is a use-after-free).
 */
MU_TEST(test_pub_algorithm_name_copied)
{
	pc_ctx_t *c = pc_new();
	char *name = malloc(16);
	unsigned char hdr[8];
	int in, o, rv;
	FILE *f;

	mu_assert(c != NULL && name != NULL, "setup");
	strcpy(name, "lz4");
	mu_assert_int_eq(0, pc_set_algorithm(c, name));
	memset(name, 'z', 15);
	name[15] = '\0';
	free(name);
	in = open(src, O_RDONLY);
	o = open(cmp_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_compress_fd(c, in, o);
	close(in);
	close(o);
	pc_free(c);
	mu_assert_int_eq(0, rv);
	/* The stream header starts with the algorithm name. */
	f = fopen(cmp_, "rb");
	mu_assert(f != NULL && fread(hdr, 1, sizeof (hdr), f) == sizeof (hdr), "read header");
	fclose(f);
	mu_assert(memcmp(hdr, "lz4", 4) == 0, "stream was not written with lz4");
}

MU_TEST(test_pub_version)
{
	mu_assert_int_eq(PC_API_VERSION, pc_api_version());
	mu_assert(pc_version() != NULL && pc_version()[0] >= '4', pc_version());
}

MU_TEST(test_pub_roundtrips)
{
	static const char *algos[] = { "zstd", "lz4", "lzma", "zlib", "bzip2",
	    "ppmd", "lzfx", "adapt2", "none", NULL };
	int i;

	for (i = 0; algos[i] != NULL; i++)
		mu_assert_int_eq(0, roundtrip(algos[i], 3, PC_DEDUPE_NONE, NULL, NULL));
	mu_assert_int_eq(0, roundtrip(NULL, 0, PC_DEDUPE_NONE, NULL, NULL));	/* defaults */
	mu_assert_int_eq(0, roundtrip("zstd", 9, PC_DEDUPE_RABIN, NULL, NULL));
	mu_assert_int_eq(0, roundtrip("zstd", 6, PC_DEDUPE_FIXED, NULL, NULL));
	mu_assert_int_eq(0, roundtrip("lz4", 1, PC_DEDUPE_NONE, "AES", "a longer passphrase"));
	mu_assert_int_eq(0, roundtrip("zstd", 3, PC_DEDUPE_NONE, "SALSA20", "pw"));
}

/* The whole password counts: same first 16 bytes, different tail fails. */
MU_TEST(test_pub_long_password)
{
	const char *a = "0123456789abcdef-tail-one", *b = "0123456789abcdef-tail-two";
	pc_ctx_t *c;
	int in, o, rv;

	mu_assert_int_eq(0, roundtrip("lz4", 1, PC_DEDUPE_NONE, "AES", a));
	c = pc_new();
	pc_set_log(c, NULL, NULL, PC_LOG_NONE);
	pc_set_password(c, b, strlen(b));
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_decompress_fd(c, in, o);
	close(in);
	close(o);
	mu_assert(rv != 0, "password tail ignored");
	pc_free(c);
}

MU_TEST(test_pub_password_not_wiped)
{
	char pw[] = "caller's buffer";
	pc_ctx_t *c = pc_new();

	mu_assert_int_eq(0, pc_set_password(c, pw, strlen(pw)));
	mu_assert(strcmp(pw, "caller's buffer") == 0, "caller buffer changed");
	pc_free(c);
}

MU_TEST(test_pub_wrong_password)
{
	pc_ctx_t *c;
	int in, o, rv;

	mu_assert_int_eq(0, roundtrip("zstd", 3, PC_DEDUPE_NONE, "AES", "right"));
	c = pc_new();
	pc_set_log(c, NULL, NULL, PC_LOG_ERR);
	pc_set_password(c, "wrong", 5);
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	rv = pc_decompress_fd(c, in, o);
	close(in);
	close(o);
	mu_assert(rv != 0, "wrong password accepted");
	mu_assert_int_eq(PC_ERR_VERIFY, pc_last_error_code(c));
	pc_free(c);
}

static void
quiet(void *a, int l, const char *m)
{
	(void) a; (void) l; (void) m;
}

MU_TEST(test_pub_bad_settings)
{
	pc_ctx_t *c = pc_new();

	pc_set_log(c, quiet, NULL, PC_LOG_ERR);
	mu_assert(pc_set_algorithm(c, "nope") != 0, "bad algorithm accepted");
	mu_assert_int_eq(PC_ERR_ARGS, pc_last_error_code(c));
	mu_assert(pc_set_level(c, 0) != 0 && pc_set_level(c, 15) != 0, "bad level");
	mu_assert(pc_set_threads(c, -1) != 0, "bad threads");
	mu_assert(pc_set_checksum(c, "MD5") != 0, "bad checksum");
	mu_assert(pc_set_dedupe(c, 9) != 0, "bad dedupe");
	mu_assert(pc_set_encryption(c, "DES", 0) != 0, "bad cipher");
	mu_assert(pc_set_encryption(c, "AES", 24) != 0, "bad keylen");
	mu_assert(pc_set_chunk_size(c, 10) != 0, "tiny chunk");
	mu_assert(pc_compress_fd(c, -1, 1) != 0, "bad fd");
	/* Global dedupe needs a file: refused for a stream. */
	mu_assert_int_eq(0, pc_set_dedupe(c, PC_DEDUPE_GLOBAL));
	mu_assert(pc_compress_fd(c, 0, 1) != 0, "global dedupe on a stream");
	mu_assert_int_eq(PC_ERR_ARGS, pc_last_error_code(c));
	pc_free(c);
	pc_free(NULL);
}

MU_TEST(test_pub_fds_left_open)
{
	pc_ctx_t *c = pc_new();
	int in = open(src, O_RDONLY), o = open(cmp_, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	mu_assert_int_eq(0, pc_compress_fd(c, in, o));
	/* Both still open and usable. */
	mu_assert(lseek(in, 0, SEEK_SET) == 0, "in_fd closed");
	mu_assert(write(o, "", 0) == 0, "out_fd closed");
	close(in);
	close(o);
	/* A context can run another job. */
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_int_eq(0, pc_decompress_fd(c, in, o));
	close(in);
	close(o);
	mu_assert(same(src, out), "reused context");
	pc_free(c);
}

static int ncalls;

static void
count(void *a, int l, const char *m)
{
	(void) a; (void) l; (void) m;
	ncalls++;
}

MU_TEST(test_pub_garbage_input)
{
	pc_ctx_t *c = pc_new();
	int in = open(src, O_RDONLY), o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	/* PC_LOG_NONE: no callback at all, the error is still recorded. */
	ncalls = 0;
	pc_set_log(c, count, NULL, PC_LOG_NONE);
	mu_assert(pc_decompress_fd(c, in, o) != 0, "plain file decompressed");
	mu_assert_int_eq(PC_ERR_FORMAT, pc_last_error_code(c));
	mu_assert(pc_last_error(c)[0] != '\0', "no message");
	mu_assert(strcmp(pc_strerror(PC_ERR_FORMAT), pc_strerror(PC_OK)) != 0, "strerror");
	mu_assert_int_eq(0, ncalls);
	close(in);
	close(o);
	pc_free(c);
}

struct deflog {
	int n, errs, infos;
	char first_err[256];
};

static void
deflog_cb(void *a, int l, const char *m)
{
	struct deflog *d = (struct deflog *)a;

	d->n++;
	if (l == PC_LOG_INFO)
		d->infos++;
	if (l == PC_LOG_ERR && d->errs++ == 0)
		snprintf(d->first_err, sizeof (d->first_err), "%s", m);
}

/*
 * pc_set_default_log(): a context without its own sink, and a message
 * logged outside any context, reach the process sink at their level; a
 * context with its own sink does not. Runs last: the sink is process-wide.
 */
MU_TEST(test_pub_default_log)
{
	struct deflog d;
	pc_ctx_t *c;
	int in, o;

	memset(&d, 0, sizeof (d));
	pc_set_default_log(deflog_cb, &d, PC_LOG_INFO);
	c = pc_new();
	in = open(src, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert(pc_decompress_fd(c, in, o) != 0, "plain file decompressed");
	close(in);
	close(o);
	mu_assert(d.errs >= 1, "context error not delivered");
	mu_assert(strstr(d.first_err, pc_last_error(c)) != NULL, d.first_err);

	/* A context's own sink takes its messages. */
	memset(&d, 0, sizeof (d));
	pc_set_log(c, quiet, NULL, PC_LOG_VERBOSE);
	in = open(src, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert(pc_decompress_fd(c, in, o) != 0, "plain file decompressed");
	close(in);
	close(o);
	mu_assert_int_eq(0, d.n);
	pc_free(c);

	/* No context: the allocator is already in use, which is logged. */
	memset(&d, 0, sizeof (d));
	mu_assert_int_eq(-1, pc_set_allocator(NULL));
	mu_assert_int_eq(1, d.errs);
	mu_assert(strstr(d.first_err, "pc_set_allocator") != NULL, d.first_err);

	/* The level: PC_LOG_NONE delivers nothing; -1 keeps it. */
	memset(&d, 0, sizeof (d));
	pc_set_default_log(deflog_cb, &d, PC_LOG_NONE);
	pc_set_default_log(deflog_cb, &d, -1);
	mu_assert_int_eq(-1, pc_set_allocator(NULL));
	mu_assert_int_eq(0, d.n);
	pc_set_default_log(NULL, NULL, PC_LOG_INFO);
}

static int th_start, th_stop, th_badname, th_onmain;
static pthread_t th_main;

static void
th_start_cb(void *ud, const char *name)
{
	static const char *names[] = { "pc-compress", "pc-decompress", "pc-writer",
	    "pc-meta", "pc-archive", "pc-extract", NULL };
	int i;

	(void) ud;
	for (i = 0; names[i] != NULL && strcmp(names[i], name) != 0; i++)
		;
	if (names[i] == NULL)
		__atomic_add_fetch(&th_badname, 1, __ATOMIC_RELAXED);
	if (pthread_equal(pthread_self(), th_main))
		__atomic_add_fetch(&th_onmain, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&th_start, 1, __ATOMIC_RELAXED);
}

static void
th_stop_cb(void *ud)
{
	(void) ud;
	__atomic_add_fetch(&th_stop, 1, __ATOMIC_RELAXED);
}

/*
 * pc_set_thread_hooks(): a compress and a decompress with 4 threads start
 * workers and a writer each; every one runs start() in the new thread
 * with a known name, and stop() before the call returns.
 */
MU_TEST(test_pub_thread_hooks)
{
	pc_thread_hooks_t h = { sizeof (h), th_start_cb, th_stop_cb, NULL };
	pc_thread_hooks_t bad = { sizeof (h) - 1, th_start_cb, th_stop_cb, NULL };
	pc_ctx_t *c;
	int in, o;

	th_main = pthread_self();
	pc_set_default_log(quiet, NULL, -1);
	mu_assert_int_eq(-1, pc_set_thread_hooks(&bad));
	pc_set_default_log(NULL, NULL, -1);
	mu_assert_int_eq(0, pc_set_thread_hooks(&h));
	c = pc_new();
	/* lz4 is not multithreaded itself: 4 threads are 4 workers. */
	pc_set_algorithm(c, "lz4");
	pc_set_threads(c, 4);
	pc_set_chunk_size(c, 256 * 1024);
	in = open(src, O_RDONLY);
	o = open(cmp_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_int_eq(0, pc_compress_fd(c, in, o));
	close(in);
	close(o);
	/* The workers (4, or fewer on a smaller machine) and the writer. */
	{
#ifdef _WIN32
		long ncpu = 4;	/* no sysconf(_SC_NPROCESSORS_ONLN); CI has 4 or more */
#else
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif

		mu_assert_int_eq((ncpu > 0 && ncpu < 4 ? (int)ncpu : 4) + 1, th_start);
	}
	mu_assert_int_eq(th_start, th_stop);
	in = open(cmp_, O_RDONLY);
	o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_int_eq(0, pc_decompress_fd(c, in, o));
	close(in);
	close(o);
	pc_free(c);
	mu_assert(same(src, out), "round trip");
	mu_assert(th_start > 5, "decompress threads not hooked");
	mu_assert_int_eq(th_start, th_stop);
	mu_assert_int_eq(0, th_badname);
	mu_assert_int_eq(0, th_onmain);

	/* Removed: no more calls. */
	mu_assert_int_eq(0, pc_set_thread_hooks(NULL));
	th_start = 0;
	mu_assert_int_eq(0, roundtrip("lz4", 1, PC_DEDUPE_NONE, NULL, NULL));
	mu_assert_int_eq(0, th_start);
}

struct prog {
	int calls, stop_at;
	uint64_t last_in, last_out, backwards;
};

static int
prog_cb(void *a, uint64_t in, uint64_t out)
{
	struct prog *p = (struct prog *)a;

	if (in < p->last_in || out < p->last_out)
		p->backwards++;
	p->last_in = in;
	p->last_out = out;
	return (++p->calls == p->stop_at);
}

/* Descriptors above stderr that are open (fcntl() fails on a closed one). */
static int
open_fds(void)
{
	int n = 0, fd;

#ifndef _WIN32
	for (fd = 3; fd < 1024; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
#else
	(void) fd;	/* no fcntl(): the check counts nothing */
#endif
	return (n);
}

/* One descriptor job on c, src to cmp_ or cmp_ to out. */
static int
fd_job(pc_ctx_t *c, int compress)
{
	int in = open(compress ? src : cmp_, O_RDONLY);
	int o = open(compress ? cmp_ : out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	int rv = compress ? pc_compress_fd(c, in, o) : pc_decompress_fd(c, in, o);

	close(in);
	close(o);
	return (rv);
}

/*
 * pc_set_progress(): progress is reported per chunk and never goes back;
 * a callback that returns non-zero on its 3rd call stops a multi-chunk
 * job with 4 threads in either direction, cleanly: -1, PC_ERR_CANCELLED,
 * no descriptor left open, and the context can run the next job.
 */
MU_TEST(test_pub_progress_cancel)
{
	struct prog p;
	pc_ctx_t *c = pc_new();
	int fds = open_fds(), dir_;

	pc_set_algorithm(c, "lz4");
	pc_set_threads(c, 4);
	pc_set_chunk_size(c, 128 * 1024);	/* src is 17 chunks */
	pc_set_log(c, quiet, NULL, PC_LOG_ERR);

	/* Not cancelled: every chunk reported, in last = the input size. */
	memset(&p, 0, sizeof (p));
	pc_set_progress(c, prog_cb, &p);
	mu_assert_int_eq(0, fd_job(c, 1));
	mu_assert(p.calls >= 17 && p.last_in == src_len && p.backwards == 0, "compress progress");
	memset(&p, 0, sizeof (p));
	mu_assert_int_eq(0, fd_job(c, 0));
	mu_assert(same(src, out), "round trip with progress");
	mu_assert(p.calls >= 17 && p.backwards == 0, "decompress progress");

	for (dir_ = 1; dir_ >= 0; dir_--) {
		memset(&p, 0, sizeof (p));
		p.stop_at = 3;
		mu_assert_int_eq(-1, fd_job(c, dir_));
		mu_assert_int_eq(3, p.calls);
		mu_assert_int_eq(PC_ERR_CANCELLED, pc_last_error_code(c));
		mu_assert(strcmp(pc_strerror(PC_ERR_CANCELLED), "cancelled") == 0, "strerror");
		mu_assert_int_eq(fds, open_fds());
		if (dir_) {
			/* The cancelled compress left a partial stream: make a full one. */
			pc_set_progress(c, NULL, NULL);
			mu_assert_int_eq(0, fd_job(c, 1));
			pc_set_progress(c, prog_cb, &p);
		}
	}
	pc_free(c);
}

/*
 * The command-line style API: a cancelled compress removes the .pz it
 * was writing, and a cancelled decompress the file it was writing.
 */
MU_TEST(test_pub_progress_cancel_files)
{
	char args[1024], pz[330], target[320];
	struct prog p;
	pc_ctx_t *c;

	snprintf(target, sizeof (target), "%s/cancelled", dir);
	snprintf(pz, sizeof (pz), "%s.pz", target);
	unlink(pz);
	memset(&p, 0, sizeof (p));
	p.stop_at = 3;
	c = create_pc_context();
	pc_set_log(c, quiet, NULL, PC_LOG_ERR);
	snprintf(args, sizeof (args), "pcompress -c lz4 -l1 -s 128k -t 4 %s %s", src, target);
	mu_assert_int_eq(0, init_pc_context_argstr(c, args));
	pc_set_progress(c, prog_cb, &p);
	mu_assert(start_pcompress(c) != 0, "cancelled compress succeeded");
	mu_assert_int_eq(PC_ERR_CANCELLED, pc_last_error_code(c));
	destroy_pc_context(c);
	mu_assert(access(pz, F_OK) != 0, "partial .pz left behind");

	/* A whole one to decompress (the args string was split in place). */
	c = create_pc_context();
	snprintf(args, sizeof (args), "pcompress -c lz4 -l1 -s 128k -t 4 %s %s", src, target);
	mu_assert_int_eq(0, init_pc_context_argstr(c, args));
	mu_assert_int_eq(0, start_pcompress(c));
	destroy_pc_context(c);
	memset(&p, 0, sizeof (p));
	p.stop_at = 3;
	c = create_pc_context();
	pc_set_log(c, quiet, NULL, PC_LOG_ERR);
	snprintf(args, sizeof (args), "pcompress -d -t 4 %s %s", pz, target);
	mu_assert_int_eq(0, init_pc_context_argstr(c, args));
	pc_set_progress(c, prog_cb, &p);
	mu_assert(start_pcompress(c) != 0, "cancelled decompress succeeded");
	mu_assert_int_eq(PC_ERR_CANCELLED, pc_last_error_code(c));
	destroy_pc_context(c);
	mu_assert(access(target, F_OK) != 0, "partial output left behind");
	unlink(pz);
}

#ifndef _WIN32
/*
 * Cancelling an archive compression, with the metadata stream and
 * without it (-T): the archiver thread is released and joined, the call
 * fails with PC_ERR_CANCELLED, and neither a descriptor nor the partial
 * .pz is left behind.
 */
MU_TEST(test_pub_progress_cancel_archive)
{
	static const char *opts[] = { "", "-T" };
	char args[1024], tree[260], f[300], target[320], pz[330];
	struct prog p;
	pc_ctx_t *c;
	int i, fds;

	/*
	 * Archive mode looks up each file's owner, and the C library may keep
	 * a descriptor once it has (illumos: a door to nscd): look up first.
	 */
	(void) getpwuid(getuid());
	(void) getgrgid(getgid());
	fds = open_fds();

	/* Four 2 MB files: 32 chunks of 256k. */
	snprintf(tree, sizeof (tree), "%s/tree", dir);
	mkdir(tree, 0700);
	for (i = 0; i < 4; i++) {
		FILE *fp;
		size_t j;

		snprintf(f, sizeof (f), "%s/f%d", tree, i);
		mu_assert((fp = fopen(f, "wb")) != NULL, f);
		for (j = 0; j < 2 * 1024 * 1024; j++)
			fputc((j / 777) % 3 ? (int)("pcompress "[j % 10]) : (int)((j * 131 >> 3) + i), fp);
		fclose(fp);
	}
	snprintf(target, sizeof (target), "%s/arc", dir);
	snprintf(pz, sizeof (pz), "%s.pz", target);
	for (i = 0; i < 2; i++) {
		unlink(pz);
		memset(&p, 0, sizeof (p));
		p.stop_at = 2;
		c = create_pc_context();
		pc_set_log(c, quiet, NULL, PC_LOG_ERR);
		snprintf(args, sizeof (args), "pcompress -a %s -c lz4 -l1 -s 256k -t 4 %s %s",
		    opts[i], tree, target);
		mu_assert_int_eq(0, init_pc_context_argstr(c, args));
		pc_set_progress(c, prog_cb, &p);
		mu_assert(start_pcompress(c) != 0, "cancelled archive succeeded");
		mu_assert_int_eq(2, p.calls);
		mu_assert_int_eq(PC_ERR_CANCELLED, pc_last_error_code(c));
		destroy_pc_context(c);
		mu_assert(access(pz, F_OK) != 0, "partial archive left behind");
		mu_assert_int_eq(fds, open_fds());
	}
	for (i = 0; i < 4; i++) {
		snprintf(f, sizeof (f), "%s/f%d", tree, i);
		unlink(f);
	}
	rmdir(tree);
}
#endif

MU_TEST_SUITE(suite_public)
{
	MU_RUN_TEST(test_pub_version);
	MU_RUN_TEST(test_pub_roundtrips);
	MU_RUN_TEST(test_pub_algorithm_name_copied);
	MU_RUN_TEST(test_pub_password_not_wiped);
	MU_RUN_TEST(test_pub_long_password);
	MU_RUN_TEST(test_pub_wrong_password);
	MU_RUN_TEST(test_pub_bad_settings);
	MU_RUN_TEST(test_pub_fds_left_open);
	MU_RUN_TEST(test_pub_garbage_input);
	MU_RUN_TEST(test_pub_progress_cancel);
	MU_RUN_TEST(test_pub_progress_cancel_files);
#ifndef _WIN32
	MU_RUN_TEST(test_pub_progress_cancel_archive);
#endif
	MU_RUN_TEST(test_pub_thread_hooks);
	MU_RUN_TEST(test_pub_default_log);
}

int
main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof (dir), "%s/pc_pub_XXXXXX", t ? t : "/tmp");
	if (mkdtemp(dir) == NULL)
		return (1);
	snprintf(src, sizeof (src), "%s/in", dir);
	snprintf(cmp_, sizeof (cmp_), "%s/in.pz", dir);
	snprintf(out, sizeof (out), "%s/out", dir);
	if (write_src(2 * 1024 * 1024 + 333) != 0)
		return (1);
	MU_RUN_SUITE(suite_public);
	unlink(src);
	unlink(cmp_);
	unlink(out);
	rmdir(dir);
	MU_REPORT();
	return MU_EXIT_CODE;
}
