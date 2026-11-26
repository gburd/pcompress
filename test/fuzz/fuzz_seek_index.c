/*
 * Fuzzing harness for sx_parse(), the seek index parser. The index body
 * a random read uses has passed its tag, but the parser still treats it
 * as untrusted (an old writer's bug, or a key holder who forged it), so
 * it must refuse anything malformed without reading outside the body.
 *
 * Besides "no crash, no sanitizer report", it checks what callers rely
 * on for an accepted body: stream offsets increase, every chunk lies in
 * the file range given, every lookup lands in a real chunk, and every
 * member lies inside the stream.
 *
 * Built by "make fuzz-build" (clang with libFuzzer and ASan) against the
 * library's objects; -DFUZZ_AFL builds a stdin driver for AFL++.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <seek_index.h>

static int
fuzz_one(const uint8_t *data, size_t size)
{
	sx_limits_t lim = { 28, 1 << 20, 1 << 16, 0 };
	sx_index_t x;
	uint64_t i, c, off, len;

	if (size < 9)
		return (0);
	lim.taglen = data[8];	/* let the body choose: any length is checked */
	if (sx_parse(data, size, &lim, &x) != 0)
		return (0);
	for (i = 0; i < x.nchunks; i++) {
		if (x.uoff[i + 1] <= x.uoff[i] ||
		    sx_chunk_file_off(&x, i) < lim.file_lo ||
		    sx_chunk_file_off(&x, i) > lim.file_hi - 8)
			abort();
		c = sx_chunk_of(&x, x.uoff[i]);
		if (c != i || sx_chunk_of(&x, x.uoff[i + 1] - 1) != i)
			abort();
	}
	/* Look up the first member's name: the result must be in the stream. */
	if (x.nmembers > 0) {
		const unsigned char *m = x.members;
		size_t n = m[16] | ((size_t)m[17] << 8);
		char name[65536];

		memcpy(name, m + 18, n);
		name[n] = '\0';
		if (!sx_find_member(&x, name, &off, &len) || len == 0 ||
		    off + len > x.total)
			abort();
	}
	sx_index_free(&x);
	return (0);
}

#ifdef FUZZ_AFL
int
main(void)
{
	static uint8_t buf[1 << 20];
	size_t n = fread(buf, 1, sizeof (buf), stdin);

	return (fuzz_one(buf, n));
}
#else
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	return (fuzz_one(data, size));
}
#endif
