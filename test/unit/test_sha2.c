/*
 * Known-answer tests for the chunk checksums as pcompress computes them
 * (compute_checksum()), so every CPU and build produces the same digest.
 *
 * "SHA256" in pcompress is SHA-512/256. Vectors from FIPS 180-4 / Python
 * hashlib over: "", "abc", and bytes 0..255 repeated 5 times.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <crypto_utils.h>
#include "minunit.h"

static const struct { int len; const char *sha512, *sha512_256; } kat[] = {
	{ 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "c672b8d1ef56ed28ab87c3622c5114069bdd3ad7b8f9737498d0c01ecef0967a" },
	{ 3, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23" },
	{ 1280, "c93f55ccf2fa8c82699ff9b58afe3591242b135d908a6d865e17e38adb41c21d1d5359e51273036373d54d20b5659cc87e6e7b381ff027d33f971416cc590f90", "29cf77853d4ed12414976cdb42377b05c197eef3bb25eb4bd86e79f4e17c96bd" },
};

static unsigned char msg[1280];

static int
digest_is(const char *name, int len, const char *hex)
{
	int ck = 0, bytes = 0, mac = 0, i;
	unsigned char out[64];
	char got[129];

	if (get_checksum_props(name, &ck, &bytes, &mac, 0) != 0)
		return (0);
	if (compute_checksum(out, ck, len == 3 ? (uchar_t *)"abc" : msg, len, 0, 0) != 0)
		return (0);
	for (i = 0; i < bytes; i++)
		sprintf(got + 2 * i, "%02x", out[i]);
	if (strcmp(got, hex) != 0) {
		fprintf(stderr, "  %s(len=%d) = %s\n  want %s\n", name, len, got, hex);
		return (0);
	}
	return (1);
}

MU_TEST(test_sha512_kat)
{
	size_t k;

	for (k = 0; k < sizeof (kat) / sizeof (kat[0]); k++)
		mu_assert(digest_is("SHA512", kat[k].len, kat[k].sha512), "SHA512 KAT");
}

MU_TEST(test_sha512_256_kat)
{
	size_t k;

	for (k = 0; k < sizeof (kat) / sizeof (kat[0]); k++)
		mu_assert(digest_is("SHA256", kat[k].len, kat[k].sha512_256),
		    "SHA256 (SHA-512/256) KAT");
}

/* Parallel (single-chunk) mode must equal itself across runs and differ
 * from serial (it is a Merkle tree, not plain SHA-512). */
MU_TEST(test_sha_parallel_stable)
{
	int ck = 0, bytes = 0, mac = 0;
	static unsigned char big[1 << 20];
	unsigned char a[64], b[64], s[64];
	size_t i;

	for (i = 0; i < sizeof (big); i++)
		big[i] = (unsigned char)(i * 131 + (i >> 9));
	mu_assert_int_eq(0, get_checksum_props("SHA512", &ck, &bytes, &mac, 0));
	mu_assert_int_eq(0, compute_checksum(a, ck, big, sizeof (big), 1, 0));
	mu_assert_int_eq(0, compute_checksum(b, ck, big, sizeof (big), 1, 0));
	mu_assert_int_eq(0, compute_checksum(s, ck, big, sizeof (big), 0, 0));
	mu_assert_mem_eq(a, b, 64);
	mu_assert(memcmp(a, s, 64) != 0, "parallel digest equals serial");
}

MU_TEST_SUITE(suite_sha2)
{
	MU_RUN_TEST(test_sha512_kat);
	MU_RUN_TEST(test_sha512_256_kat);
	MU_RUN_TEST(test_sha_parallel_stable);
}

extern void init_pcompress(void);

int
main(void)
{
	int i;

	init_pcompress();
	for (i = 0; i < (int)sizeof (msg); i++)
		msg[i] = (unsigned char)(i & 0xff);
	MU_RUN_SUITE(suite_sha2);
	MU_REPORT();
	return MU_EXIT_CODE;
}
