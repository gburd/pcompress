/*
 * Unit tests for the slab allocator.
 *
 * Tests basic alloc/free, calloc zeroing, cache pre-creation,
 * sequential reuse, and stress scenarios.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <allocator.h>
#include "minunit.h"

/* --- Basic allocation and free --- */

MU_TEST(test_slab_alloc_basic)
{
	void *p = slab_alloc(NULL, 1024);
	mu_assert(p != NULL, "slab_alloc(1024) returned NULL");
	/* Write to it to verify the memory is usable */
	memset(p, 0xAB, 1024);
	slab_free(NULL, p);
}

MU_TEST(test_slab_alloc_zero_size)
{
	/*
	 * Zero-size allocation behavior is implementation-defined.
	 * It should not crash regardless of the return value.
	 */
	void *p = slab_alloc(NULL, 0);
	if (p != NULL)
		slab_free(NULL, p);
}

MU_TEST(test_slab_alloc_large)
{
	void *p = slab_alloc(NULL, 4 * 1024 * 1024);
	mu_assert(p != NULL, "slab_alloc(4MiB) returned NULL");
	memset(p, 0xCD, 4 * 1024 * 1024);
	slab_free(NULL, p);
}

/* --- Calloc zeroing --- */

MU_TEST(test_slab_calloc_zeroed)
{
	unsigned char *p = (unsigned char *)slab_calloc(NULL, 256, 4);
	mu_assert(p != NULL, "slab_calloc returned NULL");
	int all_zero = 1;
	size_t i;
	for (i = 0; i < 256 * 4; i++) {
		if (p[i] != 0) {
			all_zero = 0;
			break;
		}
	}
	mu_assert(all_zero, "slab_calloc did not zero memory");
	slab_free(NULL, p);
}

/* --- Cache pre-creation --- */

MU_TEST(test_slab_cache_add)
{
	int rv = slab_cache_add(8192);
	mu_assert_int_eq(0, rv);
	/* Allocate from the pre-created cache */
	void *p = slab_alloc(NULL, 8192);
	mu_assert(p != NULL, "alloc from pre-created cache returned NULL");
	slab_free(NULL, p);
}

/* --- Reuse after free --- */

MU_TEST(test_slab_reuse)
{
	void *p1 = slab_alloc(NULL, 512);
	mu_assert(p1 != NULL, "first alloc returned NULL");
	slab_free(NULL, p1);

	void *p2 = slab_alloc(NULL, 512);
	mu_assert(p2 != NULL, "second alloc returned NULL");
	/*
	 * The slab allocator may or may not return the same pointer;
	 * we just verify it succeeds.
	 */
	slab_free(NULL, p2);
}

/* --- Stress: many allocations --- */

MU_TEST(test_slab_stress)
{
	#define NALLOCS 500
	void *ptrs[NALLOCS];
	int i;

	for (i = 0; i < NALLOCS; i++) {
		size_t sz = 64 + (i % 16) * 64;  /* 64 to 1024 */
		ptrs[i] = slab_alloc(NULL, sz);
		mu_assert(ptrs[i] != NULL, "stress alloc returned NULL");
		memset(ptrs[i], (unsigned char)i, sz);
	}
	for (i = 0; i < NALLOCS; i++) {
		slab_free(NULL, ptrs[i]);
	}
	#undef NALLOCS
}

/* --- Release (bypass cache) --- */

MU_TEST(test_slab_release)
{
	void *p = slab_alloc(NULL, 2048);
	mu_assert(p != NULL, "alloc for release returned NULL");
	memset(p, 0xFF, 2048);
	slab_release(NULL, p);
}

/* --- Multiple size classes --- */

MU_TEST(test_slab_mixed_sizes)
{
	void *p1 = slab_alloc(NULL, 32);
	void *p2 = slab_alloc(NULL, 4096);
	void *p3 = slab_alloc(NULL, 65536);
	mu_assert(p1 != NULL, "32-byte alloc returned NULL");
	mu_assert(p2 != NULL, "4096-byte alloc returned NULL");
	mu_assert(p3 != NULL, "64K alloc returned NULL");
	slab_free(NULL, p1);
	slab_free(NULL, p2);
	slab_free(NULL, p3);
}

/* --- Suites --- */

MU_TEST_SUITE(suite_slab_basic)
{
	MU_RUN_TEST(test_slab_alloc_basic);
	MU_RUN_TEST(test_slab_alloc_zero_size);
	MU_RUN_TEST(test_slab_alloc_large);
	MU_RUN_TEST(test_slab_calloc_zeroed);
	MU_RUN_TEST(test_slab_cache_add);
	MU_RUN_TEST(test_slab_reuse);
	MU_RUN_TEST(test_slab_release);
	MU_RUN_TEST(test_slab_mixed_sizes);
}

MU_TEST_SUITE(suite_slab_stress)
{
	MU_RUN_TEST(test_slab_stress);
}

int
main(void)
{
	slab_init();

	MU_RUN_SUITE(suite_slab_basic);
	MU_RUN_SUITE(suite_slab_stress);

	MU_REPORT();
	slab_cleanup(1);
	return MU_EXIT_CODE;
}
