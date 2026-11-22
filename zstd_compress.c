/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *
 */

#include <sys/types.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <limits.h>
#include <utils.h>
#include <pcompress.h>
#include <allocator.h>
/* ZSTD_customMem and the _advanced constructors: zstd is linked statically. */
#define	ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#include <zdict.h>

/*
 * Maximum chunk size for Zstandard.  ZSTD itself can handle very large
 * inputs, but we cap it to match the 2 GiB limit common across pcompress
 * algorithms.
 */
#define	ZSTD_MAX_CHUNK	2147450621ULL

/*
 * Map pcompress levels 1-14 to Zstandard compression levels 1-22.
 *
 *   pcompress 1-3  -> ZSTD 1-3   (fast, LZ4-competitive)
 *   pcompress 4-6  -> ZSTD 5-9   (balanced)
 *   pcompress 7-9  -> ZSTD 12-16 (high compression)
 *   pcompress 10-14 -> ZSTD 17-22 (ultra, LZMA-competitive)
 */
static int
map_level(int level)
{
	if (level <= 0) return 1;
	if (level <= 3) return level;
	if (level <= 6) return 3 + (level - 3) * 2;  /* 5, 7, 9 */
	if (level <= 9) return 10 + (level - 7) * 2;  /* 12, 14, 16 */
	if (level <= 14) return 16 + (level - 10);    /* 17, 18, 19, 20, 21 */
	return 22;
}

/*
 * Compute the optimal ZSTD_c_windowLog value for a given chunk size.
 * The window size should not exceed the chunk size, since data beyond
 * the chunk boundary is not available for back-references.  ZSTD
 * windowLog ranges from 10 (1 KB) to 31 (2 GB).
 */
static int
window_log_for_chunksize(uint64_t chunksize)
{
	int wlog = 10;

	while (wlog < 31 && ((uint64_t)1 << (wlog + 1)) <= chunksize)
		wlog++;
	return (wlog);
}

/*
 * Set when one of this thread's zstd allocations fails, so a zstd call
 * that returns only NULL can be told apart from one that refused its
 * input (zstd_dict_new()).
 */
static __thread int zstd_nomem;

/* zstd's allocations (contexts, worker pool, buffers) go through pc_malloc(). */
static void *
zstd_alloc(void *opaque, size_t n)
{
	void *p = pc_malloc(n);

	(void) opaque;
	if (p == NULL)
		zstd_nomem = 1;
	return (p);
}

static void
zstd_free(void *opaque, void *p)
{
	(void) opaque;
	pc_mfree(p);
}

static const ZSTD_customMem zstd_mem = { zstd_alloc, zstd_free, NULL };

struct zstd_params {
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
	int level;
	int nthreads;
	int window_log;
};

void
zstd_stats(int show)
{
}

int
zstd_buf_extra(uint64_t buflen)
{
	if (buflen > ZSTD_MAX_CHUNK)
		buflen = ZSTD_MAX_CHUNK;
	return ((int)(ZSTD_compressBound((size_t)buflen) - buflen));
}

void
zstd_props(algo_props_t *data, int level, uint64_t chunksize)
{
	data->compress_mt_capable = 1;
	data->decompress_mt_capable = 0;
	data->buf_extra = zstd_buf_extra(chunksize);
	data->c_max_threads = 8;
	data->delta2_span = 100;
	if (level < 7)
		data->deltac_min_distance = FOURM;
	else if (level < 10)
		data->deltac_min_distance = (EIGHTM * 4);
	else
		data->deltac_min_distance = (EIGHTM * 16);
}

int
zstd_init(void **data, int *level, int nthreads, uint64_t chunksize,
	  int file_version, compress_op_t op)
{
	struct zstd_params *zdat;

	if (chunksize > ZSTD_MAX_CHUNK) {
		log_msg(LOG_ERR, 0, "Max allowed chunk size for ZSTD is: %llu \n",
		    (unsigned long long)ZSTD_MAX_CHUNK);
		return (1);
	}

	zdat = (struct zstd_params *)slab_alloc(NULL, sizeof (struct zstd_params));
	if (!zdat) {
		log_msg(LOG_ERR, 0, "ZSTD: Memory allocation error\n");
		return (1);
	}

	zdat->cctx = NULL;
	zdat->dctx = NULL;
	zdat->nthreads = nthreads;
	zdat->window_log = window_log_for_chunksize(chunksize);

	if (op == COMPRESS) {
		zdat->cctx = ZSTD_createCCtx_advanced(zstd_mem);
		if (!zdat->cctx) {
			log_msg(LOG_ERR, 0, "ZSTD: Failed to create compression context\n");
			slab_free(NULL, zdat);
			return (1);
		}

		/*
		 * Configure persistent parameters on the context once.
		 * Multi-threading: ZSTD manages its own thread pool via
		 * nbWorkers.  Window log: sized to the chunk so the
		 * dictionary window doesn't exceed available data.
		 */
		if (nthreads > 1)
			ZSTD_CCtx_setParameter(zdat->cctx, ZSTD_c_nbWorkers,
			    nthreads);
		ZSTD_CCtx_setParameter(zdat->cctx, ZSTD_c_windowLog,
		    zdat->window_log);
	} else {
		zdat->dctx = ZSTD_createDCtx_advanced(zstd_mem);
		if (!zdat->dctx) {
			log_msg(LOG_ERR, 0, "ZSTD: Failed to create decompression context\n");
			slab_free(NULL, zdat);
			return (1);
		}
	}

	if (*level > 14) *level = 14;
	zdat->level = *level;
	*data = zdat;
	return (0);
}

int
zstd_deinit(void **data)
{
	struct zstd_params *zdat = (struct zstd_params *)(*data);

	if (zdat) {
		if (zdat->cctx)
			ZSTD_freeCCtx(zdat->cctx);
		if (zdat->dctx)
			ZSTD_freeDCtx(zdat->dctx);
		slab_free(NULL, zdat);
	}
	*data = NULL;
	return (0);
}

int
zstd_compress(void *src, uint64_t srclen, void *dst, uint64_t *dstlen,
	      int level, uchar_t chdr, int btype, void *data)
{
	struct zstd_params *zdat = (struct zstd_params *)data;
	size_t rv;
	int zstd_level;

	zstd_level = map_level(level);

	/*
	 * Reset only the session state; persistent parameters (nbWorkers,
	 * windowLog) set during init are preserved.
	 */
	ZSTD_CCtx_reset(zdat->cctx, ZSTD_reset_session_only);
	ZSTD_CCtx_setParameter(zdat->cctx, ZSTD_c_compressionLevel, zstd_level);

	rv = ZSTD_compress2(zdat->cctx, dst, (size_t)*dstlen,
	    src, (size_t)srclen);

	if (ZSTD_isError(rv)) {
		/*
		 * dst is sized to the input: "does not fit" just means the
		 * chunk is incompressible and is stored raw. Not an error.
		 */
		if (ZSTD_getErrorCode(rv) != ZSTD_error_dstSize_tooSmall)
			log_msg(LOG_ERR, 0, "ZSTD compress error: %s\n",
			    ZSTD_getErrorName(rv));
		return (-1);
	}

	*dstlen = rv;
	return (0);
}

int
zstd_decompress(void *src, uint64_t srclen, void *dst, uint64_t *dstlen,
		int level, uchar_t chdr, int btype, void *data)
{
	struct zstd_params *zdat = (struct zstd_params *)data;
	size_t rv;

	rv = ZSTD_decompressDCtx(zdat->dctx, dst, (size_t)*dstlen,
	    src, (size_t)srclen);

	if (ZSTD_isError(rv)) {
		log_msg(LOG_ERR, 0, "ZSTD decompress error: %s\n",
		    ZSTD_getErrorName(rv));
		return (-1);
	}

	*dstlen = rv;
	return (0);
}

/*
 * Dictionaries for sealed chunk objects (chunk.c). A digested dictionary
 * for compressing is made for one pcompress level; one for decompressing
 * serves every level. Both copy the dictionary bytes, so the caller's
 * buffer need not outlive them.
 */
void *
zstd_cdict_new(const void *dict, size_t len, int level)
{
	ZSTD_compressionParameters cp = ZSTD_getCParams(map_level(level), 0, len);

	return (ZSTD_createCDict_advanced(dict, len, ZSTD_dlm_byCopy, ZSTD_dct_auto,
	    cp, zstd_mem));
}

/*
 * Digest dict both ways: for compressing at pcompress level, and for
 * decompressing. 0 with *cdict and *ddict set; PC_ERR_NOMEM; or
 * PC_ERR_DICT when zstd refuses the bytes, which happens only to a
 * zstd-format dictionary (it starts with zstd's dictionary magic) whose
 * entropy tables do not parse. Bytes without the magic are taken as raw
 * content and always work. Neither half of a failure is kept.
 */
int
zstd_dict_new(const void *dict, size_t len, int level, void **cdict, void **ddict)
{
	zstd_nomem = 0;
	*ddict = ZSTD_createDDict_advanced(dict, len, ZSTD_dlm_byCopy, ZSTD_dct_auto,
	    zstd_mem);
	*cdict = *ddict == NULL ? NULL : zstd_cdict_new(dict, len, level);
	if (*cdict != NULL)
		return (0);
	ZSTD_freeDDict((ZSTD_DDict *)*ddict);
	*ddict = NULL;
	return (zstd_nomem ? PC_ERR_NOMEM : PC_ERR_DICT);
}

void
zstd_cdict_free(void *cdict)
{
	ZSTD_freeCDict((ZSTD_CDict *)cdict);
}

void
zstd_ddict_free(void *ddict)
{
	ZSTD_freeDDict((ZSTD_DDict *)ddict);
}

/*
 * zstd_compress() with a digested dictionary made for the same level;
 * data is a COMPRESS state from zstd_init(). The frame does not record
 * zstd's dictionary id (4 bytes saved): the chunk object names its
 * dictionary itself. The dictionary is dropped from the context again
 * before returning, so a later zstd_compress() on it does not use one.
 */
int
zstd_compress_dict(void *src, uint64_t srclen, void *dst, uint64_t *dstlen,
    int level, void *cdict, void *data)
{
	struct zstd_params *zdat = (struct zstd_params *)data;
	size_t rv;

	ZSTD_CCtx_reset(zdat->cctx, ZSTD_reset_session_only);
	ZSTD_CCtx_setParameter(zdat->cctx, ZSTD_c_compressionLevel, map_level(level));
	ZSTD_CCtx_setParameter(zdat->cctx, ZSTD_c_dictIDFlag, 0);
	ZSTD_CCtx_refCDict(zdat->cctx, (const ZSTD_CDict *)cdict);
	rv = ZSTD_compress2(zdat->cctx, dst, (size_t)*dstlen, src, (size_t)srclen);
	ZSTD_CCtx_refCDict(zdat->cctx, NULL);
	if (ZSTD_isError(rv))
		return (-1);	/* as in zstd_compress(): the caller stores it raw */
	*dstlen = rv;
	return (0);
}

/* zstd_decompress() with a digested dictionary; data is a DECOMPRESS state. */
int
zstd_decompress_dict(void *src, uint64_t srclen, void *dst, uint64_t *dstlen,
    void *ddict, void *data)
{
	struct zstd_params *zdat = (struct zstd_params *)data;
	size_t rv;

	rv = ZSTD_decompress_usingDDict(zdat->dctx, dst, (size_t)*dstlen, src,
	    (size_t)srclen, (const ZSTD_DDict *)ddict);
	if (ZSTD_isError(rv)) {
		log_msg(LOG_ERR, 0, "ZSTD decompress error: %s\n", ZSTD_getErrorName(rv));
		return (-1);
	}
	*dstlen = rv;
	return (0);
}

/*
 * Train a dictionary of at most *len bytes into dict from n samples laid
 * end to end in samples, sizes[i] bytes each; *len is set to its size.
 * 0, or -1 if zstd cannot make one (too few or too small samples).
 * NB: the trainer's working memory comes from the C library, not
 * pc_set_allocator()'s functions: zstd's dictBuilder calls malloc().
 */
int
zstd_train_dict(void *dict, size_t *len, const void *samples, const size_t *sizes,
    unsigned n)
{
	size_t rv = ZDICT_trainFromBuffer(dict, *len, samples, sizes, n);

	if (ZDICT_isError(rv))
		return (-1);
	*len = rv;
	return (0);
}
