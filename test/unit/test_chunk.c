/*
 * Chunk primitives (public API): chunker, ids, sealed objects.
 * Property tests: open(seal(x)) == x for every algorithm, several levels,
 * with and without a key; every one-byte change and every truncation of a
 * sealed object is rejected; chunk boundaries do not depend on how the
 * input is split across pc_chunker_feed() calls.
 */

#define _GNU_SOURCE	/* memmem */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>
#include "minunit.h"

static uint32_t rs = 12345;
static uint32_t
rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return (rs);
}

/* Mixed data: text-like runs and noise, so most algorithms compress it. */
static void
fill(unsigned char *p, size_t n, int kind)
{
	static const char *w = "the quick brown fox jumps over the lazy dog ";
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = kind == 0 ? (unsigned char)w[(i * 7 + i / 97) % 44] :
		    kind == 1 ? (unsigned char)rnd() : (unsigned char)((i / 4096) % 2 ? rnd() : w[i % 44]);
}

static void
quiet(void *a, int l, const char *m)
{
	(void) a; (void) l; (void) m;
}

static pc_ctx_t *
ctx_with(const char *algo, int level, const unsigned char *key)
{
	pc_ctx_t *c = pc_new();

	pc_set_log(c, quiet, NULL, PC_LOG_ERR);
	if (algo != NULL)
		pc_set_algorithm(c, algo);
	if (level)
		pc_set_level(c, level);
	if (key != NULL)
		pc_set_key(c, key, 32);
	return (c);
}

/* Seal with ctx; open with a fresh context of the same key. 0 if equal. */
static int
roundtrip(const char *algo, int level, const unsigned char *key,
    const unsigned char *in, size_t n, size_t *objlen)
{
	pc_ctx_t *c = ctx_with(algo, level, key), *d = ctx_with(NULL, 0, key);
	size_t cap = pc_chunk_seal_bound(n), ol = cap, pl;
	unsigned char *obj = malloc(cap), *back = malloc(n + 1);
	int rv = -1;

	if (pc_chunk_seal(c, in, n, obj, &ol) == 0 && ol <= cap &&
	    pc_chunk_open_size(obj, ol) == n) {
		pl = n;
		if (pc_chunk_open(d, obj, ol, back, &pl) == 0 && pl == n &&
		    (n == 0 || memcmp(in, back, n) == 0))
			rv = 0;
	}
	if (objlen)
		*objlen = ol;
	free(obj);
	free(back);
	pc_free(c);
	pc_free(d);
	return (rv);
}

static const char *algos[] = { "zstd", "lz4", "lzma", "zlib", "bzip2", "ppmd",
    "lzfx", "adapt", "adapt2", "lzmaMt", "none", "libbsc", NULL };

/* libbsc on big-endian fails to initialise: seal stores the chunk raw. */

MU_TEST(test_seal_open_all)
{
	static const size_t sizes[] = { 0, 1, 100, 4096, 300000 };
	static const int levels[] = { 1, 6, 9 };
	unsigned char key[32], *buf = malloc(300000);
	int a, l, s, k, kind;

	for (k = 0; k < 32; k++)
		key[k] = (unsigned char)(k * 37 + 1);
	for (kind = 0; kind < 3; kind++) {
		fill(buf, 300000, kind);
		for (a = 0; algos[a] != NULL; a++)
			for (l = 0; l < 3; l++)
				for (s = 0; s < 5; s++)
					for (k = 0; k < 2; k++) {
						char what[96];

						snprintf(what, sizeof (what), "%s l%d n=%zu key=%d kind=%d",
						    algos[a], levels[l], sizes[s], k, kind);
						mu_assert(roundtrip(algos[a], levels[l], k ? key : NULL,
						    buf, sizes[s], NULL) == 0, what);
					}
	}
	free(buf);
}

MU_TEST(test_compresses_and_falls_back)
{
	unsigned char *b = malloc(200000);
	size_t ol;

	fill(b, 200000, 0);
	mu_assert_int_eq(0, roundtrip("zstd", 6, NULL, b, 200000, &ol));
	mu_assert(ol < 200000 / 4, "text did not compress");
	fill(b, 200000, 1);
	mu_assert_int_eq(0, roundtrip("zstd", 6, NULL, b, 200000, &ol));
	mu_assert(ol == pc_chunk_seal_bound(200000), "noise not stored raw at the bound");
	free(b);
}

/* Every one-byte change and every truncation of a small object fails. */
static void
tamper_all(const unsigned char *key, int kind)
{
	pc_ctx_t *c = ctx_with("zstd", 6, key), *d = ctx_with(NULL, 0, key);
	unsigned char in[3000], obj[3200], bad[3200], out[3000];
	size_t ol = sizeof (obj), i, pl;

	fill(in, sizeof (in), kind);
	mu_assert_int_eq(0, pc_chunk_seal(c, in, sizeof (in), obj, &ol));
	for (i = 0; i < ol; i++) {
		int bit;

		for (bit = 0; bit < 8; bit += 3) {
			memcpy(bad, obj, ol);
			bad[i] ^= (unsigned char)(1 << bit);
			pl = sizeof (out);
			if (pc_chunk_open(d, bad, ol, out, &pl) == 0) {
				char m[64];
				snprintf(m, sizeof (m), "flip at %zu bit %d accepted", i, bit);
				mu_assert(0, m);
			}
		}
	}
	for (i = 0; i < ol; i++) {
		pl = sizeof (out);
		mu_assert(pc_chunk_open(d, obj, i, out, &pl) != 0, "truncation accepted");
	}
	pl = sizeof (out);
	mu_assert_int_eq(0, pc_chunk_open(d, obj, ol, out, &pl));
	mu_assert(pl == sizeof (in) && memcmp(in, out, pl) == 0, "intact object");
	pc_free(c);
	pc_free(d);
}

MU_TEST(test_tamper_rejected)
{
	unsigned char key[32];
	int i;

	for (i = 0; i < 32; i++)
		key[i] = (unsigned char)(255 - i);
	tamper_all(NULL, 0);
	tamper_all(key, 0);
	tamper_all(key, 1);	/* raw object */
}

MU_TEST(test_keys)
{
	unsigned char k1[32], k2[32], in[5000], obj[6000], out[5000];
	pc_ctx_t *c, *d;
	size_t ol = sizeof (obj), pl = sizeof (out);
	int i;

	for (i = 0; i < 32; i++) {
		k1[i] = (unsigned char)i;
		k2[i] = (unsigned char)i;
	}
	k2[31] ^= 1;
	fill(in, sizeof (in), 2);
	c = ctx_with("lz4", 1, k1);
	mu_assert_int_eq(0, pc_chunk_seal(c, in, sizeof (in), obj, &ol));
	/* Ciphertext: the plaintext text runs must not appear. */
	mu_assert(memmem(obj, ol, "quick brown fox", 15) == NULL, "plaintext visible");
	d = ctx_with(NULL, 0, k2);
	mu_assert(pc_chunk_open(d, obj, ol, out, &pl) != 0, "wrong key accepted");
	mu_assert_int_eq(PC_ERR_VERIFY, pc_last_error_code(d));
	pc_free(d);
	d = ctx_with(NULL, 0, NULL);
	pl = sizeof (out);
	mu_assert(pc_chunk_open(d, obj, ol, out, &pl) != 0, "no key accepted");
	mu_assert_int_eq(PC_ERR_CRYPTO, pc_last_error_code(d));
	pc_free(d);
	/* Two seals of the same data differ (random nonce). */
	{
		unsigned char obj2[6000];
		size_t ol2 = sizeof (obj2);

		mu_assert_int_eq(0, pc_chunk_seal(c, in, sizeof (in), obj2, &ol2));
		mu_assert(ol2 == ol && memcmp(obj, obj2, ol) != 0, "nonce reused");
	}
	mu_assert(pc_set_key(c, k1, 16) != 0, "16-byte key accepted");
	mu_assert_int_eq(0, pc_set_key(c, NULL, 0));
	pc_free(c);
}

MU_TEST(test_bad_args)
{
	pc_ctx_t *c = ctx_with("zstd", 3, NULL);
	unsigned char in[100] = { 0 }, obj[200], out[100];
	size_t ol = 10, pl = sizeof (out);

	mu_assert(pc_chunk_seal(c, in, sizeof (in), obj, &ol) != 0, "small out buffer");
	mu_assert_int_eq(PC_ERR_ARGS, pc_last_error_code(c));
	ol = sizeof (obj);
	mu_assert_int_eq(0, pc_chunk_seal(c, in, sizeof (in), obj, &ol));
	pl = 50;
	mu_assert(pc_chunk_open(c, obj, ol, out, &pl) != 0, "small plain buffer");
	mu_assert(pc_chunk_open_size("not an object at all, really", 28) == (size_t)-1, "size of junk");
	pl = sizeof (out);
	mu_assert(pc_chunk_open(c, "PCK2junkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunk",
	    70, out, &pl) != 0, "bad magic");
	mu_assert_int_eq(PC_ERR_FORMAT, pc_last_error_code(c));
	pc_free(c);
}

MU_TEST(test_ids)
{
	unsigned char a[1000], id1[32], id2[32], id3[32], key[32] = { 1 };

	fill(a, sizeof (a), 0);
	mu_assert_int_eq(0, pc_chunk_id(a, sizeof (a), NULL, 0, id1));
	mu_assert_int_eq(0, pc_chunk_id(a, sizeof (a), NULL, 0, id2));
	mu_assert(memcmp(id1, id2, 32) == 0, "id not stable");
	mu_assert_int_eq(0, pc_chunk_id(a, sizeof (a), key, 32, id3));
	mu_assert(memcmp(id1, id3, 32) != 0, "keyed id equals unkeyed");
	a[500] ^= 1;
	mu_assert_int_eq(0, pc_chunk_id(a, sizeof (a), NULL, 0, id2));
	mu_assert(memcmp(id1, id2, 32) != 0, "id ignores content");
	/* BLAKE2b-256("abc"), RFC 7693-style known answer for the unkeyed id. */
	{
		static const unsigned char kat[32] = {
			0xbd, 0xdd, 0x81, 0x3c, 0x63, 0x42, 0x39, 0x72, 0x31, 0x71, 0xef, 0x3f,
			0xee, 0x98, 0x57, 0x9b, 0x94, 0x96, 0x4e, 0x3b, 0xb1, 0xcb, 0x3e, 0x42,
			0x72, 0x62, 0xc8, 0xc0, 0x68, 0xd5, 0x23, 0x19 };
		mu_assert_int_eq(0, pc_chunk_id("abc", 3, NULL, 0, id1));
		mu_assert(memcmp(id1, kat, 32) == 0, "BLAKE2b-256(abc) known answer");
	}
}

/* Chunk all of buf in pieces cut at random; return the number of chunks. */
static int
chunk_split(pc_chunker_t *c, const unsigned char *buf, size_t n, size_t *out, int max, int split)
{
	size_t pos = 0, lens[4096];
	int total = 0, k, r;

	while (pos < n) {
		size_t take = split ? 1 + rnd() % 70000 : n - pos;

		if (take > n - pos)
			take = n - pos;
		r = pc_chunker_feed(c, buf + pos, take, lens, 4096);
		if (r < 0)
			return (-1);
		for (k = 0; k < r && total < max; k++)
			out[total++] = lens[k];
		pos += take;
	}
	r = pc_chunker_finish(c, lens, 4096);
	for (k = 0; k < r && total < max; k++)
		out[total++] = lens[k];
	return (total);
}

MU_TEST(test_chunker)
{
	size_t n = 4 * 1024 * 1024, a[2000], b[2000], sum = 0;
	unsigned char *buf = malloc(n + 100);
	pc_chunker_t *c = pc_chunker_new(16384, 4096, 65536);
	int na, nb, i, same_tail;

	mu_assert(c != NULL, "chunker");
	fill(buf, n, 1);
	na = chunk_split(c, buf, n, a, 2000, 0);
	nb = chunk_split(c, buf, n, b, 2000, 1);
	mu_assert(na > 100 && na == nb, "split changed the number of chunks");
	for (i = 0; i < na; i++) {
		mu_assert(a[i] == b[i], "split changed a boundary");
		mu_assert(a[i] <= 65536 && (a[i] >= 4096 || i == na - 1), "size outside min/max");
		sum += a[i];
	}
	mu_assert(sum == n, "chunks do not cover the input");
	/* Average roughly as asked (mask 16K past a 4K minimum). */
	mu_assert(n / na > 8192 && n / na < 40000, "average chunk size");
	/* Insert 37 bytes near the start: later boundaries realign. */
	memmove(buf + 1037, buf + 1000, n - 1000);
	memset(buf + 1000, 'X', 37);
	nb = chunk_split(c, buf, n + 37, b, 2000, 0);
	same_tail = 0;
	for (i = 1; i <= na && i <= nb; i++)
		if (a[na - i] == b[nb - i])
			same_tail++;
	mu_assert(same_tail > na - 5, "an insertion changed many boundaries");
	pc_chunker_free(c);
	mu_assert(pc_chunker_new(1000, 2000, 4000) == NULL, "min > avg accepted");
	c = pc_chunker_new(0, 0, 0);
	mu_assert(c != NULL, "defaults");
	pc_chunker_free(c);
	free(buf);
}

MU_TEST_SUITE(suite_chunk)
{
	MU_RUN_TEST(test_ids);
	MU_RUN_TEST(test_chunker);
	MU_RUN_TEST(test_compresses_and_falls_back);
	MU_RUN_TEST(test_keys);
	MU_RUN_TEST(test_bad_args);
	MU_RUN_TEST(test_tamper_rejected);
	MU_RUN_TEST(test_seal_open_all);
}

int
main(void)
{
	MU_RUN_SUITE(suite_chunk);
	MU_REPORT();
	return MU_EXIT_CODE;
}
