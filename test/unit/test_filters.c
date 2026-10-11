/*
 * Unit tests for the LZP and DICT pre-filters: round trips, and that
 * corrupt or truncated encoded data is rejected without writing past
 * the output buffer (run under ASan to catch overruns).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include "lzp.h"
#include "DictFilter.h"
#include "minunit.h"

#define N	(256 * 1024)
#define PAD	64

static unsigned char *in, *enc, *dec;
static int64_t enclen;

static void
fill_text(unsigned char *b, size_t n)
{
	static const char *w[] = { "the ", "quick ", "brown ", "fox ", "jumps ",
	    "over ", "lazy ", "dog ", "and ", "then ", "again\n" };
	size_t i = 0, k = 7;

	while (i < n) {
		const char *s = w[(k = k * 1103515245 + 12345) % 11];
		while (*s && i < n) b[i++] = (unsigned char)*s++;
	}
}

MU_TEST(test_lzp_roundtrip)
{
	int64_t r;

	enclen = lzp_compress(in, enc, N, 16, 32, 0);
	mu_assert(enclen > 0 && enclen < N, "lzp_compress should compress text");
	r = lzp_decompress(enc, dec, enclen, N, 16, 32, 0);
	mu_assert_int_eq(N, (int)r);
	mu_assert_mem_eq(in, dec, N);
}

MU_TEST(test_lzp_rejects_small_output)
{
	/* Output buffer one byte short: must fail, not overrun. */
	int64_t r = lzp_decompress(enc, dec, enclen, N - 1, 16, 32, 0);
	mu_assert(r < 0, "decode into a short buffer must fail");
}

MU_TEST(test_lzp_corrupt_never_overruns)
{
	unsigned char *bad = malloc(enclen);
	uint32_t seed = 1;
	int i, j;

	/* dec has PAD guard bytes after N; decoding must never touch them. */
	for (i = 0; i < 300; i++) {
		int64_t len = enclen;
		memcpy(bad, enc, enclen);
		for (j = 0; j < 1 + i % 8; j++) {
			seed = seed * 1664525 + 1013904223;
			bad[seed % enclen] ^= (unsigned char)(seed >> 24) | 1;
		}
		if (i % 3 == 0)	/* also truncate */
			len = 1 + (seed >> 8) % enclen;
		memset(dec + N, 0xA5, PAD);
		(void) lzp_decompress(bad, dec, len, N, 16, 32, 0);
		for (j = 0; j < PAD; j++)
			mu_assert(dec[N + j] == 0xA5, "lzp_decompress wrote past outLen");
	}
	/* Degenerate inputs. */
	mu_assert(lzp_decompress(enc, dec, 0, N, 16, 32, 0) < 0, "empty input");
	bad[0] = 200;	/* claims 200 blocks but the table is truncated */
	mu_assert(lzp_decompress(bad, dec, 9, N, 16, 32, 0) < 0, "short block table");
	free(bad);
}

MU_TEST(test_dict_roundtrip_and_corrupt)
{
	uint64_t dl = N + PAD, ol;
	int r;

	r = dict_encode(in, N, enc, &dl, 0);
	mu_assert(r != -1, "dict_encode should accept text");
	ol = N;
	mu_assert(dict_decode(enc, dl, dec, &ol) != -1, "dict_decode");
	mu_assert(ol == N, "dict length");
	mu_assert_mem_eq(in, dec, N);
	/* Length header claims more than the destination holds. */
	ol = N / 2;
	mu_assert(dict_decode(enc, dl, dec, &ol) == -1, "dict_decode short dst must fail");
}

MU_TEST_SUITE(suite_filters)
{
	MU_RUN_TEST(test_lzp_roundtrip);
	MU_RUN_TEST(test_lzp_rejects_small_output);
	MU_RUN_TEST(test_lzp_corrupt_never_overruns);
	MU_RUN_TEST(test_dict_roundtrip_and_corrupt);
}

int
main(void)
{
	slab_init();
	init_pcompress();	/* hash dispatch (XXH32) used by DictFilter */
	in = malloc(N);
	enc = malloc(2 * N + PAD);
	dec = malloc(N + PAD);
	fill_text(in, N);
	MU_RUN_SUITE(suite_filters);
	MU_REPORT();
	free(in); free(enc); free(dec);
	return MU_EXIT_CODE;
}
