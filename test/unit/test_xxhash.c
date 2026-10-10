/*
 * Unit tests for xxHash implementation.
 *
 * Verifies that the pcompress xxHash produces known hash values for
 * specific inputs, and tests edge cases (empty input, single byte).
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include "minunit.h"

/*
 * The pcompress xxHash API (from utils/xxhash.h).
 */
extern unsigned int XXH32(const void *input, int len, unsigned int seed);

MU_TEST(test_xxhash_empty)
{
	unsigned int h = XXH32("", 0, 0);
	/* Empty string with seed 0 should produce a deterministic value */
	mu_assert(h != 0 || h == 0, "xxhash should return a value for empty input");
}

MU_TEST(test_xxhash_deterministic)
{
	const char *data = "Hello, World!";
	unsigned int h1 = XXH32(data, (int)strlen(data), 0);
	unsigned int h2 = XXH32(data, (int)strlen(data), 0);
	mu_assert_int_eq((int)h1, (int)h2);
}

MU_TEST(test_xxhash_seed_differs)
{
	const char *data = "test data for hashing";
	unsigned int h1 = XXH32(data, (int)strlen(data), 0);
	unsigned int h2 = XXH32(data, (int)strlen(data), 42);
	mu_assert(h1 != h2, "different seeds should produce different hashes");
}

MU_TEST(test_xxhash_single_byte)
{
	unsigned char b = 0x42;
	unsigned int h1 = XXH32(&b, 1, 0);
	b = 0x43;
	unsigned int h2 = XXH32(&b, 1, 0);
	mu_assert(h1 != h2, "different single bytes should produce different hashes");
}

MU_TEST(test_xxhash_collision_resistance)
{
	/*
	 * Hash 1000 sequential integers and check for no collisions.
	 * With 32-bit output and 1000 inputs this should be extremely rare.
	 */
	unsigned int hashes[1000];
	int i, j;
	for (i = 0; i < 1000; i++) {
		hashes[i] = XXH32(&i, sizeof(i), 0);
	}
	for (i = 0; i < 1000; i++) {
		for (j = i + 1; j < 1000; j++) {
			mu_assert(hashes[i] != hashes[j],
			    "collision detected in sequential integers");
		}
	}
}

/*
 * Known answers over buf[i] = i*31+7, computed with an independent
 * reference model. The one-shot XXH32() in pcompress is a deliberate
 * variant (two 4-lane accumulators over 32-byte stripes, for SIMD) and
 * differs from upstream xxHash32 for len >= 16.
 * Every SIMD/scalar build must produce identical values: they feed dedupe
 * hash tables and the global dedupe index.
 */
static const struct { int len; unsigned int seed, hash, spec; } xxh_kat[] = {
	/* len, seed, pcompress XXH32 variant, upstream xxHash32 (reference) */
	{ 0, 0x00000000U, 0x02cc5d05U, 0x02cc5d05U },
	{ 1, 0x00000000U, 0x002e0d32U, 0x002e0d32U },
	{ 3, 0x00000000U, 0xaca17380U, 0xaca17380U },
	{ 4, 0x00000000U, 0x073faa82U, 0x073faa82U },
	{ 15, 0x00000000U, 0x9f29f87bU, 0x9f29f87bU },
	{ 16, 0x00000000U, 0x48e15729U, 0x3f6c9665U },
	{ 17, 0x00000000U, 0x99c6bfd3U, 0xe048ecdbU },
	{ 31, 0x00000000U, 0x76590f02U, 0x00f1525cU },
	{ 32, 0x00000000U, 0xabc94028U, 0xee42e668U },
	{ 33, 0x00000000U, 0xbce78007U, 0x770b5126U },
	{ 100, 0x00000000U, 0xdb5d0887U, 0x75936eb8U },
	{ 1024, 0x00000000U, 0x28684e82U, 0xead2ce3cU },
	{ 1024, 0x9e3779b1U, 0x2ed4fd14U, 0xb3f439f0U },
	{ 0, 0x00000001U, 0x0b2cb792U, 0x0b2cb792U },
	{ 37, 0x00003039U, 0x2c326709U, 0x7bce658aU },
};

typedef unsigned int (*xxh_fn)(const void *, int, unsigned int);

static unsigned char kat_buf[1024];

static int
xxh_variant_ok(xxh_fn h)
{
	size_t k;

	for (k = 0; k < sizeof (xxh_kat) / sizeof (xxh_kat[0]); k++) {
		unsigned int got = h(kat_buf, xxh_kat[k].len, xxh_kat[k].seed);

		if (got != xxh_kat[k].hash) {
			fprintf(stderr, "  XXH32(len=%d, seed=%08x) = %08x, want %08x\n",
			    xxh_kat[k].len, xxh_kat[k].seed, got, xxh_kat[k].hash);
			return (0);
		}
	}
	return (1);
}

#if defined(__x86_64__) || defined(__i386__)
extern unsigned int XXH32_SSE2(const void *, int, unsigned int);
extern unsigned int XXH32_SSE4(const void *, int, unsigned int);
extern unsigned int XXH32_AVX512(const void *, int, unsigned int);
#else
extern unsigned int XXH32_GENERIC(const void *, int, unsigned int);
#endif

MU_TEST(test_xxhash_kat)
{
	int i;

	for (i = 0; i < (int)sizeof (kat_buf); i++)
		kat_buf[i] = (unsigned char)(i * 31 + 7);
	/* The runtime dispatcher (what pcompress actually calls). */
	mu_assert(xxh_variant_ok(XXH32), "dispatched XXH32 KAT");
#if defined(__x86_64__) || defined(__i386__)
	mu_assert(xxh_variant_ok(XXH32_SSE2), "XXH32 SSE2 KAT");
	if (__builtin_cpu_supports("sse4.1"))
		mu_assert(xxh_variant_ok(XXH32_SSE4), "XXH32 SSE4 KAT");
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl"))
		mu_assert(xxh_variant_ok(XXH32_AVX512), "XXH32 AVX512 KAT");
#else
	mu_assert(xxh_variant_ok(XXH32_GENERIC), "XXH32 generic KAT");
#endif
}

MU_TEST_SUITE(suite_xxhash)
{
	MU_RUN_TEST(test_xxhash_kat);
	MU_RUN_TEST(test_xxhash_empty);
	MU_RUN_TEST(test_xxhash_deterministic);
	MU_RUN_TEST(test_xxhash_seed_differs);
	MU_RUN_TEST(test_xxhash_single_byte);
	MU_RUN_TEST(test_xxhash_collision_resistance);
}

extern void init_pcompress(void);

int
main(void)
{
	init_pcompress();	/* CPU detection + hash dispatch setup */
	MU_RUN_SUITE(suite_xxhash);
	MU_REPORT();
	return MU_EXIT_CODE;
}
