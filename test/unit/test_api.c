/*
 * Unit tests for the libpcompress public API (pcompress.h):
 * create/init/start/destroy, init_pc_context_argstr(), pc_set_userpw(),
 * and the return-code contract of init_pc_context().
 *
 * Each test round-trips a file through the library and compares bytes.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <utils.h>
#include <pcompress.h>
#include "minunit.h"

static char dir[256], src[300], cmp_[300], out[300];

static int
write_input(size_t n)
{
	FILE *f = fopen(src, "wb");
	size_t i;

	if (f == NULL)
		return (-1);
	for (i = 0; i < n; i++)
		fputc((int)((i * 2654435761u) >> 13) % 61 + 32, f);
	fclose(f);
	return (0);
}

static int
same_files(const char *a, const char *b)
{
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	int ca, cb, same = (fa && fb);

	while (same) {
		ca = fgetc(fa);
		cb = fgetc(fb);
		if (ca != cb)
			same = 0;
		if (ca == EOF || cb == EOF)
			break;
	}
	if (fa) fclose(fa);
	if (fb) fclose(fb);
	return (same);
}

/* Run one library invocation from a space-separated argument string. */
static int
run_args(const char *args, unsigned char *pw, int pwlen)
{
	char buf[1024];
	pc_ctx_t *pctx;
	int rv;

	snprintf(buf, sizeof (buf), "%s", args);
	pctx = create_pc_context();
	if (pctx == NULL)
		return (-1);
	rv = init_pc_context_argstr(pctx, buf);
	if (rv == 0) {
		if (pw != NULL)
			pc_set_userpw(pctx, pw, pwlen);
		rv = start_pcompress(pctx);
	}
	destroy_pc_context(pctx);
	return (rv);
}

static void
reset_files(void)
{
	unlink(cmp_);
	unlink(out);
}

MU_TEST(test_api_roundtrip_algos)
{
	static const char *algos[] = { "lz4", "zlib", "zstd", "lzma", "bzip2",
	    "lzfx", "ppmd", "adapt", "adapt2", "none",
#ifndef WORDS_BIGENDIAN
	    "libbsc",	/* refused on big-endian hosts */
#endif
	    NULL };
	char args[1024];
	int i;

	for (i = 0; algos[i] != NULL; i++) {
		reset_files();
		snprintf(args, sizeof (args), "pcompress -c %s -l 6 -s 1m %s %s",
		    algos[i], src, cmp_);
		mu_assert_int_eq(0, run_args(args, NULL, 0));
		snprintf(args, sizeof (args), "pcompress -d %s %s", cmp_, out);
		mu_assert_int_eq(0, run_args(args, NULL, 0));
		mu_assert(same_files(src, out), algos[i]);
	}
}

MU_TEST(test_api_userpw)
{
	unsigned char pw1[] = "correct horse", pw2[] = "correct horse";
	unsigned char bad[] = "wrong";
	char args[1024];

	reset_files();
	snprintf(args, sizeof (args), "pcompress -c zstd -l 3 -e AES %s %s",
	    src, cmp_);
	mu_assert_int_eq(0, run_args(args, pw1, (int)strlen((char *)pw1)));
	/* The library must wipe the caller's password buffer. */
	mu_assert(pw1[0] == 0, "password buffer not cleared");

	snprintf(args, sizeof (args), "pcompress -d %s %s", cmp_, out);
	mu_assert(run_args(args, bad, (int)strlen((char *)bad)) != 0,
	    "wrong password accepted");
	unlink(out);
	mu_assert_int_eq(0, run_args(args, pw2, (int)strlen((char *)pw2)));
	mu_assert(same_files(src, out), "AES round trip via pc_set_userpw");
}

MU_TEST(test_api_return_codes)
{
	char args[1024];

	/* Usage error: neither -c nor -d. */
	mu_assert_int_eq(2, run_args("pcompress -l 3", NULL, 0));
	/* -h / -V */
	mu_assert_int_eq(3, run_args("pcompress -h", NULL, 0));
	mu_assert_int_eq(4, run_args("pcompress -V", NULL, 0));
	/* Invalid argument values. */
	snprintf(args, sizeof (args), "pcompress -c nosuchalgo %s %s", src, cmp_);
	mu_assert_int_eq(1, run_args(args, NULL, 0));
	snprintf(args, sizeof (args), "pcompress -c zstd -l 99 %s %s", src, cmp_);
	mu_assert_int_eq(1, run_args(args, NULL, 0));
	/* Empty argument string is a no-op. */
	{
		pc_ctx_t *p = create_pc_context();
		mu_assert_int_eq(0, init_pc_context_argstr(p, (char[]){ "" }));
		destroy_pc_context(p);
	}
	/* start without init must fail, not crash. */
	{
		pc_ctx_t *p = create_pc_context();
		mu_assert(start_pcompress(p) != 0, "start without init succeeded");
		destroy_pc_context(p);
	}
}

struct logcap {
	int n, errs;
	char last[256];
};

static void
capture(void *arg, int level, const char *msg)
{
	struct logcap *c = (struct logcap *)arg;

	c->n++;
	if (level == LOG_ERR)
		c->errs++;
	snprintf(c->last, sizeof (c->last), "%s", msg);
}

MU_TEST(test_api_log_callback)
{
	struct logcap cap;
	char args[1024];
	pc_ctx_t *p;

	/* A decompress of a non-pcompress file: error goes to the callback. */
	memset(&cap, 0, sizeof (cap));
	snprintf(args, sizeof (args), "pcompress -d %s %s", src, out);
	p = create_pc_context();
	pc_set_log(p, capture, &cap, LOG_INFO);
	mu_assert_int_eq(0, init_pc_context_argstr(p, args));
	mu_assert(start_pcompress(p) != 0, "decompressing plain text succeeded");
	mu_assert(cap.errs >= 1, "error not delivered to the callback");
	mu_assert(strstr(pc_last_error(p), "not a pcompressed file") != NULL,
	    pc_last_error(p));
	mu_assert_int_eq(PC_ERR_FORMAT, pc_last_error_code(p));
	mu_assert(strcmp(pc_strerror(PC_ERR_FORMAT), "no error") != 0, "strerror");
	destroy_pc_context(p);

	/* Level filter: LOG_ERR only drops "Scaling to ..." (LOG_INFO). */
	memset(&cap, 0, sizeof (cap));
	reset_files();
	snprintf(args, sizeof (args), "pcompress -c lz4 -l 1 %s %s", src, cmp_);
	p = create_pc_context();
	pc_set_log(p, capture, &cap, LOG_ERR);
	mu_assert_int_eq(0, init_pc_context_argstr(p, args));
	mu_assert_int_eq(0, start_pcompress(p));
	mu_assert_int_eq(0, cap.n);
	mu_assert(pc_last_error(p)[0] == '\0', "error recorded on success");
	mu_assert_int_eq(PC_OK, pc_last_error_code(p));
	destroy_pc_context(p);

	/* Messages from the context's worker threads reach its callback. */
	memset(&cap, 0, sizeof (cap));
	snprintf(args, sizeof (args), "pcompress -d %s %s", cmp_, out);
	p = create_pc_context();
	pc_set_log(p, capture, &cap, LOG_INFO);
	mu_assert_int_eq(0, init_pc_context_argstr(p, args));
	mu_assert_int_eq(0, start_pcompress(p));
	mu_assert(cap.n >= 1 && strstr(cap.last, "Scaling") != NULL, cap.last);
	destroy_pc_context(p);
}

/* Error classes: a corrupted chunk is reported by a worker thread. */
MU_TEST(test_api_error_codes)
{
	char args[1024], pz[400];
	pc_ctx_t *p;
	FILE *f;
	long n;

	p = create_pc_context();
	pc_set_log(p, capture, &(struct logcap){0}, LOG_ERR);
	mu_assert_int_eq(1, init_pc_context_argstr(p, (char[]){ "pcompress -c nosuchalgo a b" }));
	mu_assert_int_eq(PC_ERR_ARGS, pc_last_error_code(p));
	destroy_pc_context(p);

	reset_files();
	snprintf(args, sizeof (args), "pcompress -c zstd -l 3 -s 64k %s %s", src, cmp_);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	/* Flip a byte in the middle of the data: a checksum must catch it. */
	snprintf(pz, sizeof (pz), "%s", cmp_);
	f = fopen(pz, "r+b");
	mu_assert(f != NULL, "open .pz");
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, n / 2, SEEK_SET);
	{
		int c = fgetc(f);

		fseek(f, n / 2, SEEK_SET);
		fputc(c ^ 0xff, f);
	}
	fclose(f);
	snprintf(args, sizeof (args), "pcompress -d %s %s", cmp_, out);
	p = create_pc_context();
	pc_set_log(p, capture, &(struct logcap){0}, LOG_ERR);
	mu_assert_int_eq(0, init_pc_context_argstr(p, args));
	mu_assert(start_pcompress(p) != 0, "corrupt file decompressed");
	mu_assert(pc_last_error_code(p) == PC_ERR_VERIFY ||
	    pc_last_error_code(p) == PC_ERR_CORRUPT, pc_last_error(p));
	destroy_pc_context(p);
}

/* Option parsing: clustering, attached/separate arguments, "--". */
MU_TEST(test_api_option_forms)
{
	char args[1024];

	reset_files();
	/* -l6 attached, "-s 1m" separate, cluster "-Cc" with an argument. */
	snprintf(args, sizeof (args), "pcompress -l6 -s 1m -Cc zstd %s %s", src, cmp_);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	snprintf(args, sizeof (args), "pcompress -d -- %s %s", cmp_, out);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	mu_assert(same_files(src, out), "option forms round trip");
	/* Unknown option and a missing argument are usage errors (2). */
	mu_assert_int_eq(2, run_args("pcompress -c zstd -Q x y", NULL, 0));
	mu_assert_int_eq(2, run_args("pcompress -c", NULL, 0));
	/* Options after operands, as glibc getopt allowed: "in -l1 out -c lz4". */
	reset_files();
	snprintf(args, sizeof (args), "pcompress %s -l1 %s -c lz4", src, cmp_);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	snprintf(args, sizeof (args), "pcompress %s %s -d", cmp_, out);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	mu_assert(same_files(src, out), "permuted options round trip");
	/* Parsing twice in one process gives the same result. */
	reset_files();
	snprintf(args, sizeof (args), "pcompress -c lz4 -l1 %s %s", src, cmp_);
	mu_assert_int_eq(0, run_args(args, NULL, 0));
	reset_files();
	mu_assert_int_eq(0, run_args(args, NULL, 0));
}

MU_TEST_SUITE(suite_api)
{
	MU_RUN_TEST(test_api_option_forms);
	MU_RUN_TEST(test_api_roundtrip_algos);
	MU_RUN_TEST(test_api_userpw);
	MU_RUN_TEST(test_api_return_codes);
	MU_RUN_TEST(test_api_log_callback);
	MU_RUN_TEST(test_api_error_codes);
}

int
main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof (dir), "%s/pc_api_XXXXXX", t ? t : "/tmp");
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return (1);
	}
	snprintf(src, sizeof (src), "%s/in.dat", dir);
	snprintf(cmp_, sizeof (cmp_), "%s/in.pz", dir);
	snprintf(out, sizeof (out), "%s/out.dat", dir);
	if (write_input(3 * 1024 * 1024 + 17) != 0)
		return (1);

	MU_RUN_SUITE(suite_api);
	reset_files();
	unlink(src);
	rmdir(dir);
	MU_REPORT();
	return MU_EXIT_CODE;
}
