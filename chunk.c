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
struct pc_chunker {
	const uint64_t *ir, *out;	/* rabin tables */
	size_t min, max;
	uint64_t mask;
	uint64_t roll;
	uchar_t win[RAB_POLYNOMIAL_WIN_SIZE];
	unsigned wpos;
	size_t cur;		/* bytes in the current, unfinished chunk */
};

#define	PC_CHUNK_AVG_DEFAULT	(64 * 1024)
#define	PC_CHUNK_MAX_LIMIT	(64UL * 1024 * 1024)

pc_chunker_t DLL_EXPORT *
pc_chunker_new(size_t avg, size_t min, size_t max)
{
	pc_chunker_t *c;
	size_t a = 1;

	if (avg == 0)
		avg = PC_CHUNK_AVG_DEFAULT;
	if (min == 0)
		min = avg / 4;
	if (max == 0)
		max = avg * 4;
	/* avg is rounded down to a power of two (it is a bit mask). */
	while (a * 2 <= avg)
		a *= 2;
	if (a < 256 || min < 64 || min > a || max < a || max > PC_CHUNK_MAX_LIMIT)
		return (NULL);
	if ((c = calloc(1, sizeof (*c))) == NULL)
		return (NULL);
	rabin_tables(&c->ir, &c->out);
	c->min = min;
	c->max = max;
	/* Expected gap past min is about avg - min; keep the mask at avg. */
	c->mask = a - 1;
	return (c);
}

void DLL_EXPORT
pc_chunker_free(pc_chunker_t *c)
{
	free(c);
}

static void
chunk_reset(pc_chunker_t *c)
{
	c->roll = 0;
	c->wpos = 0;
	c->cur = 0;
	memset(c->win, 0, sizeof (c->win));
}

int DLL_EXPORT
pc_chunker_feed(pc_chunker_t *c, const void *buf, size_t len, size_t *lens, int nlens)
{
	const uchar_t *p = (const uchar_t *)buf;
	size_t i;
	int n = 0;

	if (c == NULL || (buf == NULL && len > 0) || (lens == NULL && nlens > 0) || nlens < 0)
		return (-1);
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
	if ((buf == NULL && len > 0) || id == NULL || keylen > 64 || (key == NULL && keylen > 0))
		return (-1);
	return (blake2b_keyed2(id, 32, key, (int)keylen, buf, len, NULL, 0));
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
 *  40   n  payload: compressed (or raw) bytes, encrypted when PCK_ENCRYPTED
 *  40+n 32 tag: BLAKE2b-256 over bytes [0, 40+n), keyed with the MAC
 *          subkey when encrypted, unkeyed (an integrity check) otherwise
 *
 * Encrypt-then-MAC: the tag covers the header, so nothing in it can be
 * changed. open() checks the tag before it decrypts or decompresses.
 * Keys: enc = BLAKE2b-256(key, "pcompress chunk enc"),
 *       mac = BLAKE2b-256(key, "pcompress chunk mac").
 */
#define	PCK_MAGIC	"PCK1"
#define	PCK_VERSION	1
#define	PCK_HDR		40
#define	PCK_TAG		32
#define	PCK_ENCRYPTED	0x01
#define	PCK_RAW		0x02
#define	PCK_FLAGS	(PCK_ENCRYPTED | PCK_RAW)
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

static int
subkeys(pc_ctx_t *ctx, uchar_t enc[32], uchar_t mac[32])
{
	static const uchar_t e[] = "pcompress chunk enc", m[] = "pcompress chunk mac";

	if (blake2b_keyed2(enc, 32, ctx->chunk_key, 32, e, sizeof (e) - 1, NULL, 0) != 0 ||
	    blake2b_keyed2(mac, 32, ctx->chunk_key, 32, m, sizeof (m) - 1, NULL, 0) != 0)
		return (-1);
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

/* The context's algorithm handlers, set up for one call. */
struct codec {
	compress_func_ptr comp, decomp;
	init_func_ptr init;
	deinit_func_ptr deinit;
	void *data;
	int level;
};

extern int pc_init_algo_handlers(pc_ctx_t *pctx, const char *algo);

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

int DLL_EXPORT
pc_chunk_seal(pc_ctx_t *ctx, const void *in, size_t len, void *outp, size_t *outlen)
{
	uchar_t *o = (uchar_t *)outp, enc[32], mac[32];
	const char *algo = ctx->algo ? ctx->algo : "zstd";
	int id = strcmp(algo, "none") == 0 ? 0 : algo_id(algo);	/* none: raw */
	int level = ctx->level >= 1 ? ctx->level : 6;
	uint64_t clen = 0;
	uchar_t flags = 0, *payload;
	int rv = -1;

	if (in == NULL && len > 0)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: NULL input"));
	if (outp == NULL || outlen == NULL || *outlen < pc_chunk_seal_bound(len))
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: output buffer too small"));
	if (id < 0)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: algorithm has no object id"));
	if ((uint64_t)len > PCK_MAX_PLAIN)
		return (seal_err(ctx, PC_ERR_ARGS, "pc_chunk_seal: chunk larger than 4 GB"));
	log_reset(&ctx->log);
	payload = o + PCK_HDR;

	/* Compress into the payload area; fall back to raw if it does not shrink. */
	if (len > 0 && strcmp(algo, "none") != 0) {
		struct codec k;

		if (codec_open(ctx, algo, level, len, COMPRESS, &k) == 0) {
			int crv;

			clen = len;	/* must not grow: else store raw */
			crv = k.comp((void *)in, len, payload, &clen, k.level, 0, TYPE_UNKNOWN, k.data);
			codec_close(&k);
			if (crv < 0 || clen == 0 || clen >= len) {
				clen = 0;
			} else {
				/* adapt* return the sub-algorithm, needed to decompress. */
				o[7] = (uchar_t)level;
				if (crv > 0) {
					/* Keep it in the level byte's high nibble. */
					o[7] = (uchar_t)((level & 0x0f) | ((crv & 0x0f) << 4));
				}
			}
		}
	}
	if (clen == 0) {
		flags |= PCK_RAW;
		if (len > 0)
			memcpy(payload, in, len);
		clen = len;
		id = 0;
		o[7] = 0;
	}

	memcpy(o, PCK_MAGIC, 4);
	o[4] = PCK_VERSION;
	o[6] = (uchar_t)id;
	put_le64(o + 8, len);
	memset(o + 16, 0, 24);
	if (ctx->chunk_key_set) {
		flags |= PCK_ENCRYPTED;
		if (geturandom_bytes(o + 16, 24) != 0) {
			memset(o, 0, PCK_HDR + clen);
			return (seal_err(ctx, PC_ERR_CRYPTO, "pc_chunk_seal: no random bytes"));
		}
		if (subkeys(ctx, enc, mac) != 0 ||
		    xsalsa20_xor(payload, payload, clen, o + 16, enc) != 0)
			goto out;
	}
	o[5] = flags;
	if (blake2b_keyed2(payload + clen, PCK_TAG, ctx->chunk_key_set ? mac : NULL,
	    ctx->chunk_key_set ? 32 : 0, o, PCK_HDR + clen, NULL, 0) != 0)
		goto out;
	*outlen = PCK_HDR + clen + PCK_TAG;
	rv = 0;
out:
	memset(enc, 0, sizeof (enc));
	memset(mac, 0, sizeof (mac));
	if (rv != 0)
		return (seal_err(ctx, PC_ERR_INTERNAL, "pc_chunk_seal: crypto failure"));
	return (0);
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

	return (n < 0 ? (size_t)-1 : (size_t)n);
}

int DLL_EXPORT
pc_chunk_open(pc_ctx_t *ctx, const void *inp, size_t len, void *out, size_t *outlen)
{
	const uchar_t *p = (const uchar_t *)inp;
	uchar_t tag[PCK_TAG], enc[32], mac[32], *buf = NULL;
	uint64_t clen, plain;
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
	clen = len - PCK_HDR - PCK_TAG;

	/* 1. Verify the tag before touching the payload. */
	if (encrypted && subkeys(ctx, enc, mac) != 0)
		goto out;
	if (blake2b_keyed2(tag, PCK_TAG, encrypted ? mac : NULL, encrypted ? 32 : 0,
	    p, PCK_HDR + clen, NULL, 0) != 0)
		goto out;
	if (!ct_equal(tag, p + PCK_HDR + clen, PCK_TAG)) {
		why = PC_ERR_VERIFY;
		msg = encrypted ? "pc_chunk_open: authentication failed (wrong key or tampered)" :
		    "pc_chunk_open: checksum mismatch";
		goto out;
	}

	/* 2. Decrypt (into a scratch copy: the input is const). */
	if (encrypted) {
		if ((buf = malloc(clen ? clen : 1)) == NULL) {
			why = PC_ERR_NOMEM;
			msg = "pc_chunk_open: out of memory";
			goto out;
		}
		if (xsalsa20_xor(buf, p + PCK_HDR, clen, p + 16, enc) != 0) {
			why = PC_ERR_INTERNAL;
			goto out;
		}
	}

	/* 3. Decompress to exactly the stated length. */
	if (p[5] & PCK_RAW) {
		memcpy(out, encrypted ? buf : p + PCK_HDR, clen);
	} else {
		struct codec k;
		uint64_t dl = plain;
		uchar_t lv = p[7] & 0x0f, chdr = p[7] & 0xf0;
		int drv;

		if (codec_open(ctx, algo_ids[p[6]], lv ? lv : 6, plain, DECOMPRESS, &k) != 0) {
			why = PC_ERR_INTERNAL;
			msg = "pc_chunk_open: algorithm not available in this build";
			goto out;
		}
		drv = k.decomp(encrypted ? buf : (void *)(p + PCK_HDR), clen, out, &dl,
		    k.level, chdr, TYPE_UNKNOWN, k.data);
		codec_close(&k);
		if (drv < 0 || dl != plain)
			goto out;	/* authenticated but undecodable: corrupt at seal time */
	}
	*outlen = plain;
	rv = 0;
out:
	memset(enc, 0, sizeof (enc));
	memset(mac, 0, sizeof (mac));
	if (buf != NULL) {
		memset(buf, 0, clen);
		free(buf);
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
