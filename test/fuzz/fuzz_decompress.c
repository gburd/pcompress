/*
 * Fuzzing harness for decompression routines.
 *
 * Compatible with both libFuzzer (LLVMFuzzerTestOneInput) and AFL++
 * (stdin-based). Tests that decompression of arbitrary data does not
 * crash, trigger undefined behavior, or produce memory errors.
 *
 * Build with libFuzzer:
 *   clang -fsanitize=fuzzer,address -o fuzz_decompress fuzz_decompress.c -lpcompress ...
 *
 * Build for AFL++:
 *   afl-gcc -DFUZZ_AFL -o fuzz_decompress fuzz_decompress.c -lpcompress ...
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <pcompress.h>

#define MAX_DECOMPRESS_SZ	(4 * 1024 * 1024)	/* 4 MiB output cap */

struct fuzz_algo {
	const char *name;
	init_func_ptr init;
	deinit_func_ptr deinit;
	compress_func_ptr decompress;
};

static int
noop_deinit(void **data)
{
	*data = NULL;
	return (0);
}

static struct fuzz_algo fuzz_algos[] = {
	{ "lz4",   lz4_init,   lz4_deinit,   lz4_decompress },
	{ "zlib",  zlib_init,  zlib_deinit,  zlib_decompress },
	{ "lzma",  lzma_init,  lzma_deinit,  lzma_decompress },
	{ "bzip2", bzip2_init, noop_deinit,  bzip2_decompress },
#ifdef ENABLE_PC_ZSTD
	{ "zstd",  zstd_init,  zstd_deinit,  zstd_decompress },
#endif
	{ NULL, NULL, NULL, NULL }
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

/*
 * Attempt decompression of fuzz input with each algorithm.
 * The input is treated as compressed data; errors are expected and
 * silently ignored. The goal is to find crashes or memory corruption.
 */
static int
fuzz_one(const uint8_t *data, size_t size)
{
	struct fuzz_algo *a;
	unsigned char *out;
	void *algo_data;
	int lvl;

	fuzz_init();

	if (size < 4 || size > MAX_DECOMPRESS_SZ)
		return 0;

	out = (unsigned char *)malloc(MAX_DECOMPRESS_SZ);
	if (!out)
		return 0;

	for (a = fuzz_algos; a->name != NULL; a++) {
		lvl = 1;
		algo_data = NULL;
		if (a->init(&algo_data, &lvl, 1, MAX_DECOMPRESS_SZ,
		    VERSION, DECOMPRESS) != 0)
			continue;

		uint64_t dstlen = MAX_DECOMPRESS_SZ;
		/* Errors are expected; we only care about crashes */
		a->decompress((void *)data, (uint64_t)size,
		    out, &dstlen, lvl, 0, DATA_BINARY, algo_data);

		a->deinit(&algo_data);
	}

	free(out);
	return 0;
}

#ifdef FUZZ_AFL
/* AFL++ mode: read from stdin */
int
main(void)
{
	uint8_t buf[MAX_DECOMPRESS_SZ];
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
