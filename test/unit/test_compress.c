/*
 * Unit tests for compression algorithm round-trip correctness.
 *
 * Tests each algorithm's compress/decompress cycle at multiple levels
 * with various data patterns (zeros, random, text-like, binary).
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <pcompress.h>
#include "minunit.h"

#define TEST_BUFSZ	(256 * 1024)	/* 256 KiB test buffer */

/*
 * No-op deinit for algorithms that don't allocate state (e.g. bzip2).
 */
static int
noop_deinit(void **data)
{
	*data = NULL;
	return (0);
}
#define CHUNK_SZ	(256 * 1024)

static unsigned char src_buf[TEST_BUFSZ];
static unsigned char cmp_buf[TEST_BUFSZ * 2];
static unsigned char dec_buf[TEST_BUFSZ];

/*
 * Fill buffer with a repeating text-like pattern (compressible).
 */
static void
fill_text_pattern(unsigned char *buf, size_t len)
{
	const char *pattern = "The quick brown fox jumps over the lazy dog. ";
	size_t plen = strlen(pattern);
	size_t pos = 0;

	while (pos < len) {
		size_t n = plen;
		if (pos + n > len) n = len - pos;
		memcpy(buf + pos, pattern, n);
		pos += n;
	}
}

/*
 * Fill buffer with pseudo-random bytes (less compressible).
 */
static void
fill_random_pattern(unsigned char *buf, size_t len)
{
	unsigned int seed = 12345;
	size_t i;
	for (i = 0; i < len; i++) {
		seed = seed * 1103515245 + 12345;
		buf[i] = (unsigned char)(seed >> 16);
	}
}

/*
 * Test a single algorithm round-trip:
 *   init -> compress -> decompress -> compare -> deinit
 */
static int
test_algo_roundtrip(init_func_ptr init_fn, deinit_func_ptr deinit_fn,
    compress_func_ptr compress_fn, compress_func_ptr decompress_fn,
    int level, unsigned char *data, size_t datalen)
{
	void *algo_data = NULL;
	int lvl = level;
	uint64_t cmplen, declen;
	int rv;

	rv = init_fn(&algo_data, &lvl, 1, CHUNK_SZ, VERSION, COMPRESS);
	if (rv != 0) return -1;

	cmplen = (uint64_t)(datalen * 2);
	rv = compress_fn(data, (uint64_t)datalen, cmp_buf, &cmplen,
	    lvl, 0, DATA_BINARY, algo_data);
	deinit_fn(&algo_data);
	if (rv != 0) return -2;

	/* Re-init for decompression */
	lvl = level;
	rv = init_fn(&algo_data, &lvl, 1, CHUNK_SZ, VERSION, DECOMPRESS);
	if (rv != 0) return -3;

	declen = (uint64_t)datalen;
	rv = decompress_fn(cmp_buf, cmplen, dec_buf, &declen,
	    lvl, 0, DATA_BINARY, algo_data);
	deinit_fn(&algo_data);
	if (rv != 0) return -4;

	if (declen != (uint64_t)datalen) return -5;
	if (memcmp(data, dec_buf, datalen) != 0) return -6;

	return 0;
}

/* --- LZ4 Tests --- */

MU_TEST(test_lz4_text_l1)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lz4_init, lz4_deinit,
	        lz4_compress, lz4_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lz4_text_l9)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lz4_init, lz4_deinit,
	        lz4_compress, lz4_decompress, 9, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lz4_random)
{
	fill_random_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lz4_init, lz4_deinit,
	        lz4_compress, lz4_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lz4_zeros)
{
	memset(src_buf, 0, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lz4_init, lz4_deinit,
	        lz4_compress, lz4_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lz4_small)
{
	fill_text_pattern(src_buf, MIN_CHUNK);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lz4_init, lz4_deinit,
	        lz4_compress, lz4_decompress, 1, src_buf, MIN_CHUNK));
}

/* --- Zlib Tests --- */

MU_TEST(test_zlib_text_l1)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zlib_init, zlib_deinit,
	        zlib_compress, zlib_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zlib_text_l9)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zlib_init, zlib_deinit,
	        zlib_compress, zlib_decompress, 9, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zlib_random)
{
	fill_random_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zlib_init, zlib_deinit,
	        zlib_compress, zlib_decompress, 1, src_buf, TEST_BUFSZ));
}

/* --- LZMA Tests --- */

MU_TEST(test_lzma_text_l1)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lzma_init, lzma_deinit,
	        lzma_compress, lzma_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lzma_text_l6)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lzma_init, lzma_deinit,
	        lzma_compress, lzma_decompress, 6, src_buf, TEST_BUFSZ));
}

MU_TEST(test_lzma_random)
{
	fill_random_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(lzma_init, lzma_deinit,
	        lzma_compress, lzma_decompress, 1, src_buf, TEST_BUFSZ));
}

/* --- Bzip2 Tests --- */

MU_TEST(test_bzip2_text_l1)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(bzip2_init, noop_deinit,
	        bzip2_compress, bzip2_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_bzip2_random)
{
	fill_random_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(bzip2_init, noop_deinit,
	        bzip2_compress, bzip2_decompress, 1, src_buf, TEST_BUFSZ));
}

/* --- PPMD Tests --- */

MU_TEST(test_ppmd_text_l3)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(ppmd_init, ppmd_deinit,
	        ppmd_compress, ppmd_decompress, 3, src_buf, TEST_BUFSZ));
}

/* --- Zstandard Tests --- */

#ifdef ENABLE_PC_ZSTD
MU_TEST(test_zstd_text_l1)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zstd_text_l6)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 6, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zstd_text_l14)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 14, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zstd_random)
{
	fill_random_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zstd_zeros)
{
	memset(src_buf, 0, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 1, src_buf, TEST_BUFSZ));
}

MU_TEST(test_zstd_small)
{
	fill_text_pattern(src_buf, MIN_CHUNK);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(zstd_init, zstd_deinit,
	        zstd_compress, zstd_decompress, 1, src_buf, MIN_CHUNK));
}
#endif

/* --- None (passthrough) Tests --- */

MU_TEST(test_none_roundtrip)
{
	fill_text_pattern(src_buf, TEST_BUFSZ);
	mu_assert_int_eq(0,
	    test_algo_roundtrip(none_init, none_deinit,
	        none_compress, none_decompress, 1, src_buf, TEST_BUFSZ));
}

/* --- Test Suites --- */

MU_TEST_SUITE(suite_lz4)
{
	MU_RUN_TEST(test_lz4_text_l1);
	MU_RUN_TEST(test_lz4_text_l9);
	MU_RUN_TEST(test_lz4_random);
	MU_RUN_TEST(test_lz4_zeros);
	MU_RUN_TEST(test_lz4_small);
}

MU_TEST_SUITE(suite_zlib)
{
	MU_RUN_TEST(test_zlib_text_l1);
	MU_RUN_TEST(test_zlib_text_l9);
	MU_RUN_TEST(test_zlib_random);
}

MU_TEST_SUITE(suite_lzma)
{
	MU_RUN_TEST(test_lzma_text_l1);
	MU_RUN_TEST(test_lzma_text_l6);
	MU_RUN_TEST(test_lzma_random);
}

MU_TEST_SUITE(suite_bzip2)
{
	MU_RUN_TEST(test_bzip2_text_l1);
	MU_RUN_TEST(test_bzip2_random);
}

MU_TEST_SUITE(suite_ppmd)
{
	MU_RUN_TEST(test_ppmd_text_l3);
}

#ifdef ENABLE_PC_ZSTD
MU_TEST_SUITE(suite_zstd)
{
	MU_RUN_TEST(test_zstd_text_l1);
	MU_RUN_TEST(test_zstd_text_l6);
	MU_RUN_TEST(test_zstd_text_l14);
	MU_RUN_TEST(test_zstd_random);
	MU_RUN_TEST(test_zstd_zeros);
	MU_RUN_TEST(test_zstd_small);
}
#endif

MU_TEST_SUITE(suite_none)
{
	MU_RUN_TEST(test_none_roundtrip);
}

int
main(void)
{
	slab_init();

	MU_RUN_SUITE(suite_lz4);
	MU_RUN_SUITE(suite_zlib);
	MU_RUN_SUITE(suite_lzma);
	MU_RUN_SUITE(suite_bzip2);
	MU_RUN_SUITE(suite_ppmd);
#ifdef ENABLE_PC_ZSTD
	MU_RUN_SUITE(suite_zstd);
#endif
	MU_RUN_SUITE(suite_none);

	MU_REPORT();
	slab_cleanup(1);
	return MU_EXIT_CODE;
}
