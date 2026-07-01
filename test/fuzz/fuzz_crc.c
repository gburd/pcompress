/*
 * Fuzzing harness for CRC32 and CRC64 implementations.
 *
 * Verifies that CRC functions do not crash on arbitrary input and that
 * incremental computation matches one-shot computation (differential
 * testing).
 *
 * Build with libFuzzer:
 *   clang -fsanitize=fuzzer,address -o fuzz_crc fuzz_crc.c crc32_fast.o crc64_fast.o ...
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lzma_crc.h>

static int
fuzz_one(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;

	/* One-shot CRC32 */
	uint32_t crc32_full = lzma_crc32(data, size, 0);

	/* Incremental CRC32: split at midpoint */
	size_t mid = size / 2;
	uint32_t crc32_part = lzma_crc32(data, mid, 0);
	uint32_t crc32_inc = lzma_crc32(data + mid, size - mid, crc32_part);

	/* Differential check: one-shot must equal incremental */
	if (crc32_full != crc32_inc) {
		fprintf(stderr, "CRC32 mismatch: full=0x%08x inc=0x%08x\n",
		    crc32_full, crc32_inc);
		abort();
	}

	/* One-shot CRC64 */
	uint64_t crc64_full = lzma_crc64(data, size, 0);

	/* Incremental CRC64 */
	uint64_t crc64_part = lzma_crc64(data, mid, 0);
	uint64_t crc64_inc = lzma_crc64(data + mid, size - mid, crc64_part);

	if (crc64_full != crc64_inc) {
		fprintf(stderr, "CRC64 mismatch: full=0x%016llx inc=0x%016llx\n",
		    (unsigned long long)crc64_full,
		    (unsigned long long)crc64_inc);
		abort();
	}

	return 0;
}

#ifdef FUZZ_AFL
int
main(void)
{
	uint8_t buf[65536];
	size_t n = fread(buf, 1, sizeof(buf), stdin);
	if (n > 0)
		fuzz_one(buf, n);
	return 0;
}
#else
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	return fuzz_one(data, size);
}
#endif
