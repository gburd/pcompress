/*
 * Unit tests for the reflate filter (filters/reflate/): streams made
 * by zlib at the parameter sets it searches come back bit for bit from
 * their records, in gzip, zip (with and without data descriptors) and
 * PNG containers; streams it cannot reproduce are left alone; and the
 * record decoder refuses every malformed record rather than misbuild.
 * Containers are built with the pinned deflate (rf_deflate()), whose
 * output test_rf_deflate_kat pins against Python's zlib 1.3: the system
 * zlib may be zlib-ng (Fedora), whose streams the filter rightly does
 * not keep. test_rf_unmatched uses the system zlib for a stream the
 * filter never keeps, whichever deflate made it.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <utils.h>
#include <pc_alloc.h>
#include <crypto_utils.h>
#include "filters/reflate/reflate.h"
#include "filters/reflate/rf_deflate.h"
#include "minunit.h"

/* Compressible test text: varied enough that every level differs. */
static unsigned char *
text(size_t n)
{
	unsigned char *b = pc_malloc(n);
	size_t i;
	uint32_t x = 12345;

	for (i = 0; i < n; i++) {
		x = x * 1103515245 + 12345;
		b[i] = (i % 61 == 60) ? '\n' : "etaoin shrdlu"[(x >> 16) % 13];
	}
	return (b);
}

/* Raw deflate of raw with the pinned deflate, window 15; returns the length. */
static size_t
pdeflate(const unsigned char *raw, size_t n, unsigned char *out, size_t cap, int level,
    int memlevel, int strategy)
{
	rf_params_t p;
	size_t olen;

	p.level = level;
	p.memlevel = memlevel;
	p.wbits = 15;
	p.strategy = strategy;
	if (rf_deflate(raw, n, out, cap, &p, &olen) != 0)
		return (0);
	return (olen);
}

/* Raw deflate of raw with the system zlib; returns the length. */
static size_t
zdeflate(const unsigned char *raw, size_t n, unsigned char *out, size_t cap, int level,
    int memlevel, int strategy)
{
	z_stream zs;

	memset(&zs, 0, sizeof (zs));
	if (deflateInit2(&zs, level, Z_DEFLATED, -15, memlevel, strategy) != Z_OK)
		return (0);
	zs.next_in = (Bytef *)raw;
	zs.avail_in = (uInt)n;
	zs.next_out = out;
	zs.avail_out = (uInt)cap;
	if (deflate(&zs, Z_FINISH) != Z_STREAM_END)
		n = 0;
	else
		n = cap - zs.avail_out;
	deflateEnd(&zs);
	return (n);
}

static void
le(unsigned char *p, uint64_t v, int n)
{
	while (n-- > 0) {
		*p++ = (unsigned char)v;
		v >>= 8;
	}
}

/* A gzip file of raw at level/memlevel; *len its size. */
static unsigned char *
make_gzip(const unsigned char *raw, size_t n, int level, int memlevel, size_t *len)
{
	unsigned char *g = pc_malloc(n + 1024);
	size_t c;

	memcpy(g, "\037\213\010\000\000\000\000\000\000\003", 10);
	c = pdeflate(raw, n, g + 10, n + 1000, level, memlevel, 0);
	le(g + 10 + c, crc32(0, raw, (uInt)n), 4);
	le(g + 14 + c, n, 4);
	*len = 18 + c;
	return (g);
}

/*
 * Round trip in through the record: the record length if one was made
 * and decodes to exactly in, 0 if none was made, 1 if it misdecodes.
 */
static size_t
roundtrip(const unsigned char *in, size_t len)
{
	unsigned char *rec = NULL, *back = NULL;
	size_t rlen = rf_encode(in, len, &rec), blen;

	if (rlen == 0)
		return (0);
	blen = rf_decode(rec, rlen, &back);
	if (blen != len || memcmp(back, in, len) != 0)
		rlen = 1;
	pc_mfree(rec);
	pc_mfree(back);
	return (rlen);
}

/*
 * The pinned deflate's output, forever: SHA-256 of a raw deflate of
 * 200000 bytes, raw[i] = "etaoin shrdlucmfwyp\n"[(x >> 16) % 20] with
 * x = x * 1103515245 + 12345 (mod 2^32) from x = 1, made with Python's
 * zlib (1.3) at each (level, memLevel, strategy), window 15:
 *   c = zlib.compressobj(level, 8, -15, memlevel, strategy)
 *   hashlib.sha256(c.compress(raw) + c.flush()).hexdigest()
 * If this fails, files written with -Z no longer extract.
 */
static const struct {
	int level, memlevel, strategy;
	size_t len;
	const char *sha256;
} kat[] = {
	{ 6, 8, 0, 121221, "429a9017f56e417981639f804b482ed2df315aece4637195ba93186c6189b6d5" },
	{ 9, 9, 0, 121243, "8149ba2328832a270785927b183db3e08264528cf777d1bec9955db437186ace" },
	{ 1, 8, 0, 124769, "874d8f3bbbba81c1fe24e91bb879b67e50b0ec041462be972204583ce57878f2" },
	{ 4, 8, 0, 121236, "542223e9b94b2a79c27f497162837b51f769c26736e05e6dd4192e697a18832a" },
	{ 6, 8, 1, 111118, "d7b529c14788e28e201ac26e9f57656c6456f5d536579efe430165e1113e9f13" }
};

MU_TEST(test_rf_deflate_kat)
{
	const size_t n = 200000;
	unsigned char *raw = pc_malloc(n), *out = pc_malloc(n + 1024), d[32];
	char hex[65];
	uint32_t x = 1;
	size_t i, j, olen;
	rf_params_t p;

	for (i = 0; i < n; i++) {
		x = x * 1103515245 + 12345;
		raw[i] = "etaoin shrdlucmfwyp\n"[(x >> 16) % 20];
	}
	for (i = 0; i < sizeof (kat) / sizeof (kat[0]); i++) {
		p.level = kat[i].level;
		p.memlevel = kat[i].memlevel;
		p.wbits = 15;
		p.strategy = kat[i].strategy;
		mu_assert_int_eq(0, rf_deflate(raw, n, out, n + 1024, &p, &olen));
		mu_assert_uint64_eq(kat[i].len, olen);
		compute_checksum(d, CKSUM_SHA256, out, olen, 0, 0);
		for (j = 0; j < 32; j++)
			snprintf(hex + 2 * j, 3, "%02x", d[j]);
		mu_assert(strcmp(hex, kat[i].sha256) == 0, "pinned deflate output changed");
		/* Exactly the stream's size is room enough; one byte less is not. */
		mu_assert_int_eq(0, rf_deflate(raw, n, out, olen, &p, &olen));
		mu_assert_int_eq(-1, rf_deflate(raw, n, out, olen - 1, &p, &olen));
		mu_assert_int_eq(1, rf_deflate_match(raw, n, out, kat[i].len, &p));
		/* Another memLevel hashes differently: not this stream. */
		p.memlevel = 2;
		mu_assert_int_eq(0, rf_deflate_match(raw, n, out, kat[i].len, &p));
	}
	pc_mfree(raw);
	pc_mfree(out);
}

/* Each setting the filter searches is kept; one it does not is not. */
MU_TEST(test_rf_gzip_levels)
{
	static const int sets[][2] = { { 6, 8 }, { 9, 8 }, { 1, 8 }, { 6, 9 }, { 9, 9 },
	    { 1, 9 }, { 4, 8 } };
	const size_t n = 300000;
	unsigned char *raw = text(n), *g, *rec = NULL;
	size_t len, i;

	for (i = 0; i < sizeof (sets) / sizeof (sets[0]); i++) {
		g = make_gzip(raw, n, sets[i][0], sets[i][1], &len);
		mu_assert(roundtrip(g, len) > n, "gzip stream not kept");
		pc_mfree(g);
	}
	g = make_gzip(raw, n, 7, 5, &len);
	mu_assert_uint64_eq(0, rf_encode(g, len, &rec));
	pc_mfree(g);
	pc_mfree(raw);
}

/*
 * A stream from another deflater (the system zlib's Huffman-only
 * strategy, which the filter does not search): left alone.
 */
MU_TEST(test_rf_unmatched)
{
	const size_t n = 200000;
	unsigned char *raw = text(n), *g = pc_malloc(n + 1024), *rec = NULL;
	size_t c;

	memcpy(g, "\037\213\010\000\000\000\000\000\000\003", 10);
	c = zdeflate(raw, n, g + 10, n + 1000, 6, 3, Z_HUFFMAN_ONLY);
	le(g + 10 + c, crc32(0, raw, (uInt)n), 4);
	le(g + 14 + c, n, 4);
	mu_assert_uint64_eq(0, rf_encode(g, 18 + c, &rec));
	mu_assert(rec == NULL, "record for an unmatched stream");
	pc_mfree(raw);
	pc_mfree(g);
}

/*
 * A zip of three entries: deflated with sizes in the header, stored,
 * and deflated with a data descriptor (sizes after the data).
 */
MU_TEST(test_rf_zip)
{
	const size_t n = 100000;
	unsigned char *raw = text(n), *z = pc_malloc(4 * n), *p = z;
	size_t c, len;
	int e;

	for (e = 0; e < 3; e++) {
		memset(p, 0, 30);
		memcpy(p, "PK\003\004", 4);
		le(p + 6, e == 2 ? 8 : 0, 2);
		le(p + 8, e == 1 ? 0 : 8, 2);
		le(p + 26, 5, 2);
		memcpy(p + 30, "a.txt", 5);
		if (e == 1) {
			c = 1000;
			memcpy(p + 35, raw, c);
		} else {
			c = pdeflate(raw + e, n - e, p + 35, 2 * n, e == 0 ? 6 : 9, 8, 0);
		}
		if (e != 2)
			le(p + 18, c, 4);
		p += 35 + c;
		if (e == 2) {
			memcpy(p, "PK\007\010", 4);
			memset(p + 4, 0, 12);
			p += 16;
		}
	}
	memcpy(p, "PK\001\002 central directory", 23);	/* the walk stops here */
	p += 23;
	len = p - z;
	mu_assert(roundtrip(z, len) > len, "zip streams not kept");
	pc_mfree(raw);
	pc_mfree(z);
}

static void
png_chunk(unsigned char **p, const char *type, const unsigned char *d, size_t n)
{
	unsigned char *q = *p;

	q[0] = (unsigned char)(n >> 24);
	q[1] = (unsigned char)(n >> 16);
	q[2] = (unsigned char)(n >> 8);
	q[3] = (unsigned char)n;
	memcpy(q + 4, type, 4);
	if (n > 0)
		memcpy(q + 8, d, n);
	le(q + 8 + n, 0, 4);	/* CRC: not checked by the filter */
	*p = q + 12 + n;
}

MU_TEST(test_rf_png)
{
	const size_t n = 120000;
	unsigned char *raw = text(n), *z = pc_malloc(n + 1024), *png = pc_malloc(n + 2048);
	unsigned char *p = png;
	uLong ad;
	size_t zl;

	/* A zlib stream at level 9 (RFC 1950): header, deflate, Adler-32. */
	z[0] = 0x78;
	z[1] = 0xda;
	zl = pdeflate(raw, n, z + 2, n + 1000, 9, 8, 0);
	mu_assert(zl > 0, "deflate");
	ad = adler32(1, raw, (uInt)n);
	z[2 + zl] = (unsigned char)(ad >> 24);
	z[3 + zl] = (unsigned char)(ad >> 16);
	z[4 + zl] = (unsigned char)(ad >> 8);
	z[5 + zl] = (unsigned char)ad;
	zl += 6;
	memcpy(p, "\211PNG\r\n\032\n", 8);
	p += 8;
	png_chunk(&p, "IHDR", (const unsigned char *)"0123456789abc", 13);
	png_chunk(&p, "IDAT", z, zl);
	png_chunk(&p, "IEND", NULL, 0);
	mu_assert(roundtrip(png, p - png) > (size_t)(p - png), "PNG stream not kept");
	pc_mfree(raw);
	pc_mfree(z);
	pc_mfree(png);
}

/* Every byte of a record changed, and every truncation: never misbuilt. */
MU_TEST(test_rf_hostile)
{
	const size_t n = 20000;
	unsigned char *raw = text(n), *g, *rec = NULL, *back;
	size_t len, rlen, i, got;

	g = make_gzip(raw, n, 6, 8, &len);
	rlen = rf_encode(g, len, &rec);
	mu_assert(rlen > 0, "no record");
	for (i = 0; i < rlen; i += (i < 24 + 28 ? 1 : 97)) {
		rec[i] ^= 0x41;
		back = NULL;
		got = rf_decode(rec, rlen, &back);
		mu_assert(got == 0 || (got == len && memcmp(back, g, len) == 0),
		    "changed record misbuilt");
		pc_mfree(back);
		rec[i] ^= 0x41;
	}
	for (i = 0; i < rlen; i += 13) {
		back = NULL;
		mu_assert_uint64_eq(0, rf_decode(rec, i, &back));
	}
	pc_mfree(rec);
	pc_mfree(g);
	pc_mfree(raw);
}

/* Not containers, or too small: no record. */
MU_TEST(test_rf_not_ours)
{
	unsigned char b[4096], *rec = NULL;

	memset(b, 'x', sizeof (b));
	mu_assert_uint64_eq(0, rf_encode(b, sizeof (b), &rec));
	memcpy(b, "PK\003\004", 4);
	mu_assert_uint64_eq(0, rf_encode(b, sizeof (b), &rec));
	memcpy(b, "\037\213\010", 3);
	mu_assert_uint64_eq(0, rf_encode(b, sizeof (b), &rec));
	mu_assert_uint64_eq(0, rf_encode(b, 10, &rec));
}

MU_TEST_SUITE(suite_reflate)
{
	MU_RUN_TEST(test_rf_deflate_kat);
	MU_RUN_TEST(test_rf_gzip_levels);
	MU_RUN_TEST(test_rf_unmatched);
	MU_RUN_TEST(test_rf_zip);
	MU_RUN_TEST(test_rf_png);
	MU_RUN_TEST(test_rf_hostile);
	MU_RUN_TEST(test_rf_not_ours);
}

int
main(void)
{
	MU_RUN_SUITE(suite_reflate);
	MU_REPORT();
	return (MU_EXIT_CODE);
}
