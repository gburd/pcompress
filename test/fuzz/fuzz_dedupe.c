/*
 * Fuzzing harness for dedupe_decompress(), which rebuilds a deduplicated
 * chunk from its block index and block data. Both come from the file, so
 * every count, index, offset and length in them is untrusted.
 *
 * The first byte picks the decoder context: 0 per-segment (-D), 1 fixed
 * block (-F), 2 global (-G). The rest is the chunk as the reader hands
 * it over: the dedupe header, the block index and the block data. The
 * harness applies the same header checks as the reader in pcompress.c,
 * then copies the chunk into a buffer of exactly its size, so ASan sees
 * any read past the data, and decodes into a buffer of exactly the
 * header's restored size. The contexts persist across inputs, as a
 * worker's does across chunks: stale block entries are part of the test.
 *
 * The global context restores references to earlier output from a
 * scratch file of 256 KB. Only a global chunk given to the global
 * context gets an index token: a reader must refuse a global chunk in a
 * per-segment file rather than wait for a token that never comes (that
 * shows up as a timeout).
 *
 * Built by "make fuzz-build" (clang with libFuzzer and ASan) against the
 * library's objects; -DFUZZ_AFL builds a stdin driver for AFL++.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <utils.h>
#include <allocator.h>
#include <rabin_dedup.h>

#define	FUZZ_CHUNK	(1024 * 1024)
#define	FUZZ_OUTFILE	(256 * 1024)

static dedupe_context_t *ctxs[3];
static Sem_t index_sem;

/* Build the three decoder contexts and the global one's output file once. */
static void
setup(void)
{
	static const int flags[3] = { RABIN_DEDUPE_SEGMENTED, RABIN_DEDUPE_FIXED,
	    RABIN_DEDUPE_FILE_GLOBAL };
	algo_props_t props;
	uchar_t pat[4096];
	FILE *f;
	int i;

	if (ctxs[0] != NULL)
		return;
	init_algo_props(&props);
	for (i = 0; i < 3; i++) {
		ctxs[i] = create_dedupe_context(FUZZ_CHUNK, FUZZ_CHUNK * 2, 0, "none",
		    &props, 0, flags[i], 10, DECOMPRESS, 0, NULL, 0, 1, 0, NULL);
		if (ctxs[i] == NULL)
			abort();
		ctxs[i]->index_sem = &index_sem;
	}
	if (Sem_Init(&index_sem, 0, 0) != 0 || (f = tmpfile()) == NULL)
		abort();
	for (i = 0; i < (int)sizeof (pat); i++)
		pat[i] = (uchar_t)(i * 7);
	for (i = 0; i < FUZZ_OUTFILE / (int)sizeof (pat); i++) {
		if (fwrite(pat, 1, sizeof (pat), f) != sizeof (pat))
			abort();
	}
	if (fflush(f) != 0)
		abort();
	ctxs[2]->out_fd = dup(fileno(f));
	set_log_level(-1);
}

static int
fuzz_one(const uint8_t *data, size_t size)
{
	dedupe_context_t *ctx;
	uint32_t blknum;
	uint64_t index_sz, data_sz, index_cmp, data_cmp, deduped_sz, n;
	uchar_t *buf, *out;

	setup();
	if (size < 1 + RABIN_HDR_SIZE)
		return (0);
	ctx = ctxs[data[0] % 3];
	data++;
	size--;

	/* The reader's checks on the header (pcompress.c). */
	parse_dedupe_hdr((uchar_t *)data, &blknum, &index_sz, &data_sz, &index_cmp,
	    &data_cmp, &deduped_sz);
	if (index_sz > FUZZ_CHUNK || data_sz > FUZZ_CHUNK || deduped_sz > FUZZ_CHUNK)
		return (0);
	n = RABIN_HDR_SIZE + index_sz + deduped_sz;
	if (n > size)
		return (0);

	buf = malloc(n);
	out = malloc(data_sz > 0 ? data_sz : 1);
	if (buf == NULL || out == NULL)
		abort();
	memcpy(buf, data, n);
	if (ctx == ctxs[2] && (blknum & GLOBAL_FLAG))
		Sem_Post(&index_sem);
	reset_dedupe_context(ctx);
	ctx->cbuf = out;
	n = deduped_sz;		/* the block data bytes, as the reader passes */
	dedupe_decompress(ctx, buf, &n);
	free(buf);
	free(out);
	return (0);
}

#ifdef FUZZ_AFL
int
main(void)
{
	static uint8_t buf[1 << 21];
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
