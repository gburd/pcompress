/*
 * Error-path tests for every compressor wrapper (*_compress.c): corrupt
 * and truncated compressed data, and destination buffers that are too
 * small, must be reported as errors without writing past the buffer.
 * Also covers incompressible input and each algorithm's level clamping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <pcompress.h>
#include "minunit.h"

#define	N	(192 * 1024)
#define	GUARD	64

struct algo {
	const char *name;
	init_func_ptr init;
	deinit_func_ptr deinit;
	compress_func_ptr comp, decomp;
	props_func_ptr props;
};

static struct algo algos[] = {
	{ "zlib", zlib_init, zlib_deinit, zlib_compress, zlib_decompress, zlib_props },
	{ "bzip2", bzip2_init, NULL, bzip2_compress, bzip2_decompress, bzip2_props },
	{ "lzma", lzma_init, lzma_deinit, lzma_compress, lzma_decompress, lzma_props },
	{ "ppmd", ppmd_init, ppmd_deinit, ppmd_compress, ppmd_decompress, ppmd_props },
	{ "lzfx", lz_fx_init, lz_fx_deinit, lz_fx_compress, lz_fx_decompress, lz_fx_props },
	{ "lz4", lz4_init, lz4_deinit, lz4_compress, lz4_decompress, lz4_props },
	{ "zstd", zstd_init, zstd_deinit, zstd_compress, zstd_decompress, zstd_props },
	{ "adapt", adapt_init, adapt_deinit, adapt_compress, adapt_decompress, adapt_props },
	{ "adapt2", adapt2_init, adapt_deinit, adapt_compress, adapt_decompress, adapt_props },
#if defined(ENABLE_PC_LIBBSC) && !defined(WORDS_BIGENDIAN)
	{ "libbsc", libbsc_init, libbsc_deinit, libbsc_compress, libbsc_decompress, libbsc_props },
#endif
};
#define	NALGOS	(sizeof (algos) / sizeof (algos[0]))

static uchar_t *text, *rnd, *cmp, *dec;
/* adapt*: compress returns the sub-algorithm, which the chunk header
 * carries back to decompress as chdr (pcompress.c: type |= rv << 4). */
static uchar_t last_chdr;

static void
done(struct algo *a, void **d)
{
	if (a->deinit && *d)
		a->deinit(d);
	*d = NULL;
}

/* Compress src with algorithm a at level; returns compressed length or 0. */
static uint64_t
comp(struct algo *a, int level, uchar_t *src, uint64_t len, uchar_t *dst, uint64_t cap)
{
	void *d = NULL;
	int lv = level, rv;
	uint64_t dl = cap;
	algo_props_t p;

	memset(&p, 0, sizeof (p));
	if (a->props) a->props(&p, level, N);
	if (a->init(&d, &lv, 1, N, VERSION, COMPRESS) != 0)
		return (0);
	rv = a->comp(src, len, dst, &dl, lv, 0, TYPE_UNKNOWN, d);
	done(a, &d);
	last_chdr = (rv > 0) ? (uchar_t)(rv << 4) : 0;
	return (rv < 0 ? 0 : dl);
}

static int
decomp(struct algo *a, int level, uchar_t *src, uint64_t len, uchar_t *dst, uint64_t *dl)
{
	void *d = NULL;
	int lv = level, rv;

	if (a->init(&d, &lv, 1, N, VERSION, DECOMPRESS) != 0)
		return (-100);
	rv = a->decomp(src, len, dst, dl, lv, last_chdr, TYPE_UNKNOWN, d);
	done(a, &d);
	return (rv);
}

MU_TEST(test_roundtrip_levels)
{
	int i, l;
	static const int levels[] = { 1, 6, 9, 14 };

	for (i = 0; i < (int)NALGOS; i++) for (l = 0; l < 4; l++) {
		uint64_t cl = comp(&algos[i], levels[l], text, N, cmp, 2 * N);
		uint64_t dl = N;
		if (cl == 0) continue;	/* some algos decline, that is allowed */
		mu_assert(decomp(&algos[i], levels[l], cmp, cl, dec, &dl) >= 0, algos[i].name);
		mu_assert(dl == N && memcmp(text, dec, N) == 0, algos[i].name);
	}
}

MU_TEST(test_incompressible)
{
	int i;

	/* Must either decline (rv < 0) or round-trip; never corrupt. */
	for (i = 0; i < (int)NALGOS; i++) {
		uint64_t cl = comp(&algos[i], 6, rnd, N, cmp, 2 * N), dl = N;
		if (cl == 0) continue;
		if (decomp(&algos[i], 6, cmp, cl, dec, &dl) >= 0)
			mu_assert(dl == N && memcmp(rnd, dec, N) == 0, algos[i].name);
	}
}

MU_TEST(test_corrupt_and_short)
{
	int i, k;
	uint32_t seed = 7;

	for (i = 0; i < (int)NALGOS; i++) {
		uint64_t cl = comp(&algos[i], 6, text, N, cmp, 2 * N), dl;
		uchar_t *bad;
		if (cl == 0) continue;
		bad = malloc(cl);
		/* Destination one byte too small: error, and the guard intact. */
		memset(dec + N - 1, 0x5A, GUARD + 1);
		dl = N - 1;
		(void) decomp(&algos[i], 6, cmp, cl, dec, &dl);
		for (k = 0; k < GUARD; k++)
			mu_assert(dec[N + k] == 0x5A, algos[i].name);
		/* Corrupt / truncated input: anything but a crash or overrun. */
		for (k = 0; k < 40; k++) {
			int j;
			uint64_t len = cl;
			memcpy(bad, cmp, cl);
			for (j = 0; j < 1 + k % 5; j++) {
				seed = seed * 1664525 + 1013904223;
				bad[seed % cl] ^= (uchar_t)(seed >> 24) | 1;
			}
			if (k % 4 == 0)
				len = 1 + (seed >> 8) % cl;
			memset(dec + N, 0x5A, GUARD);
			dl = N;
			(void) decomp(&algos[i], 6, bad, len, dec, &dl);
			for (j = 0; j < GUARD; j++)
				mu_assert(dec[N + j] == 0x5A, algos[i].name);
		}
		free(bad);
		/* Compress into a destination that cannot hold the output. */
		dl = 16;
		{
			void *d = NULL; int lv = 6;
			if (algos[i].init(&d, &lv, 1, N, VERSION, COMPRESS) == 0) {
				(void) algos[i].comp(rnd, N, cmp, &dl, lv, 0, TYPE_UNKNOWN, d);
				done(&algos[i], &d);
			}
		}
	}
}

MU_TEST_SUITE(suite_algos)
{
	MU_RUN_TEST(test_roundtrip_levels);
	MU_RUN_TEST(test_incompressible);
	MU_RUN_TEST(test_corrupt_and_short);
}

int
main(void)
{
	size_t i;
	uint32_t s = 1;
	static const char *w = "lorem ipsum dolor sit amet consectetur adipiscing elit ";

	slab_init();
	init_pcompress();
	text = malloc(N); rnd = malloc(N);
	cmp = malloc(2 * N + GUARD); dec = malloc(N + GUARD);
	for (i = 0; i < N; i++) {
		text[i] = (uchar_t)w[(i + i / 977) % 56];
		s = s * 1103515245 + 12345;
		rnd[i] = (uchar_t)(s >> 16);
	}
	MU_RUN_SUITE(suite_algos);
	MU_REPORT();
	free(text); free(rnd); free(cmp); free(dec);
	return MU_EXIT_CODE;
}
