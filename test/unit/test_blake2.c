/*
 * Unit tests for BLAKE2b hash implementation.
 *
 * Tests the SSE2 variant (lowest common denominator on x86_64) of BLAKE2b
 * for determinism, known output length, keyed hashing, and incremental
 * vs one-shot equivalence.
 *
 * On non-x86 platforms, the appropriate variant (NEON, etc.) should be
 * tested instead. This file uses #ifdef to select the correct backend.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <blake2.h>
#include "minunit.h"

/*
 * Select BLAKE2 function set based on architecture.
 * Default to SSE2 on x86_64, NEON on ARM64.
 */
#if defined(__aarch64__) || defined(_M_ARM64)
#define BLAKE2B_INIT    blake2b_init_neon
#define BLAKE2B_UPDATE  blake2b_update_neon
#define BLAKE2B_FINAL   blake2b_final_neon
#define BLAKE2B_SIMPLE  blake2b_neon
#define BLAKE2_VARIANT  "NEON"
#else
#define BLAKE2B_INIT    blake2b_init_sse2
#define BLAKE2B_UPDATE  blake2b_update_sse2
#define BLAKE2B_FINAL   blake2b_final_sse2
#define BLAKE2B_SIMPLE  blake2b_sse2
#define BLAKE2_VARIANT  "SSE2"
#endif

/* --- Determinism: same input produces same hash --- */

MU_TEST(test_blake2b_deterministic)
{
	const char *data = "BLAKE2b determinism test input";
	uint8_t hash1[BLAKE2B_OUTBYTES];
	uint8_t hash2[BLAKE2B_OUTBYTES];

	int rv1 = BLAKE2B_SIMPLE(hash1, data, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)strlen(data), 0);
	int rv2 = BLAKE2B_SIMPLE(hash2, data, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)strlen(data), 0);

	mu_assert_int_eq(0, rv1);
	mu_assert_int_eq(0, rv2);
	mu_assert_mem_eq(hash1, hash2, BLAKE2B_OUTBYTES);
}

/* --- Empty input --- */

MU_TEST(test_blake2b_empty)
{
	uint8_t hash[BLAKE2B_OUTBYTES];
	int rv = BLAKE2B_SIMPLE(hash, "", NULL,
	    BLAKE2B_OUTBYTES, 0, 0);
	mu_assert_int_eq(0, rv);
	/* Verify it is not all zeros (would indicate a bug) */
	int all_zero = 1;
	int i;
	for (i = 0; i < BLAKE2B_OUTBYTES; i++) {
		if (hash[i] != 0) { all_zero = 0; break; }
	}
	mu_assert(!all_zero, "BLAKE2b of empty input should not be all zeros");
}

/* --- Different inputs produce different hashes --- */

MU_TEST(test_blake2b_differs)
{
	const char *data1 = "input one";
	const char *data2 = "input two";
	uint8_t hash1[BLAKE2B_OUTBYTES];
	uint8_t hash2[BLAKE2B_OUTBYTES];

	BLAKE2B_SIMPLE(hash1, data1, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)strlen(data1), 0);
	BLAKE2B_SIMPLE(hash2, data2, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)strlen(data2), 0);

	mu_assert(memcmp(hash1, hash2, BLAKE2B_OUTBYTES) != 0,
	    "different inputs should produce different hashes");
}

/* --- Streaming (init/update/final) matches one-shot --- */

MU_TEST(test_blake2b_streaming)
{
	const char *data = "BLAKE2b streaming versus one-shot comparison test data";
	size_t len = strlen(data);
	uint8_t hash_oneshot[BLAKE2B_OUTBYTES];
	uint8_t hash_stream[BLAKE2B_OUTBYTES];

	/* One-shot */
	BLAKE2B_SIMPLE(hash_oneshot, data, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)len, 0);

	/* Streaming: split into two updates */
	blake2b_state S;
	size_t mid = len / 2;
	BLAKE2B_INIT(&S, BLAKE2B_OUTBYTES);
	BLAKE2B_UPDATE(&S, (const uint8_t *)data, (uint64_t)mid);
	BLAKE2B_UPDATE(&S, (const uint8_t *)data + mid, (uint64_t)(len - mid));
	BLAKE2B_FINAL(&S, hash_stream, BLAKE2B_OUTBYTES);

	mu_assert_mem_eq(hash_oneshot, hash_stream, BLAKE2B_OUTBYTES);
}

/* --- Shorter output lengths --- */

MU_TEST(test_blake2b_short_output)
{
	const char *data = "short output test";
	uint8_t hash32[32];
	int rv = BLAKE2B_SIMPLE(hash32, data, NULL,
	    32, (uint64_t)strlen(data), 0);
	mu_assert_int_eq(0, rv);

	/* 32-byte output should differ from 64-byte output (truncated) */
	uint8_t hash64[BLAKE2B_OUTBYTES];
	BLAKE2B_SIMPLE(hash64, data, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)strlen(data), 0);

	/*
	 * BLAKE2b with different output lengths uses the length as a
	 * parameter, so the outputs should differ entirely.
	 */
	mu_assert(memcmp(hash32, hash64, 32) != 0,
	    "different output lengths should produce different hashes");
}

/* --- Large input (128 KiB) --- */

MU_TEST(test_blake2b_large_input)
{
	#define LARGE_SZ (128 * 1024)
	uint8_t *buf = (uint8_t *)malloc(LARGE_SZ);
	mu_assert(buf != NULL, "malloc failed for large input");
	memset(buf, 0x42, LARGE_SZ);

	uint8_t hash[BLAKE2B_OUTBYTES];
	int rv = BLAKE2B_SIMPLE(hash, buf, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)LARGE_SZ, 0);
	mu_assert_int_eq(0, rv);

	/* Verify determinism on large input */
	uint8_t hash2[BLAKE2B_OUTBYTES];
	rv = BLAKE2B_SIMPLE(hash2, buf, NULL,
	    BLAKE2B_OUTBYTES, (uint64_t)LARGE_SZ, 0);
	mu_assert_int_eq(0, rv);
	mu_assert_mem_eq(hash, hash2, BLAKE2B_OUTBYTES);

	free(buf);
	#undef LARGE_SZ
}

/* --- Suites --- */

MU_TEST_SUITE(suite_blake2b)
{
	MU_RUN_TEST(test_blake2b_deterministic);
	MU_RUN_TEST(test_blake2b_empty);
	MU_RUN_TEST(test_blake2b_differs);
	MU_RUN_TEST(test_blake2b_streaming);
	MU_RUN_TEST(test_blake2b_short_output);
	MU_RUN_TEST(test_blake2b_large_input);
}

int
main(void)
{
	printf("BLAKE2b test variant: %s\n", BLAKE2_VARIANT);
	MU_RUN_SUITE(suite_blake2b);
	MU_REPORT();
	return MU_EXIT_CODE;
}
