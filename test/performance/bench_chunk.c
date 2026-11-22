/*
 * Throughput of the chunk-store pipeline a backup tool runs, through the
 * public API only: the content-defined chunkers, the keyed chunk ids
 * (BLAKE2b and BLAKE3), and sealing and opening each chunk (compress,
 * encrypt, authenticate, and back). Prints MB/s of plaintext per stage,
 * one thread.
 *
 *   bench_chunk [MB] [algorithm[:level] ...]
 *
 * The input is half text-like (repetitive, compressible) and half random,
 * so codecs do real work and some chunks are stored raw. Default 256 MB,
 * algorithms zstd:3 lz4:1 lzma:6. A last section seals 2 KB pieces of
 * the text half with zstd:3, without and with a trained dictionary.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pcompress/pcompress.h>

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec + tv.tv_usec / 1e6);
}

static void
fill(unsigned char *b, size_t n)
{
	static const char *w[] = { "backup ", "chunk ", "store ", "object ", "id ",
	    "seal ", "open ", "manifest ", "index ", "key\n" };
	uint64_t r = 0x9e3779b97f4a7c15ULL;
	size_t i = 0, half = n / 2;

	while (i < half) {
		const char *s;

		r ^= r << 13; r ^= r >> 7; r ^= r << 17;
		s = w[r % 10];
		while (*s && i < half)
			b[i++] = (unsigned char)*s++;
	}
	for (; i < n; i++) {
		r ^= r << 13; r ^= r >> 7; r ^= r << 17;
		b[i] = (unsigned char)(r >> 32);
	}
}

static void
report(const char *what, size_t bytes, double t)
{
	printf("  %-28s %8.1f MB/s\n", what, bytes / 1e6 / (t > 0 ? t : 1e-9));
}

/*
 * zstd:3 on 2 KB pieces of the first len bytes of buf (text), without and
 * with a 64 KB dictionary trained on every other piece; the pieces sealed
 * are the ones in between.
 */
static void
bench_dict(const unsigned char *buf, size_t len)
{
	size_t piece = 2048, np = len / piece / 2, *sizes = malloc(np * sizeof (*sizes)), i;
	size_t dl = 64 * 1024, ol, pl;
	unsigned char *samples = malloc(np * piece), *dict = malloc(dl);
	unsigned char obj[4096], back[2048];
	int d;

	if (np > 20000)
		np = 20000;
	for (i = 0; i < np; i++) {
		memcpy(samples + i * piece, buf + 2 * i * piece, piece);
		sizes[i] = piece;
	}
	if (pc_dict_train(samples, sizes, (unsigned)np, dict, &dl) != 0) {
		printf("  dictionary: training failed\n");
		return;
	}
	for (d = 0; d < 2; d++) {
		pc_ctx_t *c = pc_new();
		size_t sealed = 0;
		double ts = 0, to = 0, t0;

		pc_set_log(c, NULL, NULL, PC_LOG_NONE);
		pc_set_level(c, 3);
		if (d)
			pc_set_dict(c, dict, dl);
		for (i = 0; i < np; i++) {
			const unsigned char *p = buf + (2 * i + 1) * piece;

			ol = sizeof (obj);
			t0 = now();
			if (pc_chunk_seal(c, p, piece, obj, &ol) != 0)
				exit(6);
			ts += now() - t0;
			sealed += ol;
			pl = sizeof (back);
			t0 = now();
			if (pc_chunk_open(c, obj, ol, back, &pl) != 0 || pl != piece)
				exit(7);
			to += now() - t0;
		}
		report(d ? "seal 2 KB zstd:3 +dict" : "seal 2 KB zstd:3", np * piece, ts);
		report(d ? "open 2 KB zstd:3 +dict" : "open 2 KB zstd:3", np * piece, to);
		printf("  %-28s %8.1f %%\n", "  stored size", 100.0 * sealed / (np * piece));
		pc_free(c);
	}
	free(sizes);
	free(samples);
	free(dict);
}

int
main(int argc, char **argv)
{
	size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 256, n = mb << 20;
	static const char *dflt[] = { "zstd:3", "lz4:1", "lzma:6" };
	const char **algos = argc > 2 ? (const char **)argv + 2 : dflt;
	int nalgos = argc > 2 ? argc - 2 : 3, a, nc = 0, k, cap;
	unsigned char *buf = malloc(n), key[32] = { 7 }, id[32];
	unsigned char *obj = malloc(pc_chunk_seal_bound(1 << 20)), *back = malloc(1 << 20);
	size_t *lens, off;
	pc_chunker_t *ch = pc_chunker_new(0, 0, 0);
	double t;

	if (buf == NULL || obj == NULL || back == NULL || ch == NULL || mb == 0)
		return (2);
	fill(buf, n);
	cap = (int)(n / 16384) + 16;
	lens = malloc(cap * sizeof (*lens));
	printf("%zu MB, one thread, default chunker sizes (avg 64 KB, min 16 KB)\n", mb);

	{
		pc_chunker_t *g = pc_chunker_new2(PC_CHUNKER_GEAR, 0, 0, 0);
		int ng;

		t = now();
		ng = pc_chunker_feed(g, buf, n, lens, cap);
		if (ng < 0)
			return (3);
		ng += pc_chunker_finish(g, lens + ng, cap - ng);
		report("chunker gear", n, now() - t);
		printf("  %-28s %8d (avg %zu bytes)\n", "  chunks", ng, n / (ng ? ng : 1));
		pc_chunker_free(g);
	}
	/* The rabin chunks go on to the later stages. */
	t = now();
	nc = pc_chunker_feed(ch, buf, n, lens, cap);
	if (nc < 0)
		return (3);
	nc += pc_chunker_finish(ch, lens + nc, cap - nc);
	report("chunker rabin", n, now() - t);
	printf("  %-28s %8d (avg %zu bytes)\n", "  chunks", nc, n / (nc ? nc : 1));

	t = now();
	for (off = 0, k = 0; k < nc; off += lens[k++])
		pc_chunk_id(buf + off, lens[k], key, 32, id);
	report("chunk id (keyed BLAKE2b)", n, now() - t);
	t = now();
	for (off = 0, k = 0; k < nc; off += lens[k++])
		pc_chunk_id2(PC_ID_BLAKE3, buf + off, lens[k], key, 32, id);
	report("chunk id (keyed BLAKE3)", n, now() - t);

	for (a = 0; a < nalgos; a++) {
		char name[32], *colon;
		int level = 6, keyed;

		snprintf(name, sizeof (name), "%s", algos[a]);
		if ((colon = strchr(name, ':')) != NULL) {
			*colon = '\0';
			level = atoi(colon + 1);
		}
		for (keyed = 0; keyed < 2; keyed++) {
			pc_ctx_t *c = pc_new();
			size_t sealed = 0, ol, pl;
			double ts = 0, to = 0, t0;
			char label[64];

			pc_set_log(c, NULL, NULL, PC_LOG_NONE);
			if (pc_set_algorithm(c, name) != 0 || pc_set_level(c, level) != 0) {
				printf("  %s: not available\n", algos[a]);
				pc_free(c);
				break;
			}
			if (keyed)
				pc_set_key(c, key, 32);
			for (off = 0, k = 0; k < nc; off += lens[k++]) {
				ol = pc_chunk_seal_bound(lens[k]);
				t0 = now();
				if (pc_chunk_seal(c, buf + off, lens[k], obj, &ol) != 0)
					return (4);
				ts += now() - t0;
				sealed += ol;
				pl = 1 << 20;
				t0 = now();
				if (pc_chunk_open(c, obj, ol, back, &pl) != 0 || pl != lens[k])
					return (5);
				to += now() - t0;
			}
			snprintf(label, sizeof (label), "seal %s%s", algos[a], keyed ? " +key" : "");
			report(label, n, ts);
			snprintf(label, sizeof (label), "open %s%s", algos[a], keyed ? " +key" : "");
			report(label, n, to);
			if (!keyed)
				printf("  %-28s %8.1f %%\n", "  stored size", 100.0 * sealed / n);
			pc_free(c);
		}
	}
	bench_dict(buf, n / 2);
	pc_chunker_free(ch);
	free(buf); free(obj); free(back); free(lens);
	return (0);
}
