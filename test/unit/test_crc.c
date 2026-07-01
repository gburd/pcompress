/*
 * Unit tests for CRC32 and CRC64 implementations from the LZMA SDK.
 *
 * These CRC functions are used pervasively in pcompress for data integrity,
 * crypto nonces, and deduplication checksums.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lzma_crc.h>
#include "minunit.h"

/* Well-known CRC32 test vector: "123456789" -> 0xCBF43926 */
MU_TEST(test_crc32_known_vector)
{
	const uint8_t data[] = "123456789";
	uint32_t crc = lzma_crc32(data, 9, 0);
	mu_assert_int_eq((int)0xCBF43926, (int)crc);
}

MU_TEST(test_crc32_empty)
{
	uint32_t crc = lzma_crc32((const uint8_t *)"", 0, 0);
	mu_assert_int_eq(0, (int)crc);
}

MU_TEST(test_crc32_single_byte)
{
	uint8_t b = 0;
	uint32_t c1 = lzma_crc32(&b, 1, 0);
	b = 1;
	uint32_t c2 = lzma_crc32(&b, 1, 0);
	mu_assert(c1 != c2, "different bytes should produce different CRC32");
}

MU_TEST(test_crc32_incremental)
{
	/*
	 * Verify that computing CRC32 in one shot matches computing it
	 * incrementally in two parts.
	 */
	const uint8_t data[] = "Hello, CRC32 world!";
	size_t len = strlen((const char *)data);
	size_t split = len / 2;

	uint32_t one_shot = lzma_crc32(data, len, 0);
	uint32_t part1 = lzma_crc32(data, split, 0);
	uint32_t incremental = lzma_crc32(data + split, len - split, part1);

	mu_assert_int_eq((int)one_shot, (int)incremental);
}

MU_TEST(test_crc64_empty)
{
	uint64_t crc = lzma_crc64((const uint8_t *)"", 0, 0);
	mu_assert_uint64_eq(0ULL, crc);
}

MU_TEST(test_crc64_deterministic)
{
	const uint8_t data[] = "Test CRC64 determinism";
	uint64_t c1 = lzma_crc64(data, strlen((const char *)data), 0);
	uint64_t c2 = lzma_crc64(data, strlen((const char *)data), 0);
	mu_assert_uint64_eq(c1, c2);
}

MU_TEST(test_crc64_incremental)
{
	const uint8_t data[] = "Incremental CRC64 test data string";
	size_t len = strlen((const char *)data);
	size_t split = len / 2;

	uint64_t one_shot = lzma_crc64(data, len, 0);
	uint64_t part1 = lzma_crc64(data, split, 0);
	uint64_t incremental = lzma_crc64(data + split, len - split, part1);

	mu_assert_uint64_eq(one_shot, incremental);
}

MU_TEST(test_crc64_differs_from_crc32)
{
	const uint8_t data[] = "Same input, different algorithm";
	uint32_t c32 = lzma_crc32(data, strlen((const char *)data), 0);
	uint64_t c64 = lzma_crc64(data, strlen((const char *)data), 0);
	/*
	 * While they could theoretically match in the lower 32 bits, the
	 * algorithms use different polynomials so this would be a strong
	 * indicator of a bug.
	 */
	mu_assert((uint64_t)c32 != c64,
	    "CRC32 and CRC64 should produce different values");
}

MU_TEST_SUITE(suite_crc32)
{
	MU_RUN_TEST(test_crc32_known_vector);
	MU_RUN_TEST(test_crc32_empty);
	MU_RUN_TEST(test_crc32_single_byte);
	MU_RUN_TEST(test_crc32_incremental);
}

MU_TEST_SUITE(suite_crc64)
{
	MU_RUN_TEST(test_crc64_empty);
	MU_RUN_TEST(test_crc64_deterministic);
	MU_RUN_TEST(test_crc64_incremental);
	MU_RUN_TEST(test_crc64_differs_from_crc32);
}

int
main(void)
{
	MU_RUN_SUITE(suite_crc32);
	MU_RUN_SUITE(suite_crc64);
	MU_REPORT();
	return MU_EXIT_CODE;
}
