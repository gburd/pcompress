/*
 * Chunk-level primitives (ROADMAP Phase 3): a streaming content-defined
 * chunker, chunk identities, and sealed chunk objects.
 *
 * This file is a part of Pcompress, a chunked parallel multi-algorithm
 * lossless compression and decompression program.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <utils.h>
#include <pcompress.h>
#include <crypto_utils.h>
#include <crypto_xsalsa20.h>
#include <rabin_dedup.h>
#include <crypto_scrypt.h>
#include <blake3.h>

/* --- Content-defined chunker ------------------------------------------- */

/*
 * The rolling hash is dedupe_compress()'s: a 16-byte window, multiplier
 * RAB_POLYNOMIAL_CONST, and the ir[]/out[] tables. A boundary is placed
 * after byte i when at least min bytes are in the chunk and
 * ((roll ^ ir[pushed_out]) & mask) == 0, or when the chunk reaches max.
 * The window restarts at every boundary, so a boundary depends only on the
 * bytes since the previous one: feeding the same data in any split gives
 * the same chunks.
 */
/*
 * PC_CHUNKER_GEAR is FastCDC's: h = (h << 1) + gear[byte], so the top 64
 * bits of h depend on the last 64 bytes only, with normalized chunking.
 * Before avg bytes a boundary needs the top log2(avg) + 2 bits of h zero,
 * from avg on the top log2(avg) - 2, so sizes cluster near avg. The gear
 * table is splitmix64 from a fixed seed. Both, and so every boundary, are
 * part of the repository format: never change them.
 */
struct pc_chunker {
	int algo;		/* PC_CHUNKER_* */
	const uint64_t *ir, *out;	/* rabin tables */
	size_t min, max, avg;
	uint64_t mask, mask_s, mask_l;	/* rabin; gear before / from avg */
	uint64_t roll;
	uchar_t win[RAB_POLYNOMIAL_WIN_SIZE];
	unsigned wpos;
	size_t cur;		/* bytes in the current, unfinished chunk */
	uint64_t gear[256];
};

#define	PC_CHUNK_AVG_DEFAULT	(64 * 1024)
#define	PC_CHUNK_MAX_LIMIT	(64UL * 1024 * 1024)

/* The gear table: splitmix64 seeded with "pcompres". */
static void
gear_table(uint64_t g[256])
{
	uint64_t s = 0x70636f6d70726573ULL, z;
	int i;

	for (i = 0; i < 256; i++) {
		s += 0x9e3779b97f4a7c15ULL;
		z = s;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		g[i] = z ^ (z >> 31);
	}
}

pc_chunker_t DLL_EXPORT *
pc_chunker_new(size_t avg, size_t min, size_t max)
{
	return (pc_chunker_new2(PC_CHUNKER_RABIN, avg, min, max));
}

pc_chunker_t DLL_EXPORT *
pc_chunker_new2(int algo, size_t avg, size_t min, size_t max)
{
	pc_chunker_t *c;
	size_t a = 1;
	int bits = 0;

	if (avg == 0)
		avg = PC_CHUNK_AVG_DEFAULT;
	if (min == 0)
		min = avg / 4;
	if (max == 0)
		max = avg * 4;
	/* avg is rounded down to a power of two (it is a bit mask). */
	while (a * 2 <= avg) {
		a *= 2;
		bits++;
	}
	if ((algo != PC_CHUNKER_RABIN && algo != PC_CHUNKER_GEAR) ||
	    a < 256 || min < 64 || min > a || max < a || max > PC_CHUNK_MAX_LIMIT)
		return (NULL);
	if ((c = pc_calloc(1, sizeof (*c))) == NULL)
		return (NULL);
	c->algo = algo;
	c->min = min;
	c->max = max;
	c->avg = a;
	if (algo == PC_CHUNKER_RABIN) {
		rabin_tables(&c->ir, &c->out);
		/* Expected gap past min is about avg - min; keep the mask at avg. */
		c->mask = a - 1;
	} else {
		gear_table(c->gear);
		c->mask_s = ~(uint64_t)0 << (64 - (bits + 2));
		c->mask_l = ~(uint64_t)0 << (64 - (bits - 2));
	}
	return (c);
}

void DLL_EXPORT
pc_chunker_free(pc_chunker_t *c)
{
	pc_mfree(c);
}

static void
chunk_reset(pc_chunker_t *c)
{
	c->roll = 0;
	c->wpos = 0;
	c->cur = 0;
	memset(c->win, 0, sizeof (c->win));
}

/*
 * pc_chunker_feed() for PC_CHUNKER_GEAR. h at a candidate boundary depends
 * on the last 64 bytes only, and no boundary comes before min >= 64 bytes,
 * so the first min - 64 bytes of each chunk are skipped unhashed (h is 0
 * at a chunk's start, and 64 shifts clear whatever came before).
 */
static int
gear_feed(pc_chunker_t *c, const uchar_t *p, size_t len, size_t *lens, int nlens)
{
	const uint64_t *gear = c->gear;
	const size_t min = c->min, avg = c->avg, max = c->max;
	size_t i = 0, cur = c->cur;
	uint64_t h = c->roll;
	int n = 0;

	/*
	 * cur counts the chunk's bytes before p[i]. By cur the phases are:
	 * skip to min - 64; hash, untested, to min - 1; test the strict mask
	 * to avg - 1 (so bytes min to avg - 1 of the chunk); the loose one to
	 * max. Each inner loop runs to the end of its phase or of the input,
	 * so a call stops and resumes in any phase. Copies of c's fields are
	 * locals because a byte load could alias any of them.
	 */
	while (i < len) {
		size_t start = i, lim, end;
		uint64_t m;
		int cut = 0;

		if (cur < min - 64) {
			lim = min - 64;
		} else if (cur < min - 1) {
			lim = min - 1;
			end = i + (lim - cur < len - i ? lim - cur : len - i);
			for (; i < end; i++)
				h = (h << 1) + gear[p[i]];
			cur += i - start;
			continue;
		} else {
			lim = cur < avg - 1 ? avg - 1 : max;
			m = cur < avg - 1 ? c->mask_s : c->mask_l;
			end = i + (lim - cur < len - i ? lim - cur : len - i);
			while (i < end) {
				h = (h << 1) + gear[p[i++]];
				if ((h & m) == 0) {
					cut = 1;
					break;
				}
			}
			cur += i - start;
			if (!cut && cur < max)
				continue;
			if (n == nlens)
				return (-2);	/* lens[] full: caller must feed less */
			lens[n++] = cur;
			cur = 0;
			h = 0;
			continue;
		}
		/* Skipping: those bytes cannot reach h at any boundary. */
		end = lim - cur < len - i ? lim - cur : len - i;
		cur += end;
		i += end;
	}
	c->cur = cur;
	c->roll = h;
	return (n);
}

int DLL_EXPORT
pc_chunker_feed(pc_chunker_t *c, const void *buf, size_t len, size_t *lens, int nlens)
{
	const uchar_t *p = (const uchar_t *)buf;
	size_t i;
	int n = 0;

	if (c == NULL || (buf == NULL && len > 0) || (lens == NULL && nlens > 0) || nlens < 0)
		return (-1);
	if (c->algo == PC_CHUNKER_GEAR)
		return (gear_feed(c, p, len, lens, nlens));
	for (i = 0; i < len; i++) {
		uchar_t b = p[i], o = c->win[c->wpos];

		c->win[c->wpos] = b;
		c->wpos = (c->wpos + 1) & (RAB_POLYNOMIAL_WIN_SIZE - 1);
		c->roll = ((c->roll * RAB_POLYNOMIAL_CONST) & POLY_MASK) + b - c->out[o];
		c->cur++;
		if ((c->cur >= c->min && ((c->roll ^ c->ir[o]) & c->mask) == 0) ||
		    c->cur >= c->max) {
			if (n == nlens)
				return (-2);	/* lens[] full: caller must feed less */
			lens[n++] = c->cur;
			chunk_reset(c);
		}
	}
	return (n);
}

int DLL_EXPORT
pc_chunker_finish(pc_chunker_t *c, size_t *lens, int nlens)
{
	int n = 0;

	if (c == NULL)
		return (-1);
	if (c->cur > 0) {
		if (nlens < 1 || lens == NULL)
			return (-2);
		lens[n++] = c->cur;
	}
	chunk_reset(c);
	return (n);
}

/* --- Chunk identity ----------------------------------------------------- */

int DLL_EXPORT
pc_chunk_id(const void *buf, size_t len, const void *key, size_t keylen, uint8_t id[32])
{
	if ((buf == NULL && len > 0) || id == NULL || keylen > 64 || (key == NULL) != (keylen == 0))
		return (-1);
	return (blake2b_keyed2(id, 32, key, (int)keylen, buf, len, NULL, 0));
}

int DLL_EXPORT
pc_chunk_id2(int alg, const void *buf, size_t len, const void *key, size_t keylen,
    uint8_t id[32])
{
	blake3_hasher h;

	if (alg == PC_ID_BLAKE2B)
		return (pc_chunk_id(buf, len, key, keylen, id));
	if (alg != PC_ID_BLAKE3 || (buf == NULL && len > 0) || id == NULL ||
	    (key == NULL ? keylen != 0 : keylen != BLAKE3_KEY_LEN))
		return (-1);
	if (key != NULL)
		blake3_hasher_init_keyed(&h, key);
	else
		blake3_hasher_init(&h);
	blake3_hasher_update(&h, buf, len);
	blake3_hasher_finalize(&h, id, 32);
	/* The state holds the key's chaining values. */
	memset(&h, 0, sizeof (h));
	return (0);
}

/*
 * A 256-bit key for one purpose from a master key: BLAKE2b keyed with the
 * master over "pcompress derive", one length byte and the purpose. The
 * digest is 48 bytes, cut to 32: BLAKE2b's parameter block holds the
 * digest length, so no output here equals a 32-byte chunk id made with
 * the master key, whatever data that id was taken over. The length byte
 * keeps one purpose from being a prefix of another.
 */
int DLL_EXPORT
pc_derive_key(const uint8_t master[32], const char *purpose, uint8_t out[32])
{
	static const uchar_t dom[] = "pcompress derive";
	uchar_t hdr[sizeof (dom)], k[48];
	size_t plen;
	int rv;

	if (master == NULL || purpose == NULL || out == NULL ||
	    (plen = strlen(purpose)) == 0 || plen > 255)
		return (-1);
	memcpy(hdr, dom, sizeof (dom) - 1);
	hdr[sizeof (dom) - 1] = (uchar_t)plen;
	rv = blake2b_keyed2(k, 48, master, 32, hdr, sizeof (hdr),
	    (const uchar_t *)purpose, plen);
	if (rv == 0)
		memcpy(out, k, 32);
	memset(k, 0, sizeof (k));
	return (rv);
}

/* --- Sealed chunk objects ----------------------------------------------- */

/*
 * Object v1, all integers little-endian:
 *
 *   0   4  magic "PCK1"
 *   4   1  version (1)
 *   5   1  flags: PCK_ENCRYPTED, PCK_RAW (payload not compressed)
 *   6   1  algorithm id (see algo_ids[]; 0 when PCK_RAW)
 *   7   1  level
 *   8   8  plaintext length
 *  16  24  nonce (random when encrypted, else zero)
 *  40  32  with PCK_DICT only: the dictionary's id (pc_dict_id())
 *  h    n  payload: compressed (or raw) bytes, encrypted when PCK_ENCRYPTED;
 *          h is 72 with PCK_DICT, else 40
 *  h+n 32  tag: BLAKE2b-256 over bytes [0, h+n), keyed with the MAC
 *          subkey when encrypted, unkeyed (an integrity check) otherwise
 *
 * Encrypt-then-MAC: the tag covers the header, so nothing in it can be
 * changed. open() checks the tag before it decrypts or decompresses.
 * Keys: enc || mac = BLAKE2b-512(key, "pcompress chunk subkeys"); see
 * subkeys() for why not two BLAKE2b-256 values.
 *
 * PCK_DICT (since 4.5): the payload is a zstd frame made with the
 * dictionary the id names. Only zstd objects carry it, never raw ones, so
 * objects sealed without a dictionary are what older versions write, and
 * those versions refuse PCK_DICT objects (an unknown flag) as corrupt.
 */
#define	PCK_MAGIC	"PCK1"
#define	PCK_VERSION	1
#define	PCK_HDR		40
#define	PCK_DICTID	32
#define	PCK_TAG		32
#define	PCK_ENCRYPTED	0x01
#define	PCK_RAW		0x02
#define	PCK_DICT	0x04
#define	PCK_FLAGS	(PCK_ENCRYPTED | PCK_RAW | PCK_DICT)
#define	PCK_ZSTD	1	/* algo_ids[] index of "zstd" */
/* Largest plaintext open() accepts, whatever the header says. */
#define	PCK_MAX_PLAIN	(1ULL << 32)

/* Stable on-disk ids; the index is the id. Never reorder. */
static const char *algo_ids[] = { "", "zstd", "lz4", "lzma", "zlib", "bzip2",
    "ppmd", "lzfx", "libbsc", "adapt", "adapt2", "lzmaMt", NULL };

static int
algo_id(const char *name)
{
	int i;

	for (i = 1; algo_ids[i] != NULL; i++)
		if (strcmp(algo_ids[i], name) == 0)
			return (i);
	return (-1);
}

size_t DLL_EXPORT
pc_chunk_seal_bound(size_t len)
{
	/* Raw is always possible, so the bound is raw plus framing. */
	if (len > SIZE_MAX - PCK_HDR - PCK_TAG)
		return (SIZE_MAX);	/* no buffer is that big: seal then refuses */
	return (len + PCK_HDR + PCK_TAG);
}

static void
put_le64(uchar_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uchar_t)(v >> (8 * i));
}

static uint64_t
get_le64(const uchar_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return (v);
}

/*
 * Chunk ids are public (object names) and are BLAKE2b-256 keyed, often with
 * this same key, over data an attacker may choose. So the subkeys must not
 * be BLAKE2b-256 of anything: they come from one 64-byte digest, and
 * BLAKE2b's parameter block holds the digest length, making its output
 * unrelated to every 32-byte id.
 */
static int
subkeys(pc_ctx_t *ctx, uchar_t enc[32], uchar_t mac[32])
{
	static const uchar_t d[] = "pcompress chunk subkeys";
	uchar_t k[64];

	if (blake2b_keyed2(k, 64, ctx->chunk_key, 32, d, sizeof (d) - 1, NULL, 0) != 0)
		return (-1);
	memcpy(enc, k, 32);
	memcpy(mac, k + 32, 32);
	memset(k, 0, sizeof (k));
	return (0);
}

/* Constant-time compare of n bytes. */
static int
ct_equal(const uchar_t *a, const uchar_t *b, int n)
{
	uchar_t d = 0;
	int i;

	for (i = 0; i < n; i++)
		d |= a[i] ^ b[i];
	return (d == 0);
}

/* The context's algorithm handlers and their state. */
struct codec {
	compress_func_ptr comp, decomp;
	init_func_ptr init;
	deinit_func_ptr deinit;
	void *data;
	int level;
};

extern int pc_init_algo_handlers(pc_ctx_t *pctx, const char *algo);

/*
 * Codec state kept in the context between pc_chunk_seal() (and open)
 * calls: one for compressing, one for decompressing. Setting a codec up
 * allocates its working memory (a zstd context, an lzma encoder's tables),
 * which for 64 KB chunks cost more than compressing them: zstd at level
 * 12 and lzma ran at 1-2 MB/s. The state is kept for one algorithm and
 * level and for inputs up to len bytes, and remade when any changes.
 */
struct codec_cache {
	char algo[ALGO_SZ + 1];
	int level;
	uint64_t len;
	struct codec k;
};

static void
codec_close(struct codec *k);

static int
codec_open(pc_ctx_t *ctx, const char *algo, int level, uint64_t len, compress_op_t op,
    struct codec *k)
{
	memset(k, 0, sizeof (*k));
	if (pc_init_algo_handlers(ctx, algo) != 0)
		return (-1);
	k->comp = ctx->_compress_func;
	k->decomp = ctx->_decompress_func;
	k->init = ctx->_init_func;
	k->deinit = ctx->_deinit_func;
	k->level = level;
	if (k->init != NULL && k->init(&k->data, &k->level, 1, len ? len : 1, VERSION, op) != 0)
		return (-1);
	return (0);
}

/*
 * The context's codec for algo at level, able to take len bytes: the
 * kept one if it matches, else a new one (at least 256 KB, rounded up to
 * a power of two below 1 GB, so chunks of varying size do not each remake
 * it).
 * NULL on failure. Owned by the context (chunk_codecs_free()).
 */
static struct codec *
codec_get(pc_ctx_t *ctx, const char *algo, int level, uint64_t len, compress_op_t op)
{
	struct codec_cache **slot = (struct codec_cache **)&ctx->chunk_codec[op == COMPRESS];
	struct codec_cache *c = *slot;
	uint64_t cap = 256 * 1024;

	if (c != NULL && c->level == level && len <= c->len && strcmp(c->algo, algo) == 0)
		return (&c->k);
	if (c != NULL) {
		codec_close(&c->k);
		pc_mfree(c);
		*slot = NULL;
	}
	while (cap < len && cap < ((uint64_t)1 << 30))
		cap <<= 1;
	if (cap < len)
		cap = len;	/* past 1 GB: exact, codecs have limits near 2 GB */
	if ((c = pc_calloc(1, sizeof (*c))) == NULL)
		return (NULL);
	if (codec_open(ctx, algo, level, cap, op, &c->k) != 0) {
		codec_close(&c->k);
		pc_mfree(c);
		return (NULL);
	}
	snprintf(c->algo, sizeof (c->algo), "%s", algo);
	c->level = level;
	c->len = cap;
	*slot = c;
	return (&c->k);
}

static void chunk_dict_free(pc_ctx_t *ctx);

/* Free the codec state and dictionary pc_chunk_seal()/pc_chunk_open() kept in ctx. */
void
chunk_codecs_free(pc_ctx_t *ctx)
{
	int i;

	chunk_dict_free(ctx);
	for (i = 0; i < 2; i++) {
		struct codec_cache *c = (struct codec_cache *)ctx->chunk_codec[i];

		if (c != NULL) {
			codec_close(&c->k);
			pc_mfree(c);
			ctx->chunk_codec[i] = NULL;
		}
	}
}

static void
codec_close(struct codec *k)
{
	if (k->deinit != NULL && k->data != NULL)
		k->deinit(&k->data);
}

static int
seal_err(pc_ctx_t *ctx, pc_error_t code, const char *msg)
{
	log_state_t *prev = log_bound();

	log_reset(&ctx->log);
	log_bind(&ctx->log);
	log_error_code(code);
	log_msg(LOG_ERR, 0, "%s", msg);
	log_bind(prev);
	return (-1);
}

/* --- Dictionaries ------------------------------------------------------- */

/*
 * The context's pc_set_dict() dictionary: its bytes and id, and zstd's
 * digested forms, made by pc_set_dict(). The compressing one is for one
 * level and remade when the level changes; the decompressing one serves
 * every level.
 */
struct chunk_dict {
	uint8_t id[32];
	void *cdict, *ddict;
	int clevel;		/* the level cdict was made for */
	size_t len;
	uchar_t bytes[];
};

/* A dictionary larger than this is refused (zstd's trainer makes ~100 KB). */
#define	PCK_DICT_MAX	(16 * 1024 * 1024)

/*
 * Chunks this size or larger are sealed without the dictionary, as plain
 * zstd objects. The digested dictionary carries match-finder tables
 * sized for small inputs, and zstd compresses the whole source with
 * them: a 4 MB chunk at level 12 came out 8% larger than without the
 * dictionary, and a 1 MB one at level 3 1% larger and 7% slower. Below
 * 128 KB it makes text 3 to 25% smaller, and on gear chunks (avg 64 KB)
 * the cutoff gives up less than 0.1% of that. Letting zstd size the
 * tables for each chunk (a CDict that keeps its level) digests the
 * dictionary again for every large chunk: zstd:3 on 1 MB chunks ran 20
 * to 40% slower than with no dictionary, for 0 to 3% smaller output.
 */
#define	PCK_DICT_CHUNK	(128 * 1024)

/* Free a dictionary pc_set_dict() made; NULL is ignored. */
static void
dict_destroy(struct chunk_dict *d)
{
	if (d == NULL)
		return;
#ifdef ENABLE_PC_ZSTD
	if (d->cdict != NULL)
		zstd_cdict_free(d->cdict);
	if (d->ddict != NULL)
		zstd_ddict_free(d->ddict);
#endif
	memset(d->bytes, 0, d->len);
	pc_mfree(d);
}

static void
chunk_dict_free(pc_ctx_t *ctx)
{
	dict_destroy((struct chunk_dict *)ctx->chunk_dict);
	ctx->chunk_dict = NULL;
}

int DLL_EXPORT
pc_dict_id(const void *dict, size_t len, uint8_t id[32])
{
	if (dict == NULL || len == 0 || id == NULL)
		return (-1);
	return (blake2b_keyed2(id, 32, NULL, 0, dict, len, NULL, 0));
}

int DLL_EXPORT
pc_dict_train(const void *samples, const size_t *sizes, unsigned n, void *dict,
    size_t *dictlen)
{
	if (samples == NULL || sizes == NULL || n == 0 || dict == NULL ||
	    dictlen == NULL || *dictlen == 0)
		return (-1);
#ifdef ENABLE_PC_ZSTD
	return (zstd_train_dict(dict, dictlen, samples, sizes, n));
#else
	return (-1);
#endif
}

int DLL_EXPORT
pc_set_dict(pc_ctx_t *ctx, const void *dict, size_t len)
{
#ifdef ENABLE_PC_ZSTD
	struct chunk_dict *d;
	int err;
#endif

	if (ctx == NULL)
		return (-1);
	if (dict == NULL && len == 0) {
		chunk_dict_free(ctx);
		return (0);
	}
#ifndef ENABLE_PC_ZSTD
	return (seal_err(ctx, PC_ERR_ARGS, "pc_set_dict: this build has no zstd"));
#else
	/* Make the new one completely before dropping the old: a failure keeps it. */
	if (dict == NULL || len == 0 || len > PCK_DICT_MAX)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_set_dict: bad dictionary size"));
	if ((d = pc_calloc(1, sizeof (*d) + len)) == NULL)
		return (seal_err(ctx, PC_ERR_NOMEM, "pc_set_dict: out of memory"));
	memcpy(d->bytes, dict, len);
	d->len = len;
	if (pc_dict_id(dict, len, d->id) != 0) {
		dict_destroy(d);
		return (seal_err(ctx, PC_ERR_INTERNAL, "pc_set_dict: hash failure"));
	}
	/* Digest it now: a dictionary zstd refuses fails here, not at seal or open. */
	d->clevel = ctx->level >= 1 ? ctx->level : 6;
	if ((err = zstd_dict_new(d->bytes, len, d->clevel, &d->cdict, &d->ddict)) != 0) {
		dict_destroy(d);
		return (seal_err(ctx, (pc_error_t)err, err == PC_ERR_NOMEM ?
		    "pc_set_dict: out of memory" : "pc_set_dict: malformed zstd dictionary"));
	}
	chunk_dict_free(ctx);
	ctx->chunk_dict = d;
	return (0);
#endif
}

#ifdef ENABLE_PC_ZSTD
/*
 * Compress in to payload with the context's dictionary: 0 and *clen set,
 * or -1 to store the chunk raw instead (too small to pay for the id, did
 * not shrink enough, or no memory for the digested dictionary). The
 * object grows by the 32-byte id, so the payload must be at least that
 * much smaller than the input: then pc_chunk_seal_bound() still holds.
 */
static int
dict_compress(pc_ctx_t *ctx, const void *in, size_t len, int level, uchar_t *payload,
    uint64_t *clen)
{
	struct chunk_dict *d = (struct chunk_dict *)ctx->chunk_dict;
	struct codec *k;

	if (len <= PCK_DICTID + 1)
		return (-1);
	if ((k = codec_get(ctx, "zstd", level, len, COMPRESS)) == NULL)
		return (-1);
	if (d->cdict == NULL || d->clevel != k->level) {
		if (d->cdict != NULL)
			zstd_cdict_free(d->cdict);
		d->clevel = k->level;
		if ((d->cdict = zstd_cdict_new(d->bytes, d->len, k->level)) == NULL)
			return (-1);
	}
	*clen = len - PCK_DICTID - 1;
	if (zstd_compress_dict((void *)in, len, payload, clen, k->level, d->cdict, k->data) != 0)
		return (-1);
	return (0);
}
#endif

static int chunk_seal(pc_ctx_t *, const void *, size_t, void *, size_t *);
static int chunk_open(pc_ctx_t *, const void *, size_t, void *, size_t *);

/* Codec messages (e.g. "output too small" before a raw fallback) go to the
 * context's log, not the process default. */
int DLL_EXPORT
pc_chunk_seal(pc_ctx_t *ctx, const void *in, size_t len, void *outp, size_t *outlen)
{
	log_state_t *prev;
	int rv;

	if (ctx == NULL)
		return (-1);
	prev = log_bound();
	log_bind(&ctx->log);
	rv = chunk_seal(ctx, in, len, outp, outlen);
	log_bind(prev);
	return (rv);
}

int DLL_EXPORT
pc_chunk_open(pc_ctx_t *ctx, const void *inp, size_t len, void *out, size_t *outlen)
{
	log_state_t *prev;
	int rv;

	if (ctx == NULL)
		return (-1);
	prev = log_bound();
	log_bind(&ctx->log);
	rv = chunk_open(ctx, inp, len, out, outlen);
	log_bind(prev);
	return (rv);
}

/*
 * The chi-square statistic against uniform of a byte histogram of n
 * bytes counted in four tables: 256 * sum(count^2) / n - n.
 */
static uint64_t
chi2_of(uint32_t count[4][256], uint64_t n)
{
	uint64_t sq = 0;
	int i;

	for (i = 0; i < 256; i++) {
		uint64_t c = (uint64_t)count[0][i] + count[1][i] + count[2][i] + count[3][i];

		sq += c * c;
	}
	return (sq * 256 / n - n);
}

/*
 * The chi-square statistic of the byte histogram of runs runs of
 * PCK_SAMPLE_RUN bytes spread evenly over p, against uniform. Random
 * bytes give 255 on average (standard deviation 23) whatever the sample
 * size; text and code give thousands to millions. With delta non-NULL,
 * *delta is the same statistic for the differences between neighbouring
 * bytes in each run (all but the first four): random bytes give 255
 * there too, while ramps, counters, permutation tables and smooth
 * signals, whose byte values can be spread evenly, give far more.
 */
#define	PCK_SAMPLE_RUN	128

static uint64_t
sample_chi2(const uchar_t *p, size_t len, int runs, uint64_t *delta)
{
	uint32_t count[4][256], dcount[4][256];
	size_t step = (len - PCK_SAMPLE_RUN) / (runs - 1);
	int r, i;

	memset(count, 0, sizeof (count));
	if (delta != NULL)
		memset(dcount, 0, sizeof (dcount));
	for (r = 0; r < runs; r++) {
		const uchar_t *q = p + r * step;

		/* Four tables: runs of one byte value would serialize on one counter. */
		for (i = 0; i < PCK_SAMPLE_RUN; i += 4) {
			count[0][q[i]]++;
			count[1][q[i + 1]]++;
			count[2][q[i + 2]]++;
			count[3][q[i + 3]]++;
		}
		for (i = 4; delta != NULL && i < PCK_SAMPLE_RUN; i += 4) {
			dcount[0][(uchar_t)(q[i] - q[i - 1])]++;
			dcount[1][(uchar_t)(q[i + 1] - q[i])]++;
			dcount[2][(uchar_t)(q[i + 2] - q[i + 1])]++;
			dcount[3][(uchar_t)(q[i + 3] - q[i + 2])]++;
		}
	}
	if (delta != NULL)
		*delta = chi2_of(dcount, (uint64_t)runs * (PCK_SAMPLE_RUN - 4));
	return (chi2_of(count, (uint64_t)runs * PCK_SAMPLE_RUN));
}

/*
 * Whether a chunk looks incompressible, so pc_chunk_seal() stores it raw
 * without running the codec. A 2 KB sample (16 runs) far from uniform
 * settles it cheaply for most compressible data. Otherwise each 256 KB
 * segment of the chunk (the whole chunk if smaller) is sampled with 64
 * runs, 8 KB, and every segment must score below 350 both on its bytes
 * and on the differences between neighbouring bytes. The second test
 * catches data with evenly spread byte values that still compresses:
 * 0..255 ramps, lookup tables, counters. Testing per segment keeps a
 * compressible region of a 4 MB chunk from hiding between the samples.
 * Measured on gear chunks (avg 64 KB) and on 1 MB and 4 MB pieces (EC2
 * Xeon 8488C), against what zstd -19 (pieces: -9) would have saved on
 * the chunks it skips:
 *
 *	random, H.264 video	88-100% skipped, nothing lost
 *	.gz/.xz/.zst files	77% of gear chunks skipped, 0.03% of the
 *				input lost; 42% / 9% of 1 / 4 MB, nothing lost
 *	PNG files		41% of gear chunks, 0.004% lost
 *	JPEG files		 2% of gear chunks, 0.01% lost
 *	tar of /usr/share/doc	13% of gear chunks (its .gz members), 0.004%
 *	enwik8, /usr/lib, /usr/bin, ramps, permutations, counters,
 *	dithered gradients, raw RGB test images	nothing skipped
 *	random with one 16 or 64 KB text region per 4 MB	nothing lost
 *
 * The worst chunk skipped would have shrunk 4.6%. Byte statistics cannot
 * see a repeated random block (each copy looks random): such a chunk is
 * stored raw where an LZ coder would have found the repeat. Chunks under
 * 16 KB are not tested; the sample would be most of them.
 */
#define	PCK_SAMPLE_MIN	(16 * 1024)
#define	PCK_SAMPLE_SEG	(256 * 1024)
#define	PCK_CHI2_QUICK	1000
#define	PCK_CHI2_RAW	350

static int
looks_incompressible(const uchar_t *p, size_t len)
{
	size_t nseg, seg, s;
	uint64_t delta;

	if (len < PCK_SAMPLE_MIN || sample_chi2(p, len, 16, NULL) >= PCK_CHI2_QUICK)
		return (0);
	nseg = (len + PCK_SAMPLE_SEG - 1) / PCK_SAMPLE_SEG;
	seg = len / nseg;
	for (s = 0; s < nseg; s++) {
		size_t n = s == nseg - 1 ? len - s * seg : seg;

		if (sample_chi2(p + s * seg, n, 64, &delta) >= PCK_CHI2_RAW ||
		    delta >= PCK_CHI2_RAW)
			return (0);
	}
	return (1);
}

/*
 * Compress in into the object at o for chunk_seal(): sets the payload
 * (at o + *hdr), *clen and the level byte o[7], and returns the flags;
 * *id becomes 0 when the chunk is stored raw, as it is when it does not
 * shrink or looks incompressible. With a dictionary set (zstd only),
 * chunks under PCK_DICT_CHUNK use it and the payload follows its id.
 */
static uchar_t
seal_payload(pc_ctx_t *ctx, const void *in, size_t len, uchar_t *o, uint64_t *clen,
    size_t *hdr, int *id)
{
	const char *algo = ctx->algo ? ctx->algo : "zstd";
	int level = ctx->level >= 1 ? ctx->level : 6;

	*hdr = PCK_HDR;
	*clen = 0;
	if (len == 0 || *id == 0 || looks_incompressible(in, len)) {
		/* Stored raw. */
#ifdef ENABLE_PC_ZSTD
	} else if (ctx->chunk_dict != NULL && len < PCK_DICT_CHUNK) {
		if (dict_compress(ctx, in, len, level, o + PCK_HDR + PCK_DICTID, clen) == 0) {
			memcpy(o + PCK_HDR, ((struct chunk_dict *)ctx->chunk_dict)->id, PCK_DICTID);
			*hdr = PCK_HDR + PCK_DICTID;
			o[7] = (uchar_t)level;
			return (PCK_DICT);
		}
#endif
	} else {
		struct codec *k = codec_get(ctx, algo, level, len, COMPRESS);
		int crv;

		/* Must not grow: else store raw. */
		*clen = len;
		if (k != NULL && (crv = k->comp((void *)in, len, o + PCK_HDR, clen, k->level, 0,
		    TYPE_UNKNOWN, k->data)) >= 0 && *clen > 0 && *clen < len) {
			/* adapt* return the sub-algorithm, kept in the level's high nibble. */
			o[7] = (uchar_t)(crv > 0 ? (level & 0x0f) | ((crv & 0x0f) << 4) : level);
			return (0);
		}
	}
	if (len > 0)
		memcpy(o + PCK_HDR, in, len);
	*clen = len;
	*id = 0;
	o[7] = 0;
	return (PCK_RAW);
}

static int
chunk_seal(pc_ctx_t *ctx, const void *in, size_t len, void *outp, size_t *outlen)
{
	uchar_t *o = (uchar_t *)outp, enc[32], mac[32];
	const char *algo = ctx->algo ? ctx->algo : "zstd";
	int id = strcmp(algo, "none") == 0 ? 0 : algo_id(algo);	/* none: raw */
	uint64_t clen = 0;
	size_t hdr;
	uchar_t flags, *payload;
	int rv = -1;

	if (in == NULL && len > 0)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: NULL input"));
	if (outp == NULL || outlen == NULL || len > SIZE_MAX - PCK_HDR - PCK_TAG ||
	    *outlen < pc_chunk_seal_bound(len))
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: output buffer too small"));
	if (id < 0)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: algorithm has no object id"));
	if (ctx->chunk_dict != NULL && id != PCK_ZSTD)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: a dictionary needs zstd"));
	if ((uint64_t)len > PCK_MAX_PLAIN)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: chunk larger than 4 GB"));
	log_reset(&ctx->log);
	flags = seal_payload(ctx, in, len, o, &clen, &hdr, &id);
	payload = o + hdr;

	memcpy(o, PCK_MAGIC, 4);
	o[4] = PCK_VERSION;
	o[6] = (uchar_t)id;
	put_le64(o + 8, len);
	memset(o + 16, 0, 24);
	if (ctx->chunk_key_set) {
		flags |= PCK_ENCRYPTED;
		if (geturandom_bytes(o + 16, 24) != 0) {
			memset(o, 0, hdr + clen);
			return (seal_err(ctx, PC_ERR_CRYPTO, "pc_chunk_seal: no random bytes"));
		}
		if (subkeys(ctx, enc, mac) != 0 ||
		    xsalsa20_xor(payload, payload, clen, o + 16, enc) != 0)
			goto out;
	}
	o[5] = flags;
	if (blake2b_keyed2(payload + clen, PCK_TAG, ctx->chunk_key_set ? mac : NULL,
	    ctx->chunk_key_set ? 32 : 0, o, hdr + clen, NULL, 0) != 0)
		goto out;
	*outlen = hdr + clen + PCK_TAG;
	rv = 0;
out:
	memset(enc, 0, sizeof (enc));
	memset(mac, 0, sizeof (mac));
	if (rv != 0)
		return (seal_err(ctx, PC_ERR_INTERNAL, "pc_chunk_seal: crypto failure"));
	return (0);
}

/* Bytes before the payload: the fixed header, and the dictionary id if any. */
static size_t
header_len(const uchar_t *p)
{
	return ((p[5] & PCK_DICT) ? PCK_HDR + PCK_DICTID : PCK_HDR);
}

/* Header checks shared by open() and open_size(). Returns plaintext size or -1. */
static int64_t
check_header(const uchar_t *p, size_t len)
{
	uint64_t plain;

	if (len < PCK_HDR + PCK_TAG || memcmp(p, PCK_MAGIC, 4) != 0 || p[4] != PCK_VERSION ||
	    (p[5] & ~PCK_FLAGS) != 0)
		return (-1);
	if ((p[5] & PCK_RAW) ? p[6] != 0 : (p[6] == 0 || p[6] >= sizeof (algo_ids) / sizeof (algo_ids[0]) - 1))
		return (-1);
	/* A dictionary object is zstd, never raw, and has room for the id. */
	if ((p[5] & PCK_DICT) && (p[6] != PCK_ZSTD || len < PCK_HDR + PCK_DICTID + PCK_TAG))
		return (-1);
	plain = get_le64(p + 8);
	if (plain > PCK_MAX_PLAIN)
		return (-1);
	if ((p[5] & PCK_RAW) && plain != len - PCK_HDR - PCK_TAG)
		return (-1);
	return ((int64_t)plain);
}

size_t DLL_EXPORT
pc_chunk_open_size(const void *obj, size_t len)
{
	int64_t n = (obj == NULL) ? -1 : check_header((const uchar_t *)obj, len);

	/* (size_t)-1 is the error value; on 32-bit, 4 GB does not fit either. */
	return (n < 0 || (uint64_t)n >= SIZE_MAX ? (size_t)-1 : (size_t)n);
}

/*
 * Decompress an authenticated, decrypted payload of clen bytes at src to
 * exactly plain bytes at out, for chunk_open(). 0, or -1 with *why and
 * *msg set.
 */
static int
open_payload(pc_ctx_t *ctx, const uchar_t *p, const uchar_t *src, uint64_t clen,
    uint64_t plain, void *out, pc_error_t *why, const char **msg)
{
	struct codec *k;
	uint64_t dl = plain;
	uchar_t lv = p[7] & 0x0f, chdr = p[7] & 0xf0;
	int drv;

	if ((k = codec_get(ctx, algo_ids[p[6]], lv ? lv : 6, plain, DECOMPRESS)) == NULL) {
		*why = PC_ERR_INTERNAL;
		*msg = "pc_chunk_open: algorithm not available in this build";
		return (-1);
	}
	if (p[5] & PCK_DICT) {
#ifdef ENABLE_PC_ZSTD
		struct chunk_dict *d = (struct chunk_dict *)ctx->chunk_dict;

		if (d == NULL || memcmp(d->id, p + PCK_HDR, PCK_DICTID) != 0) {
			*why = PC_ERR_DICT;
			*msg = d == NULL ? "pc_chunk_open: object needs a dictionary, none set" :
			    "pc_chunk_open: object needs a different dictionary";
			return (-1);
		}
		drv = zstd_decompress_dict((void *)src, clen, out, &dl, d->ddict, k->data);
#else
		drv = -1;	/* not reached: codec_get() has no zstd */
#endif
	} else {
		drv = k->decomp((void *)src, clen, out, &dl, k->level, chdr, TYPE_UNKNOWN, k->data);
	}
	if (drv < 0 || dl != plain)
		return (-1);	/* authenticated but undecodable: corrupt at seal time */
	return (0);
}

static int
chunk_open(pc_ctx_t *ctx, const void *inp, size_t len, void *out, size_t *outlen)
{
	const uchar_t *p = (const uchar_t *)inp;
	uchar_t tag[PCK_TAG], enc[32], mac[32], *buf = NULL;
	uint64_t clen = 0, plain;
	size_t hdr;
	int64_t n;
	int encrypted, rv = -1;
	pc_error_t why = PC_ERR_CORRUPT;
	const char *msg = "pc_chunk_open: corrupt object";

	if (inp == NULL || out == NULL || outlen == NULL)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_open: NULL argument"));
	if ((n = check_header(p, len)) < 0)
		return (seal_err(ctx, len >= 4 && memcmp(p, PCK_MAGIC, 4) == 0 ?
		    PC_ERR_CORRUPT : PC_ERR_FORMAT, "pc_chunk_open: not a valid chunk object"));
	plain = (uint64_t)n;
	if (*outlen < plain)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_open: output buffer too small"));
	encrypted = (p[5] & PCK_ENCRYPTED) != 0;
	if (encrypted && !ctx->chunk_key_set)
		return (seal_err(ctx, PC_ERR_CRYPTO, "pc_chunk_open: object is encrypted, no key set"));
	if (!encrypted && ctx->chunk_key_set)
		return (seal_err(ctx, PC_ERR_VERIFY, "pc_chunk_open: object is not encrypted but a key is set"));
	log_reset(&ctx->log);
	hdr = header_len(p);
	clen = len - hdr - PCK_TAG;

	/* 1. Verify the tag before touching the payload. */
	if (encrypted && subkeys(ctx, enc, mac) != 0)
		goto out;
	if (blake2b_keyed2(tag, PCK_TAG, encrypted ? mac : NULL, encrypted ? 32 : 0,
	    p, hdr + clen, NULL, 0) != 0)
		goto out;
	if (!ct_equal(tag, p + hdr + clen, PCK_TAG)) {
		why = PC_ERR_VERIFY;
		msg = encrypted ? "pc_chunk_open: authentication failed (wrong key or tampered)" :
		    "pc_chunk_open: checksum mismatch";
		goto out;
	}

	/* 2. Decrypt (into a scratch copy: the input is const). */
	if (encrypted) {
		if ((buf = pc_malloc(clen ? clen : 1)) == NULL) {
			why = PC_ERR_NOMEM;
			msg = "pc_chunk_open: out of memory";
			goto out;
		}
		if (xsalsa20_xor(buf, p + hdr, clen, p + 16, enc) != 0) {
			why = PC_ERR_INTERNAL;
			goto out;
		}
	}

	/* 3. Decompress to exactly the stated length. */
	if (p[5] & PCK_RAW)
		memcpy(out, encrypted ? buf : p + hdr, clen);
	else if (open_payload(ctx, p, encrypted ? buf : p + hdr, clen, plain, out, &why, &msg) != 0)
		goto out;
	*outlen = plain;
	rv = 0;
out:
	memset(enc, 0, sizeof (enc));
	memset(mac, 0, sizeof (mac));
	if (buf != NULL) {
		memset(buf, 0, clen);
		pc_mfree(buf);
	}
	if (rv != 0)
		return (seal_err(ctx, why, msg));
	return (0);
}

int DLL_EXPORT
pc_set_key(pc_ctx_t *ctx, const void *key, size_t keylen)
{
	memset(ctx->chunk_key, 0, sizeof (ctx->chunk_key));
	ctx->chunk_key_set = 0;
	if (key == NULL && keylen == 0)
		return (0);
	if (key == NULL || keylen != 32)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_set_key: key must be 32 bytes"));
	memcpy(ctx->chunk_key, key, 32);
	ctx->chunk_key_set = 1;
	return (0);
}

/* --- Key files ---------------------------------------------------------- */

/*
 * A key file holds a random 256-bit master key wrapped with a key derived
 * from a password of any length (ROADMAP Phase 4). 96 bytes, little-endian:
 *
 *   0  6  magic "PCKEY1"
 *   6  1  KDF id: 1 = scrypt
 *   7  1  log2 N (scrypt work factor)
 *   8  4  r
 *  12  4  p
 *  16 16  salt
 *  32 24  nonce
 *  56  8  reserved, 0
 *  64 32  the wrapped key (XSalsa20 with the first half of the KDF output)
 *  96 32  tag: BLAKE2b-256 keyed with a MAC key from the KDF, over [0, 96)
 *
 * (128 bytes in all.) The KDF output is 64 bytes: the first 32 encrypt,
 * the last 32 authenticate. A wrong password fails the tag. The KDF and
 * its parameters are in the file, so they can change without breaking old
 * files; open refuses parameters outside sane bounds (a crafted file must
 * not make it allocate gigabytes).
 */
#define	PCK_KF_MAGIC	"PCKEY1"
#define	PCK_KF_SIZE	128
#define	PCK_KF_SCRYPT	1
#define	PCK_KF_LOGN	17	/* 128 MB, about 0.3 s: interactive use */

static void
put_le32(uchar_t *p, uint32_t v)
{
	p[0] = (uchar_t)v; p[1] = (uchar_t)(v >> 8);
	p[2] = (uchar_t)(v >> 16); p[3] = (uchar_t)(v >> 24);
}

static uint32_t
get_le32(const uchar_t *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static int
kf_derive(const void *pw, size_t pwlen, const uchar_t *hdr, uchar_t dk[64])
{
	uint32_t logn = hdr[7], r = get_le32(hdr + 8), p = get_le32(hdr + 12);
	scrypt_params_t kp;

	/* Bounds checked before any work (scrypt_params_ok(): at most 512 MB). */
	kp.logn = (int)logn;
	kp.r = r;
	kp.p = p;
	if (hdr[6] != PCK_KF_SCRYPT || !scrypt_params_ok(&kp))
		return (-1);
	return (crypto_scrypt(pw, pwlen, hdr + 16, 16, (uint64_t)1 << logn, r, p, dk, 64));
}

size_t DLL_EXPORT
pc_keyfile_size(void)
{
	return (PCK_KF_SIZE);
}

int DLL_EXPORT
pc_keyfile_create(const void *pw, size_t pwlen, uint8_t key[32], void *out)
{
	uchar_t *o = (uchar_t *)out, dk[64];
	int rv = -1;

	if (pw == NULL || pwlen == 0 || key == NULL || out == NULL)
		return (-1);
	memset(o, 0, PCK_KF_SIZE);
	memcpy(o, PCK_KF_MAGIC, 6);
	o[6] = PCK_KF_SCRYPT;
	o[7] = PCK_KF_LOGN;
	put_le32(o + 8, 8);
	put_le32(o + 12, 1);
	if (geturandom_bytes(o + 16, 16 + 24) != 0 || geturandom_bytes(key, 32) != 0)
		goto out;
	if (kf_derive(pw, pwlen, o, dk) != 0 ||
	    xsalsa20_xor(o + 64, key, 32, o + 32, dk) != 0 ||
	    blake2b_keyed2(o + 96, 32, dk + 32, 32, o, 96, NULL, 0) != 0)
		goto out;
	rv = 0;
out:
	memset(dk, 0, sizeof (dk));
	if (rv != 0) {
		memset(o, 0, PCK_KF_SIZE);
		memset(key, 0, 32);
	}
	return (rv);
}

int DLL_EXPORT
pc_keyfile_open(const void *in, size_t len, const void *pw, size_t pwlen, uint8_t key[32])
{
	const uchar_t *p = (const uchar_t *)in;
	uchar_t dk[64], tag[32];
	int rv = -1;

	if (in == NULL || pw == NULL || key == NULL || len != PCK_KF_SIZE ||
	    memcmp(p, PCK_KF_MAGIC, 6) != 0 || get_le64(p + 56) != 0)
		return (-1);
	if (kf_derive(pw, pwlen, p, dk) != 0)
		goto out;
	if (blake2b_keyed2(tag, 32, dk + 32, 32, p, 96, NULL, 0) != 0 ||
	    !ct_equal(tag, p + 96, 32))
		goto out;	/* wrong password or modified file */
	if (xsalsa20_xor(key, p + 64, 32, p + 32, dk) != 0)
		goto out;
	rv = 0;
out:
	memset(dk, 0, sizeof (dk));
	if (rv != 0)
		memset(key, 0, 32);
	return (rv);
}

/* --- Persistent chunk-id index ----------------------------------------- */

/*
 * A set of 32-byte chunk ids kept in a file, so a backup tool can tell
 * "already stored" without asking its object store. The file is "PCIDX1",
 * two zero bytes, then 32-byte ids in the order they were added; it is
 * only ever appended to. On open the ids are loaded into a hash table; a
 * partial record at the end (a crash while appending) is cut off.
 *
 * Adds are buffered and written by pc_index_sync()/pc_index_close(), so a
 * crash can only lose entries, never add one: the tool then stores those
 * chunks again, which is safe. Add an id only after its object is stored.
 */
#define	PCI_MAGIC	"PCIDX1\0\0"
#define	PCI_HDR		8
#define	PCI_BUF		(2048 * 32)

/*
 * Only a hash table is in memory: per slot 32 bits of the id's hash and
 * its record number, 8 bytes, 3/8 to 3/4 full, so 11 to 21 bytes per id
 * (about 270 MB for 1 TB of 64 KB chunks). The ids stay in the file, or
 * in buf until written. A lookup whose hash matches reads the id back to
 * confirm it, so a false "present" (which would lose data) cannot happen,
 * and a present id costs one read. Records not yet written are numbered
 * PCI_BUFREC + their place in buf, and get their file record numbers when
 * written; with another handle appending too, those are only known then.
 *
 * shortcut: still O(ids) memory and an O(ids) read at open; past tens of
 * TB per repository a tool should keep its own on-disk catalogue.
 */
#define	PCI_BUFREC	0x80000000U
#define	PCI_MAXREC	0x7fffffffU	/* about 2 billion ids, 128 TB of 64 KB chunks */

struct pc_index {
	int fd;
	size_t n;		/* ids known to this handle */
	uint32_t *hash;		/* per slot; meaningful where rec != 0 */
	uint32_t *rec;		/* per slot: 0 empty, else record number + 1 */
	size_t nslots;		/* power of two, kept at most 3/4 full */
	uchar_t buf[PCI_BUF];	/* adds not yet written */
	size_t buflen;
	uint8_t probe[32];	/* an id read back from the file */
};

static uint32_t
pci_hash(const uint8_t id[32])
{
	uint64_t h = 0;
	int i;

	/* Ids are BLAKE2b output; mix anyway in case a caller uses others. */
	for (i = 0; i < 8; i++)
		h = (h << 8) | id[i];
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	return ((uint32_t)h);
}

/* The id of slot value r (record + 1): from buf, or read from the file. */
static const uint8_t *
pci_record(pc_index_t *x, uint32_t r)
{
	r--;
	if (r >= PCI_BUFREC)
		return (x->buf + (size_t)(r - PCI_BUFREC) * 32);
	if (lseek(x->fd, PCI_HDR + (off_t)r * 32, SEEK_SET) < 0 ||
	    Read(x->fd, x->probe, 32) != 32)
		return (NULL);
	return (x->probe);
}

/* 1 found, 0 not (*where: the free slot), -1 if reading an id failed. */
static int
pci_find(pc_index_t *x, const uint8_t id[32], uint32_t h, size_t *where)
{
	size_t mask = x->nslots - 1, s = (size_t)h & mask;

	for (; x->rec[s] != 0; s = (s + 1) & mask) {
		const uint8_t *p;

		if (x->hash[s] != h)
			continue;
		if ((p = pci_record(x, x->rec[s])) == NULL)
			return (-1);
		if (memcmp(p, id, 32) == 0) {
			*where = s;
			return (1);
		}
	}
	*where = s;
	return (0);
}

/* Double the table; the stored hashes place every slot again. */
static int
pci_grow(pc_index_t *x)
{
	size_t ns = x->nslots ? x->nslots * 2 : 1024, i, mask = ns - 1;
	uint32_t *h, *r;

	/* Up to 2^32 slots: the hash is 32 bits (PCI_MAXREC keeps it there). */
	if (ns > SIZE_MAX / sizeof (*h) || (h = pc_malloc(ns * sizeof (*h))) == NULL)
		return (-1);
	if ((r = pc_calloc(ns, sizeof (*r))) == NULL) {
		pc_mfree(h);
		return (-1);
	}
	for (i = 0; i < x->nslots; i++) {
		size_t s;

		if (x->rec[i] == 0)
			continue;
		for (s = (size_t)x->hash[i] & mask; r[s] != 0; s = (s + 1) & mask)
			;
		h[s] = x->hash[i];
		r[s] = x->rec[i];
	}
	pc_mfree(x->hash);
	pc_mfree(x->rec);
	x->hash = h;
	x->rec = r;
	x->nslots = ns;
	return (0);
}

/*
 * Insert into the table only, as record rec (a file record number, or
 * PCI_BUFREC + place in buf). 1 added, 0 already present, -1 on error.
 */
static int
pci_insert(pc_index_t *x, const uint8_t id[32], uint32_t rec)
{
	uint32_t h = pci_hash(id);
	size_t s;
	int r;

	if (x->n >= PCI_MAXREC || ((x->n + 1) * 4 > x->nslots * 3 && pci_grow(x) != 0))
		return (-1);
	if ((r = pci_find(x, id, h, &s)) != 0)
		return (r == 1 ? 0 : -1);
	x->hash[s] = h;
	x->rec[s] = rec + 1;
	x->n++;
	return (1);
}

/*
 * Write buf, then renumber its records to where they landed: O_APPEND
 * puts them at the end, after anything another handle appended.
 */
static int
pci_flush(pc_index_t *x)
{
	off_t end, after;
	size_t i, s;

	if (x->buflen == 0)
		return (0);
	if ((end = lseek(x->fd, 0, SEEK_END)) < 0)
		return (-1);
	if (Write(x->fd, x->buf, x->buflen) != (int64_t)x->buflen) {
		/* Undo a short write so later records stay aligned; keep the buffer. */
		int undone = ftruncate(x->fd, end);

		(void) undone;
		return (-1);
	}
	if ((after = lseek(x->fd, 0, SEEK_CUR)) < 0 ||
	    (uint64_t)(after - PCI_HDR) / 32 > PCI_MAXREC)
		return (-1);
	for (i = 0; i < x->buflen / 32; i++) {
		const uint8_t *id = x->buf + i * 32;

		if (pci_find(x, id, pci_hash(id), &s) != 1)
			return (-1);	/* cannot happen: it was added */
		x->rec[s] = (uint32_t)((after - PCI_HDR) / 32 - x->buflen / 32 + i) + 1;
	}
	x->buflen = 0;
	return (0);
}

pc_index_t DLL_EXPORT *
pc_index_open(const char *path)
{
	pc_index_t *x;
	uchar_t hdr[PCI_HDR];
	off_t sz, end, pos;

	if (path == NULL || (x = pc_calloc(1, sizeof (*x))) == NULL)
		return (NULL);
	/* O_APPEND: whole-record appends from two processes cannot overlap. */
	x->fd = -1;
	if ((x->fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0600)) == -1 || pci_grow(x) != 0)
		goto fail;
	sz = lseek(x->fd, 0, SEEK_END);
	if (sz == 0) {
		if (Write(x->fd, PCI_MAGIC, PCI_HDR) != PCI_HDR)
			goto fail;
		return (x);
	}
	if (sz < PCI_HDR || lseek(x->fd, 0, SEEK_SET) != 0 ||
	    Read(x->fd, hdr, PCI_HDR) != PCI_HDR || memcmp(hdr, PCI_MAGIC, PCI_HDR) != 0)
		goto fail;	/* not an index file: never overwrite it */
	end = PCI_HDR + ((sz - PCI_HDR) / 32) * 32;
	if ((uint64_t)(end - PCI_HDR) / 32 > PCI_MAXREC)
		goto fail;
	for (pos = PCI_HDR; pos < end; ) {
		size_t want = (size_t)(end - pos < PCI_BUF ? end - pos : PCI_BUF), i;
		uint32_t first = (uint32_t)((pos - PCI_HDR) / 32);

		/*
		 * Loaded through buf, numbered as file records; pci_find() may
		 * read a duplicate's earlier copy back, which moves the offset.
		 */
		if (lseek(x->fd, pos, SEEK_SET) != pos || Read(x->fd, x->buf, want) != (int64_t)want)
			goto fail;
		for (i = 0; i < want; i += 32)
			if (pci_insert(x, x->buf + i, first + (uint32_t)(i / 32)) < 0)
				goto fail;
		pos += want;
	}
	/* Drop a partial last record (a crash while appending). */
	if (end != sz && ftruncate(x->fd, end) != 0)
		goto fail;
	return (x);
fail:
	if (x->fd != -1)
		close(x->fd);
	pc_mfree(x->hash);
	pc_mfree(x->rec);
	pc_mfree(x);
	return (NULL);
}

int DLL_EXPORT
pc_index_has(pc_index_t *x, const uint8_t id[32])
{
	size_t s;

	if (x == NULL || id == NULL)
		return (-1);
	return (pci_find(x, id, pci_hash(id), &s));
}

int DLL_EXPORT
pc_index_add(pc_index_t *x, const uint8_t id[32])
{
	int r;

	if (x == NULL || id == NULL)
		return (-1);
	/* Make room first: a failed add must leave nothing half done. */
	if (x->buflen + 32 > PCI_BUF && pci_flush(x) != 0)
		return (-1);
	if ((r = pci_insert(x, id, PCI_BUFREC + (uint32_t)(x->buflen / 32))) != 1)
		return (r);
	memcpy(x->buf + x->buflen, id, 32);
	x->buflen += 32;
	return (1);
}

size_t DLL_EXPORT
pc_index_count(pc_index_t *x)
{
	return (x ? x->n : 0);
}

int DLL_EXPORT
pc_index_sync(pc_index_t *x)
{
	if (x == NULL || pci_flush(x) != 0 || fsync(x->fd) != 0)
		return (-1);
	return (0);
}

int DLL_EXPORT
pc_index_close(pc_index_t *x)
{
	int rv;

	if (x == NULL)
		return (0);
	rv = pc_index_sync(x);
	if (close(x->fd) != 0)
		rv = -1;
	pc_mfree(x->hash);
	pc_mfree(x->rec);
	pc_mfree(x);
	return (rv);
}
