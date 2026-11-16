/*
 * Fuzzing harness for pc_chunk_open() and pc_keyfile_open(), which take
 * untrusted bytes: a backup tool opens objects and key files read back
 * from storage someone else may control.
 *
 * An unkeyed object's tag is a plain BLAKE2b-256, so anyone can forge a
 * valid one and reach the decompressors; random bytes almost never pass
 * the tag. So the input is split: the first byte picks a mode, and in the
 * "forged object" mode the harness builds a PCK1 header from the next
 * bytes (algorithm, level, declared length), uses the rest as payload,
 * and computes a valid tag with pc_chunk_id(), as an attacker would.
 * Open must then fail or return exactly the declared length.
 *
 * The other modes feed the raw input to pc_chunk_open() (with and
 * without a key, so the header and tag checks are fuzzed too) and to
 * pc_keyfile_open(). Uses only the public API. Built by "make fuzz-build"
 * (clang with libFuzzer and ASan); -DFUZZ_AFL builds a stdin driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>

#define	HDR		40
#define	TAG		32
#define	OUT_MAX		(1 << 20)	/* declared lengths are capped here */

static pc_ctx_t *plain_ctx, *keyed_ctx;
static uint8_t *out;

static void
quiet(void *a, int l, const char *m)
{
	(void) a; (void) l; (void) m;
}

static void
setup(void)
{
	static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	if (out != NULL)
		return;
	plain_ctx = pc_new();
	keyed_ctx = pc_new();
	out = malloc(OUT_MAX);
	if (plain_ctx == NULL || keyed_ctx == NULL || out == NULL)
		abort();
	pc_set_log(plain_ctx, quiet, NULL, PC_LOG_NONE);
	pc_set_log(keyed_ctx, quiet, NULL, PC_LOG_NONE);
	pc_set_key(keyed_ctx, key, sizeof (key));
}

/* Open and check the length contract: fail, or exactly what was declared. */
static void
open_check(pc_ctx_t *c, const uint8_t *obj, size_t len)
{
	size_t want = pc_chunk_open_size(obj, len), got = OUT_MAX;

	if (want != (size_t)-1 && want <= OUT_MAX) {
		got = want;
		if (pc_chunk_open(c, obj, len, out, &got) == 0 && got != want)
			abort();
	} else {
		/* Not a valid header, or too large to buffer: must still fail cleanly. */
		pc_chunk_open(c, obj, len, out, &got);
	}
}

/*
 * Input: algorithm id, level byte, 3-byte declared length, then payload.
 * Builds a version-1 unencrypted object with a valid tag around it.
 */
static void
forged(const uint8_t *d, size_t n)
{
	uint8_t *o;
	size_t plen, i;
	uint64_t plain;

	if (n < 5)
		return;
	plain = (uint64_t)d[2] | (uint64_t)d[3] << 8 | (uint64_t)(d[4] & 0x0f) << 16;
	plen = n - 5;
	if ((o = calloc(1, HDR + plen + TAG)) == NULL)
		abort();
	memcpy(o, "PCK1", 4);
	o[4] = 1;
	o[5] = d[0] & 0x80 ? 0x02 : 0;	/* PCK_RAW sometimes */
	o[6] = d[0] & 0x7f;
	o[7] = d[1];
	for (i = 0; i < 8; i++)
		o[8 + i] = (uint8_t)(plain >> (8 * i));
	memcpy(o + HDR, d + 5, plen);
	if (pc_chunk_id(o, HDR + plen, NULL, 0, o + HDR + plen) != 0)
		abort();
	open_check(plain_ctx, o, HDR + plen + TAG);
	free(o);
}

static int
fuzz_one(const uint8_t *data, size_t size)
{
	uint8_t key[32];

	setup();
	if (size < 1)
		return (0);
	switch (data[0] % 4) {
	case 0:
	case 1:
		forged(data + 1, size - 1);
		break;
	case 2:
		open_check(data[0] & 4 ? keyed_ctx : plain_ctx, data + 1, size - 1);
		break;
	case 3:
		/*
		 * A key file with the right magic runs scrypt with the file's
		 * own parameters, up to 512 MB and a second or more: far too
		 * slow for fuzzing. Clamp N and r in a copy so the KDF stays
		 * cheap; the parsing, the bounds checks around them and the
		 * tag check are what is under test.
		 */
		if (size - 1 == pc_keyfile_size()) {
			uint8_t kf[128];

			memcpy(kf, data + 1, sizeof (kf));
			if (kf[7] > 12)
				kf[7] = 10 + kf[7] % 3;
			kf[8] = 1 + kf[8] % 2;
			kf[9] = kf[10] = kf[11] = 0;
			if (kf[12] > 2)
				kf[12] = 1 + kf[12] % 2;
			pc_keyfile_open(kf, sizeof (kf), "pw", 2, key);
		} else {
			pc_keyfile_open(data + 1, size - 1, "pw", 2, key);
		}
		break;
	}
	return (0);
}

#ifdef FUZZ_AFL
int
main(void)
{
	static uint8_t buf[OUT_MAX];
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
