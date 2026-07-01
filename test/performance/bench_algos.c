/*
 * Performance benchmarking for pcompress compression algorithms.
 *
 * Measures throughput (MB/s) and compression ratio for each algorithm
 * across data types and compression levels. Results are printed in a
 * tabular format suitable for comparison.
 *
 * Usage: bench_algos [iterations]
 */

#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <pcompress.h>

#define BENCH_BUFSZ	(1024 * 1024)	/* 1 MiB benchmark buffer */
#define CHUNK_SZ	(1024 * 1024)

static unsigned char src_buf[BENCH_BUFSZ];
static unsigned char cmp_buf[BENCH_BUFSZ * 2];
static unsigned char dec_buf[BENCH_BUFSZ];

static double
now_sec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void
fill_text(unsigned char *buf, size_t len)
{
	const char *p = "The quick brown fox jumps over the lazy dog. ";
	size_t plen = strlen(p), pos = 0;
	while (pos < len) {
		size_t n = plen < (len - pos) ? plen : (len - pos);
		memcpy(buf + pos, p, n);
		pos += n;
	}
}

static void
fill_random(unsigned char *buf, size_t len)
{
	unsigned int s = 98765;
	size_t i;
	for (i = 0; i < len; i++) {
		s = s * 1103515245 + 12345;
		buf[i] = (unsigned char)(s >> 16);
	}
}

struct algo_entry {
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

static struct algo_entry algos[] = {
	{ "lz4",   lz4_init,   lz4_deinit,   lz4_compress,   lz4_decompress },
	{ "zlib",  zlib_init,  zlib_deinit,  zlib_compress,  zlib_decompress },
	{ "bzip2", bzip2_init, noop_deinit,  bzip2_compress, bzip2_decompress },
	{ "lzma",  lzma_init,  lzma_deinit,  lzma_compress,  lzma_decompress },
	{ "ppmd",  ppmd_init,  ppmd_deinit,  ppmd_compress,  ppmd_decompress },
#ifdef ENABLE_PC_ZSTD
	{ "zstd",  zstd_init,  zstd_deinit,  zstd_compress,  zstd_decompress },
#endif
	{ NULL, NULL, NULL, NULL, NULL }
};

static void
bench_algo(struct algo_entry *a, int level, unsigned char *data,
    size_t datalen, int iterations, const char *data_label)
{
	void *algo_data = NULL;
	int lvl = level;
	uint64_t cmplen, declen;
	double t0, t1, cmp_time, dec_time;
	double cmp_mbps, dec_mbps, ratio;
	int i, rv;

	/* Compress phase */
	rv = a->init(&algo_data, &lvl, 1, CHUNK_SZ, VERSION, COMPRESS);
	if (rv != 0) {
		printf("%-6s  %-8s  L%-2d  (init failed)\n",
		    a->name, data_label, level);
		return;
	}

	/* Warmup */
	cmplen = (uint64_t)(datalen * 2);
	a->compress(data, (uint64_t)datalen, cmp_buf, &cmplen,
	    lvl, 0, DATA_BINARY, algo_data);

	t0 = now_sec();
	for (i = 0; i < iterations; i++) {
		cmplen = (uint64_t)(datalen * 2);
		rv = a->compress(data, (uint64_t)datalen, cmp_buf, &cmplen,
		    lvl, 0, DATA_BINARY, algo_data);
		if (rv != 0) break;
	}
	t1 = now_sec();
	cmp_time = t1 - t0;
	a->deinit(&algo_data);

	if (rv != 0) {
		printf("%-6s  %-8s  L%-2d  (compress failed)\n",
		    a->name, data_label, level);
		return;
	}

	/* Decompress phase */
	lvl = level;
	rv = a->init(&algo_data, &lvl, 1, CHUNK_SZ, VERSION, DECOMPRESS);
	if (rv != 0) {
		printf("%-6s  %-8s  L%-2d  (decompress init failed)\n",
		    a->name, data_label, level);
		return;
	}

	t0 = now_sec();
	for (i = 0; i < iterations; i++) {
		declen = (uint64_t)datalen;
		rv = a->decompress(cmp_buf, cmplen, dec_buf, &declen,
		    lvl, 0, DATA_BINARY, algo_data);
		if (rv != 0) break;
	}
	t1 = now_sec();
	dec_time = t1 - t0;
	a->deinit(&algo_data);

	if (rv != 0) {
		printf("%-6s  %-8s  L%-2d  (decompress failed)\n",
		    a->name, data_label, level);
		return;
	}

	ratio = (double)cmplen / (double)datalen * 100.0;
	cmp_mbps = ((double)datalen * iterations / (1024.0 * 1024.0)) / cmp_time;
	dec_mbps = ((double)datalen * iterations / (1024.0 * 1024.0)) / dec_time;

	printf("%-6s  %-8s  L%-2d  %6.1f%%  %8.1f MB/s  %8.1f MB/s\n",
	    a->name, data_label, level, ratio, cmp_mbps, dec_mbps);
}

int
main(int argc, char *argv[])
{
	int iterations = 10;
	struct algo_entry *a;
	int levels[] = { 1, 6, 14 };
	int nlvl = sizeof(levels) / sizeof(levels[0]);
	int l;

	if (argc > 1)
		iterations = atoi(argv[1]);
	if (iterations < 1)
		iterations = 1;

	slab_init();

	printf("Pcompress Algorithm Benchmark (%d iterations, %d KiB blocks)\n\n",
	    iterations, BENCH_BUFSZ / 1024);
	printf("%-6s  %-8s  %-3s  %7s  %13s  %13s\n",
	    "Algo", "Data", "Lvl", "Ratio", "Compress", "Decompress");
	printf("------  --------  ---  -------  -------------  -------------\n");

	for (a = algos; a->name != NULL; a++) {
		/* Text data */
		fill_text(src_buf, BENCH_BUFSZ);
		for (l = 0; l < nlvl; l++)
			bench_algo(a, levels[l], src_buf, BENCH_BUFSZ,
			    iterations, "text");

		/* Random data */
		fill_random(src_buf, BENCH_BUFSZ);
		for (l = 0; l < nlvl; l++)
			bench_algo(a, levels[l], src_buf, BENCH_BUFSZ,
			    iterations, "random");

		printf("\n");
	}

	slab_cleanup(1);
	return 0;
}
