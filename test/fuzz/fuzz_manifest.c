/*
 * Fuzzing harness for pc_manifest_decode(), which takes untrusted bytes
 * (a backup tool decodes manifests it reads back from storage).
 *
 * Besides "no crash, no sanitizer report", it checks the property the
 * format promises: the encoding is canonical, so anything the decoder
 * accepts must re-encode to exactly the input bytes, and every entry it
 * returns must be one pc_manifest_find() finds again.
 *
 * Uses only the public API. Built by "make fuzz-build" (clang with
 * libFuzzer and ASan); -DFUZZ_AFL builds a stdin driver for AFL++.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>

static int
fuzz_one(const uint8_t *data, size_t size)
{
	pc_manifest_t *m = pc_manifest_decode(data, size);
	size_t i, n, len = 0;
	uint8_t *re;
	pc_entry_t e, f;

	if (m == NULL)
		return (0);

	/* Accepted: it must re-encode to the same bytes. */
	if (pc_manifest_encode(m, NULL, &len) != 0 || len != size)
		abort();
	re = malloc(len ? len : 1);
	if (re == NULL)
		abort();
	if (pc_manifest_encode(m, re, &len) != 0 || len != size ||
	    memcmp(re, data, size) != 0)
		abort();
	free(re);

	/* Every entry is reachable by its path, at its own index. */
	n = pc_manifest_count(m);
	for (i = 0; i < n; i++) {
		if (pc_manifest_get(m, i, &e) != 0)
			abort();
		if (pc_manifest_find(m, e.path, &f) != (int64_t)i ||
		    f.nchunks != e.nchunks || f.size != e.size)
			abort();
	}
	pc_manifest_free(m);
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
