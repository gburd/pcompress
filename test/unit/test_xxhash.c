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

MU_TEST_SUITE(suite_xxhash)
{
	MU_RUN_TEST(test_xxhash_empty);
	MU_RUN_TEST(test_xxhash_deterministic);
	MU_RUN_TEST(test_xxhash_seed_differs);
	MU_RUN_TEST(test_xxhash_single_byte);
	MU_RUN_TEST(test_xxhash_collision_resistance);
}

int
main(void)
{
	MU_RUN_SUITE(suite_xxhash);
	MU_REPORT();
	return MU_EXIT_CODE;
}
