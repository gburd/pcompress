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

MU_TEST_SUITE(suite_public)
{
	MU_RUN_TEST(test_pub_version);
	MU_RUN_TEST(test_pub_roundtrips);
	MU_RUN_TEST(test_pub_password_not_wiped);
	MU_RUN_TEST(test_pub_long_password);
	MU_RUN_TEST(test_pub_wrong_password);
	MU_RUN_TEST(test_pub_bad_settings);
	MU_RUN_TEST(test_pub_fds_left_open);
	MU_RUN_TEST(test_pub_garbage_input);
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
