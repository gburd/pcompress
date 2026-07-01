/*
 * Fuzzing harness for the compression path.
 *
 * Tests that compressing arbitrary data does not crash, trigger undefined
 * behavior, or produce memory errors. Also verifies that compress->decompress
 * round-trips correctly when compression succeeds.
 *
 * Build with libFuzzer:
 *   clang -fsanitize=fuzzer,address -o fuzz_compress fuzz_compress.c -lpcompress ...
 *
 * Build for AFL++:
 *   afl-gcc -DFUZZ_AFL -o fuzz_compress fuzz_compress.c -lpcompress ...
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <pcompress.h>

#define MAX_INPUT_SZ	(256 * 1024)	/* 256 KiB input cap */
#define MAX_OUTPUT_SZ	(512 * 1024)	/* 512 KiB output cap */
#define CHUNK_SZ	(256 * 1024)

struct fuzz_algo {
	const char *name;
	init_func_ptr init;
	deinit_func_ptr deinit;
	compress_func_ptr compress;
	compress_func_ptr decompress;
};

static int
noop_deinit(void **data)
{
	*data = NULL;
	return (0);
}

static struct fuzz_algo fuzz_algos[] = {
	{ "lz4",   lz4_init,   lz4_deinit,   lz4_compress,   lz4_decompress },
	{ "zlib",  zlib_init,  zlib_deinit,  zlib_compress,  zlib_decompress },
	{ "lzma",  lzma_init,  lzma_deinit,  lzma_compress,  lzma_decompress },
	{ "bzip2", bzip2_init, noop_deinit,  bzip2_compress, bzip2_decompress },
#ifdef ENABLE_PC_ZSTD
	{ "zstd",  zstd_init,  zstd_deinit,  zstd_compress,  zstd_decompress },
#endif
	{ NULL, NULL, NULL, NULL, NULL }
};

static int initialized = 0;

static void
fuzz_init(void)
{
	if (!initialized) {
		slab_init();
		initialized = 1;
	}
}

static int
fuzz_one(const uint8_t *data, size_t size)
{
	struct fuzz_algo *a;
	unsigned char *cmp_buf, *dec_buf;
	void *algo_data;
	int lvl;

	fuzz_init();

	if (size < 1 || size > MAX_INPUT_SZ)
		return 0;

	cmp_buf = (unsigned char *)malloc(MAX_OUTPUT_SZ);
	dec_buf = (unsigned char *)malloc(MAX_INPUT_SZ);
	if (!cmp_buf || !dec_buf) {
		free(cmp_buf);
		free(dec_buf);
		return 0;
	}

	for (a = fuzz_algos; a->name != NULL; a++) {
		lvl = 1;
		algo_data = NULL;

		if (a->init(&algo_data, &lvl, 1, CHUNK_SZ,
		    VERSION, COMPRESS) != 0)
			continue;

		uint64_t cmplen = MAX_OUTPUT_SZ;
		int rv = a->compress((void *)data, (uint64_t)size,
		    cmp_buf, &cmplen, lvl, 0, DATA_BINARY, algo_data);
		a->deinit(&algo_data);

		if (rv != 0)
			continue;

		/* Verify round-trip if compression succeeded */
		lvl = 1;
		if (a->init(&algo_data, &lvl, 1, CHUNK_SZ,
		    VERSION, DECOMPRESS) != 0)
			continue;

		uint64_t declen = (uint64_t)size;
		rv = a->decompress(cmp_buf, cmplen, dec_buf, &declen,
		    lvl, 0, DATA_BINARY, algo_data);
		a->deinit(&algo_data);

		if (rv == 0 && declen == (uint64_t)size) {
			if (memcmp(data, dec_buf, size) != 0) {
				fprintf(stderr,
				    "ROUND-TRIP MISMATCH: %s (size=%zu)\n",
				    a->name, size);
				abort();
			}
		}
	}

	free(cmp_buf);
	free(dec_buf);
	return 0;
}

#ifdef FUZZ_AFL
/* AFL++ mode: read from stdin */
int
main(void)
{
	uint8_t buf[MAX_INPUT_SZ];
	size_t n;

	n = fread(buf, 1, sizeof(buf), stdin);
	if (n > 0)
		fuzz_one(buf, n);
	return 0;
}
#else
/* libFuzzer entry point */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	return fuzz_one(data, size);
}
#endif
