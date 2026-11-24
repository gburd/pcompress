/*
 * Unit tests for the LZP, DICT, E8E9 and BCJ pre-filters: round trips, and that
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
#include "filters/dispack/dis.hpp"
#include "filters/bcj/bcj.h"
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

/* Put a CALL rel32 whose displacement is disp at b[i]. */
static void
put_call(unsigned char *b, uint32_t i, uint32_t disp)
{
	b[i] = 0xe8;
	b[i + 1] = disp & 0xff;
	b[i + 2] = (disp >> 8) & 0xff;
	b[i + 3] = (disp >> 16) & 0xff;
	b[i + 4] = 0;
}

/*
 * Every buffer Forward_E89() transforms must come back from Inverse_E89().
 * The two cases that did not: a call at byte 0 (the inverse never looks
 * there), and a call whose offset wraps to zero (left unconverted by the
 * forward pass, but rewritten by the inverse).
 */
MU_TEST(test_e8e9_roundtrip)
{
	unsigned char b[4096], orig[4096];
	uint32_t i;

	memset(b, 0x90, sizeof (b));
	for (i = 64; i < 4000; i += 97)
		put_call(b, i, 0x1234 + i);
	put_call(b, 0, 0x4321);
	memcpy(orig, b, sizeof (b));
	mu_assert(Forward_E89(b, sizeof (b)) == 0, "E8E9 should transform calls");
	mu_assert(Inverse_E89(b, sizeof (b)) == 0, "Inverse_E89");
	mu_assert_mem_eq(orig, b, sizeof (b));

	/* Offset 0x800 + 0xfff800 wraps to 0: the buffer is left alone. */
	put_call(b, 0x800, 0xfff800);
	mu_assert(Forward_E89(b, sizeof (b)) == -1, "wrapping offset must refuse");

	/* Lengths the forward pass never produces are refused, not overrun. */
	mu_assert(Inverse_E89(b, 4) == -1, "Inverse_E89 on 4 bytes");
}

/* A small LCG, so the BCJ inputs below are the same on every platform. */
static uint32_t bcj_st;

static uint32_t
bcj_rnd(void)
{
	bcj_st = bcj_st * 1103515245u + 12345u;
	return ((bcj_st >> 8) | (bcj_st << 24));
}

static void
put32(unsigned char *b, size_t *n, uint32_t w)
{
	b[(*n)++] = w & 0xff;
	b[(*n)++] = (w >> 8) & 0xff;
	b[(*n)++] = (w >> 16) & 0xff;
	b[(*n)++] = w >> 24;
}

/* 1024 words, a quarter BL and a quarter ADRP. */
static size_t
gen_arm64(unsigned char *b)
{
	size_t n = 0;
	int i;

	bcj_st = 5;
	for (i = 0; i < 1024; i++) {
		uint32_t w = bcj_rnd(), k = bcj_rnd() % 4;

		if (k == 0)
			w = 0x94000000u | (w & 0x03ffffffu);
		else if (k == 1)
			w = 0x90000000u | (w & 0x60ffffffu);
		put32(b, &n, w);
	}
	return (n);
}

/* JAL ra, AUIPC+JALR pairs, 16-bit and random words, mixed. */
static size_t
gen_riscv(unsigned char *b)
{
	static const uint32_t regs[4] = { 1, 5, 6, 10 };
	size_t n = 0;
	int i;

	bcj_st = 7;
	for (i = 0; i < 1024; i++) {
		uint32_t w = bcj_rnd(), k = bcj_rnd() % 5, rd = regs[bcj_rnd() % 4];

		if (k == 0) {
			w = 0xefu | (1u << 7) | (w & 0xfffff000u);
		} else if (k == 1) {
			put32(b, &n, 0x17u | (rd << 7) | (w & 0xfffff000u));
			w = 0x67u | (rd << 15) | (1u << 7) | (bcj_rnd() & 0xfff00000u);
		} else if (k == 2) {
			b[n++] = w & 0xff;
			b[n++] = (w >> 8) & 0xff;
			continue;
		}
		put32(b, &n, w);
	}
	return (n);
}

static uint64_t
fnv1a(const unsigned char *b, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	while (n--)
		h = (h ^ *b++) * 0x100000001b3ULL;
	return (h);
}

/*
 * The BCJ coders must match XZ Utils byte for byte, forever: archives
 * hold their output. The expected hashes are of xz 5.8.1's own output
 * for these inputs (xz --format=raw --arm64 or --riscv, then the raw
 * LZMA2 layer decoded alone); the ARM64 one also matches xz 5.4.5.
 */
MU_TEST(test_bcj_known_answer)
{
	static unsigned char b[8192], orig[8192];
	size_t n;

	n = gen_arm64(b);
	mu_assert(n == 4096 && fnv1a(b, n) == 0xed01bcedea48bfb5ULL, "ARM64 input");
	memcpy(orig, b, n);
	bcj_arm64_encode(b, n);
	mu_assert(fnv1a(b, n) == 0xe9e6fa6cb0f20f18ULL, "ARM64 matches xz 5.8.1");
	bcj_arm64_decode(b, n);
	mu_assert_mem_eq(orig, b, n);

	n = gen_riscv(b);
	mu_assert(n == 4562 && fnv1a(b, n) == 0x4b6c8fe3201454beULL, "RISC-V input");
	memcpy(orig, b, n);
	bcj_riscv_encode(b, n);
	mu_assert(fnv1a(b, n) == 0x2777b85d5404d727ULL, "RISC-V matches xz 5.8.1");
	bcj_riscv_decode(b, n);
	mu_assert_mem_eq(orig, b, n);
}

/*
 * Any bytes decode without touching memory outside the buffer: odd
 * lengths, tails shorter than an instruction, and empty input (run
 * under ASan; the buffers are exact-size heap copies).
 */
MU_TEST(test_bcj_any_length)
{
	static unsigned char src[64];
	size_t len, i;

	for (i = 0; i < sizeof (src); i++)
		src[i] = (i % 3 == 0) ? 0x17 : (unsigned char)(0x94 ^ i);
	for (len = 0; len <= sizeof (src); len++) {
		unsigned char *b = malloc(len > 0 ? len : 1);

		mu_assert(b != NULL, "malloc");
		memcpy(b, src, len);
		bcj_arm64_decode(b, len);
		bcj_riscv_decode(b, len);
		bcj_arm64_encode(b, len);
		bcj_riscv_encode(b, len);
		free(b);
	}
}

MU_TEST_SUITE(suite_filters)
{
	MU_RUN_TEST(test_lzp_roundtrip);
	MU_RUN_TEST(test_lzp_rejects_small_output);
	MU_RUN_TEST(test_lzp_corrupt_never_overruns);
	MU_RUN_TEST(test_dict_roundtrip_and_corrupt);
	MU_RUN_TEST(test_e8e9_roundtrip);
	MU_RUN_TEST(test_bcj_known_answer);
	MU_RUN_TEST(test_bcj_any_length);
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
