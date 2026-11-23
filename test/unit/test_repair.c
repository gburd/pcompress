/*
 * Unit tests for the repair data (repair.c): the field and the Cauchy
 * code are pinned by vectors from an independent Python model (shift-
 * and-add multiplication, brute-force inverses; see the comment at
 * par_kat[]), every erasure pattern up to the parity count is rebuilt,
 * the vector multiply paths agree with the scalar one, the header
 * parser refuses what no writer makes, and a file with repair data is
 * repaired after damage within the bound and refused beyond it.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <utils.h>
#include <pc_alloc.h>
#include <repair.h>
#include "minunit.h"

/*
 * k = 5 data rows of 8 bytes, data[r][j] = (r * 37 + j * 11 + 1) & 0xff,
 * m = 3 parity rows, row i = sum over r of 1 / (i ^ (m + r)) * data[r]:
 *   def mul(a, b): p = 0; while b: (p ^= a if b & 1); a <<= 1;
 *       (a ^= 0x11d if a & 0x100); b >>= 1; return p
 *   inv(a) = the x in 1..255 with mul(a, x) == 1
 */
static const unsigned char par_kat[3][8] = {
	{ 0xdf, 0xb1, 0x6f, 0xcd, 0xe4, 0x47, 0x5e, 0x97 },
	{ 0x5e, 0x8b, 0x7b, 0xa2, 0x0e, 0x50, 0x5e, 0xac },
	{ 0xf6, 0xfc, 0xf2, 0xb2, 0x9b, 0xb2, 0xb3, 0x03 }
};

MU_TEST(test_rp_field)
{
	unsigned int a;

	mu_assert_int_eq(0x1d, rp_gf_mul(0x80, 2));
	mu_assert_int_eq(0x8f, rp_gf_mul(0x53, 0xca));
	mu_assert_int_eq(0x8c, rp_gf_inv(0x53));
	for (a = 1; a < 256; a++)
		mu_assert_int_eq(1, rp_gf_mul((unsigned char)a, rp_gf_inv((unsigned char)a)));
}

/* Encode the KAT stripe into blk (8 blocks of 8 bytes). */
static void
kat_stripe(unsigned char blk[8][8])
{
	uint32_t r, i, j;

	memset(blk, 0, 64);
	for (r = 0; r < 5; r++)
		for (j = 0; j < 8; j++)
			blk[r][j] = (unsigned char)((r * 37 + j * 11 + 1) & 0xff);
	for (i = 0; i < 3; i++)
		for (r = 0; r < 5; r++)
			rp_muladd(blk[5 + i], blk[r], rp_coef(i, r, 3), 8);
}

MU_TEST(test_rp_encode_kat)
{
	unsigned char blk[8][8];

	kat_stripe(blk);
	mu_assert_mem_eq(par_kat, blk[5], sizeof (par_kat));
}

/* Every set of up to 3 lost blocks of 8 comes back; 4 is refused. */
MU_TEST(test_rp_erasures)
{
	unsigned char good[8][8], blk[8][8], ok[8], *p[8];
	unsigned int mask, i, n;

	kat_stripe(good);
	for (mask = 0; mask < 256; mask++) {
		memcpy(blk, good, sizeof (blk));
		for (i = 0, n = 0; i < 8; i++) {
			ok[i] = !(mask & (1u << i));
			p[i] = blk[i];
			if (!ok[i]) {
				memset(blk[i], 0xa5, 8);
				n++;
			}
		}
		if (n <= 3) {
			mu_assert_int_eq(0, rp_stripe_fix(p, ok, 5, 3, 8));
			mu_assert_mem_eq(good, blk, sizeof (blk));
		} else {
			mu_assert_int_eq(-1, rp_stripe_fix(p, ok, 5, 3, 8));
		}
	}
}

/* The dispatched multiply equals the portable one on every length. */
MU_TEST(test_rp_muladd_paths)
{
	unsigned char src[300], d1[300], d2[300];
	unsigned int c, len, i;

	for (i = 0; i < sizeof (src); i++)
		src[i] = (unsigned char)(i * 151 + 7);
	for (c = 0; c < 256; c += 17) {
		for (len = 0; len <= sizeof (src); len += 37) {
			for (i = 0; i < sizeof (d1); i++)
				d1[i] = d2[i] = (unsigned char)(i ^ c);
			rp_muladd(d1, src, (unsigned char)c, len);
			rp_muladd_scalar(d2, src, (unsigned char)c, len);
			mu_assert_mem_eq(d2, d1, sizeof (d1));
		}
	}
}

static char dir[256];

/* path of name in the test's directory; two calls may be live at once. */
static const char *
tpath(const char *name)
{
	static char p[2][512];
	static int n;

	n = !n;
	snprintf(p[n], sizeof (p[n]), "%s/%s", dir, name);
	return (p[n]);
}

/*
 * Write a file of n pseudo-random bytes with pct percent repair data;
 * keep a copy of the protected bytes in *orig.
 */
static uint64_t
make_file(const char *name, uint64_t n, int pct, unsigned char **orig)
{
	unsigned char *b = pc_malloc(n);
	struct stat sb;
	uint64_t i, x = 88172645463325252ULL;
	int fd;

	for (i = 0; i < n; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		b[i] = (unsigned char)x;
	}
	fd = open(tpath(name), O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd == -1 || write(fd, b, n) != (ssize_t)n ||
	    rp_write(fd, n, 0, pct, NULL, NULL) != 0 || fstat(fd, &sb) != 0)
		return (0);
	close(fd);
	*orig = b;
	return ((uint64_t)sb.st_size);
}

/* Overwrite len bytes at off of the file with garbage. */
static void
smash(const char *name, uint64_t off, uint64_t len)
{
	unsigned char junk[65536];
	int fd = open(tpath(name), O_WRONLY, 0);

	memset(junk, 0x5a, sizeof (junk));
	if (fd == -1 || lseek(fd, (off_t)off, SEEK_SET) == -1 ||
	    write(fd, junk, len) != (ssize_t)len)
		perror("smash");
	close(fd);
}

/* The first n bytes of the file equal orig. */
static int
same(const char *name, const unsigned char *orig, uint64_t n)
{
	unsigned char *b = pc_malloc(n);
	int fd = open(tpath(name), O_RDONLY, 0), rv;

	rv = fd != -1 && read(fd, b, n) == (ssize_t)n && memcmp(b, orig, n) == 0;
	close(fd);
	pc_mfree(b);
	return (rv);
}

MU_TEST(test_rp_file)
{
	const uint64_t n = 3 * 1024 * 1024 + 123;	/* last block partial */
	unsigned char *orig = NULL;
	uint64_t fsize;
	int fd;

	fsize = make_file("a", n, 10, &orig);
	mu_assert(fsize > n, "rp_write failed");
	fd = open(tpath("a"), O_RDONLY, 0);
	mu_assert_uint64_eq(n, rp_logical_size(fd, fsize));
	close(fd);
	mu_assert_int_eq(0, rp_repair(tpath("a"), NULL));	/* intact */

	/*
	 * A burst of 64 KB (16 blocks, 16 different stripes), the partial
	 * last block, part of the parity and the first CRC table and header
	 * copy, all at once: rebuilt in place.
	 */
	smash("a", 1000000, 65536);
	smash("a", n - 50, 50);
	smash("a", n, 200);
	smash("a", fsize - 20000, 8192);
	mu_assert_int_eq(0, rp_repair(tpath("a"), NULL));
	mu_assert(same("a", orig, n), "data not repaired");
	mu_assert_int_eq(0, rp_repair(tpath("a"), NULL));

	/* Repaired copy to a new name; the damaged file is left alone. */
	smash("a", 2000000, 4096);
	mu_assert_int_eq(0, rp_repair(tpath("a"), tpath("b")));
	mu_assert(same("b", orig, n), "copy not repaired");
	mu_assert(!same("a", orig, n), "original changed");
	mu_assert_int_eq(-1, rp_repair(tpath("a"), tpath("b")));	/* b exists */

	/*
	 * A burst longer than the interleave covers (this file has 20
	 * stripes of 4 parity blocks: 80 blocks, 320 KB): refused, unchanged.
	 */
	for (fd = 0; fd < 8; fd++)
		smash("a", (uint64_t)fd * 65536, 65536);
	mu_assert_int_eq(-1, rp_repair(tpath("a"), NULL));
	mu_assert(!same("a", orig, n), "refused repair changed the file");
	pc_mfree(orig);
	unlink(tpath("a"));
	unlink(tpath("b"));

	/* No repair data: refused. */
	fd = open(tpath("c"), O_RDWR | O_CREAT | O_TRUNC, 0600);
	mu_assert(fd != -1 && write(fd, "not a pz file, and too short anyway", 35) == 35,
	    "write");
	close(fd);
	mu_assert_int_eq(-1, rp_repair(tpath("c"), NULL));
	unlink(tpath("c"));
}

/* A tiny file: one stripe, fewer blocks than k. */
MU_TEST(test_rp_small)
{
	unsigned char *orig = NULL;
	uint64_t fsize = make_file("s", 5000, 50, &orig);

	mu_assert(fsize > 5000, "rp_write failed");
	smash("s", 0, 4096);
	mu_assert_int_eq(0, rp_repair(tpath("s"), NULL));
	mu_assert(same("s", orig, 5000), "small file not repaired");
	pc_mfree(orig);
	unlink(tpath("s"));
}

MU_TEST(test_rp_hdr_hostile)
{
	unsigned char *orig = NULL, tr[RP_TRAILER_SZ], b[RP_HDR_SZ];
	uint64_t fsize = make_file("h", 100000, 5, &orig);
	rp_hdr_t h;
	int fd, i;

	mu_assert(fsize > 100000, "rp_write failed");
	fd = open(tpath("h"), O_RDONLY, 0);
	mu_assert(fd != -1 && lseek(fd, (off_t)(fsize - RP_TRAILER_SZ), SEEK_SET) != -1 &&
	    read(fd, tr, sizeof (tr)) == (ssize_t)sizeof (tr), "read trailer");
	close(fd);
	mu_assert_int_eq(0, rp_hdr_parse(tr, &h));
	mu_assert_uint64_eq(100000, h.plen);
	mu_assert_uint64_eq(fsize - 100000, h.rlen);
	/* Any changed byte of the fields or their hash is refused. */
	for (i = 0; i < 64; i++) {
		memcpy(b, tr, sizeof (b));
		b[i] ^= 1;
		mu_assert_int_eq(-1, rp_hdr_parse(b, &h));
	}
	/* Unsealed: the tag bytes must be zero. */
	memcpy(b, tr, sizeof (b));
	b[70] = 1;
	mu_assert_int_eq(-1, rp_hdr_parse(b, &h));
	pc_mfree(orig);
	unlink(tpath("h"));
}

MU_TEST_SUITE(suite_repair)
{
	MU_RUN_TEST(test_rp_field);
	MU_RUN_TEST(test_rp_encode_kat);
	MU_RUN_TEST(test_rp_erasures);
	MU_RUN_TEST(test_rp_muladd_paths);
	MU_RUN_TEST(test_rp_file);
	MU_RUN_TEST(test_rp_small);
	MU_RUN_TEST(test_rp_hdr_hostile);
}

int
main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof (dir), "%s/pc_repair_XXXXXX", t != NULL ? t : "/tmp");
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return (1);
	}
	MU_RUN_SUITE(suite_repair);
	MU_REPORT();
	rmdir(dir);
	return (MU_EXIT_CODE);
}
