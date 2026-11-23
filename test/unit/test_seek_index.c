/*
 * Unit tests for the seek index body (seek_index.c): the encoding is
 * pinned by a known-answer vector built independently (Python struct,
 * see the comment at kat[]), and the parser refuses every malformed body
 * it is given rather than reading past it.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <pc_alloc.h>
#include <seek_index.h>
#include "minunit.h"

/*
 * Two chunks (at file offsets 30 and 500, 600 and 400 bytes, 4-byte tags
 * 01020304 and 05060708), one member "a/b.c" at stream offset 512 of 300
 * bytes, end marker at 1000. From:
 *   b"PCSXIDX1" + bytes([4]) + bytes(7) + struct.pack("<QQQ", 1000, 2, 1)
 *   + struct.pack("<QQ", 30, 600) + b"\x01\x02\x03\x04"
 *   + struct.pack("<QQ", 500, 400) + b"\x05\x06\x07\x08"
 *   + struct.pack("<QQH", 512, 300, 5) + b"a/b.c"
 */
static const unsigned char kat[] = {
	0x50, 0x43, 0x53, 0x58, 0x49, 0x44, 0x58, 0x31, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
	0xf4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x06, 0x07, 0x08, 0x00, 0x02, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x2c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x61, 0x2f, 0x62, 0x2e, 0x63
};

static const sx_limits_t lim = { 28, 1000, 1024, 4 };

/* The writer produces exactly the vector. */
MU_TEST(test_sx_encode_kat)
{
	static const unsigned char t1[4] = { 1, 2, 3, 4 }, t2[4] = { 5, 6, 7, 8 };
	sx_writer_t w;
	unsigned char *b;
	uint64_t len = 0;

	sx_writer_init(&w, 4);
	sx_add_chunk(&w, 30, 600, t1);
	sx_add_chunk(&w, 500, 400, t2);
	sx_add_member(&w, "a/b.c", 512);
	sx_end_member(&w, 812);
	b = sx_body(&w, 1000, &len);
	mu_assert(b != NULL, "sx_body failed");
	mu_assert_uint64_eq(sizeof (kat), len);
	mu_assert_mem_eq(kat, b, sizeof (kat));
	pc_mfree(b);
	sx_writer_free(&w);
}

/* The parser reads the vector back, and lookups land in the right chunk. */
MU_TEST(test_sx_parse_kat)
{
	sx_index_t x;
	uint64_t off, len;

	mu_assert_int_eq(0, sx_parse(kat, sizeof (kat), &lim, &x));
	mu_assert_uint64_eq(2, x.nchunks);
	mu_assert_uint64_eq(1000, x.total);
	mu_assert_uint64_eq(0, sx_chunk_of(&x, 0));
	mu_assert_uint64_eq(0, sx_chunk_of(&x, 599));
	mu_assert_uint64_eq(1, sx_chunk_of(&x, 600));
	mu_assert_uint64_eq(1, sx_chunk_of(&x, 999));
	mu_assert_uint64_eq(500, sx_chunk_file_off(&x, 1));
	mu_assert(sx_chunk_tag(&x, 1)[0] == 5, "wrong tag");
	mu_assert(sx_find_member(&x, "a/b.c", &off, &len) == 1, "member not found");
	mu_assert_uint64_eq(512, off);
	mu_assert_uint64_eq(300, len);
	mu_assert(sx_find_member(&x, "a/b", &off, &len) == 0, "prefix matched");
	sx_index_free(&x);
}

/* Parse a copy of kat with byte i set to v; returns sx_parse()'s result. */
static int
parse_patched(size_t i, unsigned char v, size_t len)
{
	unsigned char b[sizeof (kat)];
	sx_index_t x;
	int rv;

	memcpy(b, kat, sizeof (kat));
	b[i] = v;
	rv = sx_parse(b, len, &lim, &x);
	if (rv == 0)
		sx_index_free(&x);
	return (rv);
}

/* Every field that could send a reader astray is refused. */
MU_TEST(test_sx_parse_hostile)
{
	size_t n;

	mu_assert_int_eq(-1, parse_patched(0, 'X', sizeof (kat)));	/* magic */
	mu_assert_int_eq(-1, parse_patched(8, 8, sizeof (kat)));	/* tag length */
	mu_assert_int_eq(-1, parse_patched(16, 0xe9, sizeof (kat)));	/* end marker */
	mu_assert_int_eq(-1, parse_patched(24, 0xff, sizeof (kat)));	/* chunk count */
	mu_assert_int_eq(-1, parse_patched(24, 0, sizeof (kat)));	/* no chunks */
	mu_assert_int_eq(-1, parse_patched(32, 2, sizeof (kat)));	/* member count */
	mu_assert_int_eq(-1, parse_patched(40, 0x0a, sizeof (kat)));	/* inside header */
	{
		unsigned char b[sizeof (kat)];
		sx_index_t x;

		memcpy(b, kat, sizeof (kat));
		b[60] = 33;	/* chunk 1 at 33: overlaps chunk 0's length field */
		b[61] = 0;
		mu_assert_int_eq(-1, sx_parse(b, sizeof (b), &lim, &x));
	}
	mu_assert_int_eq(-1, parse_patched(61, 0x03, sizeof (kat)));	/* past the end */
	{
		unsigned char b[sizeof (kat)];
		sx_index_t x;

		/* Chunk 0 at 2^64 - 4: "off + 8" wraps to 4, inside the file. */
		memcpy(b, kat, sizeof (kat));
		b[40] = 0xfc;
		memset(b + 41, 0xff, 7);
		mu_assert_int_eq(-1, sx_parse(b, sizeof (b), &lim, &x));
	}
	mu_assert_int_eq(-1, parse_patched(49, 0x08, sizeof (kat)));	/* > chunksize */
	{
		unsigned char b[sizeof (kat)];
		sx_index_t x;

		memcpy(b, kat, sizeof (kat));
		b[48] = b[49] = 0;	/* chunk 0 decodes to nothing */
		mu_assert_int_eq(-1, sx_parse(b, sizeof (b), &lim, &x));
	}
	mu_assert_int_eq(-1, parse_patched(85, 0x04, sizeof (kat)));	/* member past total */
	mu_assert_int_eq(-1, parse_patched(97, 0x06, sizeof (kat)));	/* name length */
	mu_assert_int_eq(-1, parse_patched(100, 0, sizeof (kat)));	/* NUL in name */
	/* Every truncation, and one byte too many. */
	for (n = 0; n < sizeof (kat); n++)
		mu_assert_int_eq(-1, parse_patched(0, 'P', n));
	{
		unsigned char b[sizeof (kat) + 1];
		sx_index_t x;

		memcpy(b, kat, sizeof (kat));
		b[sizeof (kat)] = 0;
		mu_assert_int_eq(-1, sx_parse(b, sizeof (b), &lim, &x));
	}
}

MU_TEST_SUITE(suite_seek_index)
{
	MU_RUN_TEST(test_sx_encode_kat);
	MU_RUN_TEST(test_sx_parse_kat);
	MU_RUN_TEST(test_sx_parse_hostile);
}

int
main(void)
{
	MU_RUN_SUITE(suite_seek_index);
	MU_REPORT();
	return (MU_EXIT_CODE);
}
