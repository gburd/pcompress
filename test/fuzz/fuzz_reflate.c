/*
 * Fuzzing harness for rf_decode(), the reflate record decoder. A record
 * comes from the archive, which an attacker may write, so the decoder
 * must refuse anything malformed without reading outside the record or
 * allocating beyond its limits.
 *
 * Byte 0 picks the mode. 0: the rest is a record, decoded as is. 1: the
 * rest is a member; rf_encode() makes a record from it, which must then
 * decode to exactly the member (the encoder's own promise).
 *
 * Built by "make fuzz-build" against the library's objects; -DFUZZ_AFL
 * builds a stdin driver for AFL++.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <pc_alloc.h>
#include "filters/reflate/reflate.h"

static int
fuzz_one(const uint8_t *data, size_t size)
{
	unsigned char *out = NULL, *rec = NULL;
	size_t n, r;

	if (size < 1)
		return (0);
	if (data[0] == 0) {
		n = rf_decode(data + 1, size - 1, &out);
		if (n == 0 && out != NULL)
			abort();
		pc_mfree(out);
		return (0);
	}
	r = rf_encode(data + 1, size - 1, &rec);
	if (r != 0) {
		n = rf_decode(rec, r, &out);
		if (n != size - 1 || memcmp(out, data + 1, n) != 0)
			abort();
	}
	pc_mfree(rec);
	pc_mfree(out);
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
