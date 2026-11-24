/*
 * The rabin polynomial computation is derived from:
 * http://code.google.com/p/rabin-fingerprint-c/
 *
 * originally created by Joel Lawrence Tucci on 09-March-2011.
 *
 * Rabin polynomial portions Copyright (c) 2011 Joel Lawrence Tucci
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of the project's author nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

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

#ifndef __STDC_FORMAT_MACROS
#define	__STDC_FORMAT_MACROS	1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <allocator.h>
#include <utils.h>
#include <pthread.h>
#include <heap.h>
#include "../utils/xxhash.h"

#define	QSORT_LT(a, b)	((*a)<(*b))
#define	QSORT_TYPE pc_u64u_t	/* sorted arrays are at arbitrary byte offsets */
#include <qsort.h>

#include "rabin_dedup.h"

/*
 * Scratch arrays are carved from the end of the output buffer. Round the
 * start down to 8 bytes: they hold pointers and 64-bit values, and
 * real_chunksize is arbitrary (SIGBUS on strict-alignment CPUs).
 */
#define	SCRATCH_TAIL(ctx, sz) \
	((uchar_t *)(((uintptr_t)((ctx)->cbuf + (ctx)->real_chunksize - (sz))) & ~(uintptr_t)7))
#if defined(__USE_SSE_INTRIN__)
#	include <emmintrin.h>
#endif

#if defined(_OPENMP)
#include <omp.h>
#endif

#define	DELTA_EXTRA2_PCT(x) ((x) >> 1)
#define	DELTA_EXTRA_PCT(x) (((x) >> 1) + ((x) >> 3))
#define	DELTA_NORMAL_PCT(x) (((x) >> 1) + ((x) >> 2) + ((x) >> 3))

extern int lzma_init(void **data, int *level, int nthreads, int64_t chunksize,
		     int file_version, compress_op_t op);
extern int lzma_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *destlen, int level, uchar_t chdr, void *data);
extern int lzma_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, void *data);
extern int lzma_deinit(void **data);
extern int bsdiff(u_char *oldbuf, bsize_t oldsize, u_char *newbuf, bsize_t newsize,
       u_char *diff, u_char *scratch, bsize_t scratchsize);
extern bsize_t get_bsdiff_sz(u_char *pbuf, uint64_t avail);
extern int bspatch(u_char *pbuf, u_char *oldbuf, bsize_t oldsize, u_char *newbuf,
	bsize_t *_newsize);

static uint64_t ir[256], out[256];
static uint64_t gear[256];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;
static void init_rabin_tables(void);

/* The polynomial tables, for other users of the rolling hash (chunk.c). */
void
rabin_tables(const uint64_t **ir_tab, const uint64_t **out_tab)
{
	pthread_once(&tables_once, init_rabin_tables);
	*ir_tab = ir;
	*out_tab = out;
}

/* Irreducible-polynomial tables: pure functions of constants, built once. */
static void
init_rabin_tables(void)
{
	unsigned int term, pow, i, j;
	uint64_t val, poly_pow;

	pc_gear_table(gear);
	poly_pow = 1;
	for (j = 0; j < RAB_POLYNOMIAL_WIN_SIZE; j++)
		poly_pow = (poly_pow * RAB_POLYNOMIAL_CONST) & POLY_MASK;
	for (j = 0; j < 256; j++) {
		term = 1;
		pow = 1;
		val = 1;
		out[j] = (j * poly_pow) & POLY_MASK;
		for (i = 0; i < RAB_POLYNOMIAL_WIN_SIZE; i++) {
			if (term & FP_POLY)
				val += ((pow * j) & POLY_MASK);
			pow = (pow * RAB_POLYNOMIAL_CONST) & POLY_MASK;
			term <<= 1;
		}
		ir[j] = val;
	}
}

static uint32_t
dedupe_min_blksz(int rab_blk_sz)
{
	uint32_t min_blk;

	min_blk = (1 << (rab_blk_sz + RAB_BLK_MIN_BITS)) - 1024;
	return (min_blk);
}

uint32_t
dedupe_buf_extra(uint64_t chunksize, int rab_blk_sz, const char *algo, int delta_flag)
{
	if (rab_blk_sz < 0 || rab_blk_sz > 5)
		rab_blk_sz = RAB_BLK_DEFAULT;

	return ((chunksize / dedupe_min_blksz(rab_blk_sz)) * sizeof (uint32_t));
}

/*
 * Helper function to let caller size the the user specific compression chunk/segment
 * to align with deduplication requirements.
 */
int
global_dedupe_bufadjust(uint32_t rab_blk_sz, uint64_t *user_chunk_sz, int pct_interval,
		 const char *algo, cksum_t ck, cksum_t ck_sim, size_t file_sz,
		 size_t memlimit, int nthreads, int pipe_mode)
{
	uint64_t memreqd;
	archive_config_t cfg;
	int rv, pct_i, hash_entry_size;
	uint32_t hash_slots;

	rv = 0;
	pct_i = pct_interval;
	if (pipe_mode && pct_i == 0)
		pct_i = DEFAULT_PCT_INTERVAL;

	rv = setup_db_config_s(&cfg, rab_blk_sz, user_chunk_sz, &pct_i, algo, ck, ck_sim,
		 file_sz, &hash_slots, &hash_entry_size, &memreqd, memlimit, "/tmp");
	return (rv);
}

/*
 * Initialize the algorithm with the default params.
 */
dedupe_context_t *
create_dedupe_context(uint64_t chunksize, uint64_t real_chunksize, int rab_blk_sz,
    const char *algo, const algo_props_t *props, int delta_flag, int dedupe_flag,
    int file_version, compress_op_t op, uint64_t file_size, char *tmppath,
    int pipe_mode, int nthreads, size_t freeram, archive_config_t **shared_arc) {
	dedupe_context_t *ctx;
	archive_config_t *arc;

	if (rab_blk_sz < 0 || rab_blk_sz > 5)
		rab_blk_sz = RAB_BLK_DEFAULT;

	if (dedupe_flag == RABIN_DEDUPE_FIXED || dedupe_flag == RABIN_DEDUPE_FILE_GLOBAL)
		delta_flag = 0;

	pthread_once(&tables_once, init_rabin_tables);

	/*
	 * Global Deduplication: one in-memory index per job, shared by that
	 * job's worker contexts through *shared_arc (created by the first
	 * worker, freed by the job via destroy_dedupe_index()).
	 */
	arc = NULL;
	if (dedupe_flag == RABIN_DEDUPE_FILE_GLOBAL && op == COMPRESS && shared_arc != NULL) {
		if (*shared_arc == NULL) {
			int pct_interval, chunk_cksum, cksum_bytes, mac_bytes;
			char *ck;

			pct_interval = 0;
			if (pipe_mode)
				pct_interval = DEFAULT_PCT_INTERVAL;

			chunk_cksum = 0;
			if ((ck = getenv("PCOMPRESS_CHUNK_HASH_GLOBAL")) != NULL) {
				if (get_checksum_props(ck, &chunk_cksum, &cksum_bytes, &mac_bytes, 1) != 0 ||
				    strcmp(ck, "CRC64") == 0) {
					log_msg(LOG_ERR, 0, "Invalid PCOMPRESS_CHUNK_HASH_GLOBAL.\n");
					return (NULL);
				}
			}
			if (chunk_cksum == 0) {
				chunk_cksum = DEFAULT_CHUNK_CKSUM;
				if (get_checksum_props(NULL, &chunk_cksum, &cksum_bytes, &mac_bytes, 0) != 0) {
					log_msg(LOG_ERR, 0, "Invalid default chunk checksum: %d\n", DEFAULT_CHUNK_CKSUM);
					return (NULL);
				}
			}
			*shared_arc = init_global_db_s(NULL, tmppath, rab_blk_sz, chunksize, pct_interval,
			    algo, chunk_cksum, GLOBAL_SIM_CKSUM, file_size, freeram, nthreads);
			if (*shared_arc == NULL)
				return (NULL);
		}
		arc = *shared_arc;
	}

	/*
	 * Rabin window size must be power of 2 for optimization.
	 */
	if (!ISP2(RAB_POLYNOMIAL_WIN_SIZE)) {
		log_msg(LOG_ERR, 0, "Rabin window size must be a power of 2 in range 4 <= x <= 64\n");
		return (NULL);
	}

	if (arc) {
		if (chunksize < RAB_MIN_CHUNK_SIZE_GLOBAL) {
			log_msg(LOG_ERR, 0, "Minimum chunk size for Global Dedup must be %" PRIu64 " bytes\n",
			RAB_MIN_CHUNK_SIZE_GLOBAL);
			return (NULL);
		}
	} else {
		if (chunksize < RAB_MIN_CHUNK_SIZE) {
			log_msg(LOG_ERR, 0, "Minimum chunk size for Dedup must be %" PRIu64 " bytes\n",
			RAB_MIN_CHUNK_SIZE);
			return (NULL);
		}
	}

	/*
	 * For LZMA with chunksize <= LZMA Window size and/or Delta enabled we
	 * use 4K minimum Rabin block size. For everything else it is 2K based
	 * on experimentation.
	 */
	ctx = (dedupe_context_t *)slab_alloc(NULL, sizeof (dedupe_context_t));
	ctx->rabin_poly_max_block_size = RAB_POLYNOMIAL_MAX_BLOCK_SIZE;
	ctx->arc = arc;
	ctx->out_fd = -1;	/* set by the decompressor for global dedupe */

	ctx->dedupe_flag = dedupe_flag;
	ctx->rabin_poly_avg_block_size = RAB_BLK_AVG_SZ(rab_blk_sz);
	ctx->rabin_poly_min_block_size = dedupe_min_blksz(rab_blk_sz);
	ctx->delta_flag = 0;
	ctx->deltac_min_distance = props->deltac_min_distance;
	ctx->pagesize = sysconf(_SC_PAGE_SIZE);
	ctx->similarity_cksums = NULL;
	ctx->show_chunks = 0;
	if (arc) {
		arc->pagesize = ctx->pagesize;
		if (rab_blk_sz < 3)
			ctx->rabin_poly_max_block_size = RAB_POLY_MAX_BLOCK_SIZE_GLOBAL;
	}

	/*
	 * Scale down similarity percentage based on avg block size unless user specified
	 * argument '-EE' in which case fixed 40% match is used for Delta compression.
	 */
	if (delta_flag == DELTA_NORMAL) {
		if (ctx->rabin_poly_avg_block_size < (1 << 14)) {
			ctx->delta_flag = 1;
		} else if (ctx->rabin_poly_avg_block_size < (1 << 16)) {
			ctx->delta_flag = 2;
		} else {
			ctx->delta_flag = 3;
		}
	} else if (delta_flag == DELTA_EXTRA) {
		ctx->delta_flag = 2;
	}

	if (dedupe_flag != RABIN_DEDUPE_FIXED)
		ctx->blknum = chunksize / ctx->rabin_poly_min_block_size;
	else
		ctx->blknum = chunksize / ctx->rabin_poly_avg_block_size;

	if (chunksize % ctx->rabin_poly_min_block_size)
		++(ctx->blknum);

	if (ctx->blknum > RABIN_MAX_BLOCKS) {
		log_msg(LOG_ERR, 0, "Chunk size too large for dedup.\n");
		destroy_dedupe_context(ctx);
		return (NULL);
	}
	ctx->blocks = NULL;
	if (real_chunksize > 0 && dedupe_flag != RABIN_DEDUPE_FILE_GLOBAL) {
		ctx->blocks = (rabin_blockentry_t **)slab_calloc(NULL,
			ctx->blknum, sizeof (rabin_blockentry_t *));
	}
	if (ctx == NULL ||
	    (ctx->blocks == NULL && real_chunksize > 0 && dedupe_flag != RABIN_DEDUPE_FILE_GLOBAL)) {
		log_msg(LOG_ERR, 0,
		    "Could not allocate rabin polynomial context, out of memory\n");
		destroy_dedupe_context(ctx);
		return (NULL);
	}

	if (arc && dedupe_flag == RABIN_DEDUPE_FILE_GLOBAL) {
		ctx->similarity_cksums = (uchar_t *)slab_calloc(NULL,
					arc->sub_intervals,
					arc->similarity_cksum_sz);
		if (!ctx->similarity_cksums) {
			log_msg(LOG_ERR, 0,
			    "Could not allocate dedupe context, out of memory\n");
			destroy_dedupe_context(ctx);
			return (NULL);
		}
	}

	ctx->lzma_data = NULL;
	ctx->level = 14;
	if (real_chunksize > 0) {
		lzma_init(&(ctx->lzma_data), &(ctx->level), 1, chunksize, file_version, op);

		// The lzma_data member is not needed during decompression
		if (!(ctx->lzma_data) && op == COMPRESS) {
			log_msg(LOG_ERR, 0,
			    "Could not initialize LZMA data for dedupe index, out of memory\n");
			destroy_dedupe_context(ctx);
			return (NULL);
		}
	}

	slab_cache_add(sizeof (rabin_blockentry_t));
	ctx->real_chunksize = real_chunksize;
	reset_dedupe_context(ctx);
	return (ctx);
}

void
reset_dedupe_context(dedupe_context_t *ctx)
{
	ctx->valid = 0;
}

/* Free a job's global-dedupe index (see create_dedupe_context()). */
void
destroy_dedupe_index(archive_config_t **shared_arc)
{
	if (shared_arc != NULL && *shared_arc != NULL) {
		destroy_global_db_s(*shared_arc);
		*shared_arc = NULL;
	}
}

void
destroy_dedupe_context(dedupe_context_t *ctx)
{
	if (ctx) {
		uint32_t i;

		if (ctx->blocks) {
			for (i=0; i<ctx->blknum && ctx->blocks[i] != NULL; i++) {
				slab_free(NULL, ctx->blocks[i]);
			}
			slab_free(NULL, ctx->blocks);
		}
		if (ctx->similarity_cksums) slab_free(NULL, ctx->similarity_cksums);
		if (ctx->lzma_data) lzma_deinit(&(ctx->lzma_data));
		slab_free(NULL, ctx);
	}
}

/*
 * Simple insertion sort of integers. Used for sorting a small number of items to
 * avoid overheads of qsort() with callback function.
 */
static void
isort_uint64(pc_u64u_t *ary, uint32_t nitems)
{
	uint32_t i, j, k;
	uint64_t tmp;

	for (i = 1 ; i < nitems; i++) {
		for (j = 0 ; j < i ; j++) {
			if (ary[j] > ary[i]) {
				tmp = ary[j] ;
				ary[j] = ary[i] ;

				for (k = i ; k > j ; k--)
					ary[k] = ary[k - 1] ;

				ary[k + 1] = tmp ;
			}
		}
	}
}

/*
 * Sort an array of 64-bit unsigned integers. The QSORT macro provides an
 * inline quicksort routine that does not use a callback function.
 */
static void
do_qsort(pc_u64u_t *arr, uint32_t len)
{
	QSORT(arr, len);
}

static inline int
ckcmp(uchar_t *a, uchar_t *b, int sz)
{
	/* Checksums sit at arbitrary byte offsets. */
	const pc_u64u_t *v1 = (const pc_u64u_t *)a;
	const pc_u64u_t *v2 = (const pc_u64u_t *)b;
	int len;

	len = 0;
	do {
		if (*v1 != *v2) {
			return (1);
		}
		++v1;
		++v2;
		len += sizeof (*v1);
	} while (len < sz);

	return (0);
}

/*
 * The length of the block that starts at p, of the len bytes left: the
 * first cut at or past min bytes, else max, else len. A cut follows the
 * byte after which the top RAB_GEAR_BITS bits of chunk.c's gear hash
 * (FastCDC's, table from pc_gear_table()) are zero, so blocks average
 * min + 1 KB like the 10-bit Rabin test this replaced. FastCDC's
 * normalized chunking (a stricter test before the average, a looser one
 * after) was measured and lost dedupe: -G on five PostgreSQL source trees
 * deduped 11% worse. Only the 64 bytes before a candidate cut reach the
 * top bits of h, so hashing starts 64 bytes before min. The decoder reads
 * block lengths, so these cuts are not part of the format.
 */
static uint32_t
gear_cut(const dedupe_context_t *ctx, const uchar_t *p, uint64_t len)
{
	const uint64_t mask = ~(uint64_t)0 << (64 - RAB_GEAR_BITS);
	uint64_t h = 0;
	uint32_t i, min = ctx->rabin_poly_min_block_size;
	uint32_t n = len < ctx->rabin_poly_max_block_size ? len :
	    ctx->rabin_poly_max_block_size;

	if (n <= min)
		return (n);
	for (i = min - RAB_WINDOW_SLIDE_OFFSET; i < min - 1; i++)
		h = (h << 1) + gear[p[i]];
	for (; i < n; i++) {
		h = (h << 1) + gear[p[i]];
		if ((h & mask) == 0)
			return (i + 1);
	}
	return (n);
}

/**
 * Perform Deduplication.
 * Both content-defined (gear hash, gear_cut()) and Fixed Block Deduplication
 * are supported. Content-defined blocks vary in size from min to 128K.
 */
uint32_t
dedupe_compress(dedupe_context_t *ctx, uchar_t *buf, uint64_t *size, uint64_t offset,
		uint64_t *rabin_pos, int mt)
{
	uint64_t i, last_offset, j, ary_sz;
	uint32_t blknum;
	uchar_t *buf1 = (uchar_t *)buf;
	uint32_t length;
	uint32_t *ctx_heap;
	rabin_blockentry_t **htab;
	MinHeap heap;
	DEBUG_STAT_EN(uint32_t max_count);
	DEBUG_STAT_EN(max_count = 0);
	DEBUG_STAT_EN(double strt, en_1, en);

	length = offset;
	last_offset = 0;
	blknum = 0;
	ctx->valid = 0;
	if (*size < ctx->rabin_poly_avg_block_size) {
		/*
		 * Must ensure that we are signaling the index semaphores before skipping
		 * in order to maintain proper sequencing and avoid deadlocks.
		 */
		if (ctx->arc) {
			Sem_Wait(ctx->index_sem);
			Sem_Post(ctx->index_sem_next);
		}
		return (0);
	}
	DEBUG_STAT_EN(strt = get_wtime_millis());

	if (ctx->dedupe_flag == RABIN_DEDUPE_FIXED) {
		blknum = *size / ctx->rabin_poly_avg_block_size;
		j = *size % ctx->rabin_poly_avg_block_size;
		if (j)
			++blknum;
		else
			j = ctx->rabin_poly_avg_block_size;

		last_offset = 0;
		length = ctx->rabin_poly_avg_block_size;
		for (i=0; i<blknum; i++) {
			if (i == blknum-1) {
				length = j;
			}
			if (ctx->blocks[i] == 0) {
				ctx->blocks[i] = (rabin_blockentry_t *)slab_alloc(NULL,
				    sizeof (rabin_blockentry_t));
			}
			ctx->blocks[i]->offset = last_offset;
			ctx->blocks[i]->index = i; // Need to store for sorting
			ctx->blocks[i]->length = length;
			ctx->blocks[i]->similar = 0;
			last_offset += length;
		}
		goto process_blocks;
	}

	if (rabin_pos == NULL) {
		/*
		 * If global dedupe is active, the global blocks array uses temp space in
		 * the target buffer.
		 */
		ary_sz = 0;
		if (ctx->arc != NULL) {
			ary_sz = (sizeof (global_blockentry_t) * (*size / ctx->rabin_poly_min_block_size + 1));
			ctx->g_blocks = (global_blockentry_t *)SCRATCH_TAIL(ctx, ary_sz);
		}

		/*
		 * Initialize arrays for sketch computation. We re-use memory allocated
		 * for the compressed chunk temporarily.
		 */
		ary_sz += ctx->rabin_poly_max_block_size;
		ctx_heap = (uint32_t *)SCRATCH_TAIL(ctx, ary_sz);
	}
	/*
	 * If rabin_pos is non-NULL we are only asked for the last block
	 * boundary in the chunk, so that the caller can carry the bytes after
	 * it over to the next chunk, whose blocks start there. Any cut will
	 * do, so cutting starts max bytes before the end rather than at 0.
	 */
	if (rabin_pos) {
		last_offset = 0;
		i = *size > ctx->rabin_poly_max_block_size ?
		    *size - ctx->rabin_poly_max_block_size : 0;
		while (i < *size) {
			i += gear_cut(ctx, buf1 + i, *size - i);
			if (i < *size)
				last_offset = i;
		}
		*rabin_pos = last_offset;
		return (0);
	}

	/*
	 * Cut blocks until what is left is no longer than min; that tail
	 * (or a last stretch without a cut) becomes the final block below.
	 */
	while (*size - last_offset > ctx->rabin_poly_min_block_size) {
		uint64_t pc[4];
		uint32_t blen;

		blen = gear_cut(ctx, buf1 + last_offset, *size - last_offset);
		if (last_offset + blen == *size)
			break;
		length = blen;
		if (!(ctx->arc)) {
			if (ctx->blocks[blknum] == 0)
				ctx->blocks[blknum] = (rabin_blockentry_t *)slab_alloc(NULL,
				    sizeof (rabin_blockentry_t));
			ctx->blocks[blknum]->offset = last_offset;
			ctx->blocks[blknum]->index = blknum; // Need to store for sorting
			ctx->blocks[blknum]->length = length;
		} else {
			ctx->g_blocks[blknum].length = length;
			ctx->g_blocks[blknum].offset = last_offset;
		}
		DEBUG_STAT_EN(if (length >= ctx->rabin_poly_max_block_size) ++max_count);
		if (ctx->show_chunks) {
			fprintf(stderr, "Block offset: %" PRIu64 ", length: %u\n", last_offset, length);
		}

		/*
		 * Reset the heap structure and find the K min values if Delta Compression
		 * is enabled. We use a min heap mechanism taken from the heap based priority
		 * queue implementation in Python.
		 * Here K = similarity extent = 87% or 62% or 50%.
		 * 
		 * Once block contents are arranged in a min heap we compute the K min values
		 * sketch by hashing over the heap till K%. We interpret the raw bytes as a
		 * sequence of 64-bit integers.
		 * This is variant of minhashing which is used widely, for example in various
		 * search engines to detect similar documents.
		 */
		if (ctx->delta_flag) {
			length /= 8;
			pc[1] = DELTA_NORMAL_PCT(length);
			pc[2] = DELTA_EXTRA_PCT(length);
			pc[3] = DELTA_EXTRA2_PCT(length);

			heap_nsmallest(&heap, buf1+last_offset,
				       (int64_t *)ctx_heap, pc[ctx->delta_flag], length);
			ctx->blocks[blknum]->similarity_hash =
				XXH32((const uchar_t *)ctx_heap, heap_size(&heap)*8, 0);
		}
		++blknum;
		last_offset += blen;
	}

	// Insert the last left-over trailing bytes, if any, into a block.
	if (last_offset < *size) {
		length = *size - last_offset;
		if (!(ctx->arc)) {
			if (ctx->blocks[blknum] == 0)
				ctx->blocks[blknum] = (rabin_blockentry_t *)slab_alloc(NULL,
					sizeof (rabin_blockentry_t));
			ctx->blocks[blknum]->offset = last_offset;
			ctx->blocks[blknum]->index = blknum;
			ctx->blocks[blknum]->length = length;
		} else {
			ctx->g_blocks[blknum].length = length;
			ctx->g_blocks[blknum].offset = last_offset;
		}
		if (ctx->show_chunks) {
			fprintf(stderr, "Block offset: %" PRIu64 ", length: %u\n", last_offset, length);
		}

		if (ctx->delta_flag) {
			uint64_t cur_sketch;
			uint64_t pc[4];

			if (length > ctx->rabin_poly_min_block_size) {
				length /= 8;
				pc[1] = DELTA_NORMAL_PCT(length);
				pc[2] = DELTA_EXTRA_PCT(length);
				pc[3] = DELTA_EXTRA2_PCT(length);

				heap_nsmallest(&heap, buf1+last_offset,
					       (int64_t *)ctx_heap, pc[ctx->delta_flag], length);

				cur_sketch =
				    XXH32((const uchar_t *)ctx_heap,  heap_size(&heap)*8, 0);
			} else {
				cur_sketch =
				    XXH32((const uchar_t *)(buf1+last_offset), length, 0);
			}
			ctx->blocks[blknum]->similarity_hash = cur_sketch;
		}
		++blknum;
		last_offset = *size;
	}

process_blocks:
	// If we found at least a few chunks, perform dedup.
	DEBUG_STAT_EN(en_1 = get_wtime_millis());
	DEBUG_STAT_EN(fprintf(stderr, "Original size: %" PRId64 ", blknum: %u\n", *size, blknum));
	DEBUG_STAT_EN(fprintf(stderr, "Number of maxlen blocks: %u\n", max_count));
	if (blknum <=2 && ctx->arc) {
		Sem_Wait(ctx->index_sem);
		Sem_Post(ctx->index_sem_next);
	}
	if (blknum > 2) {
		uint64_t pos, matchlen, pos1 = 0;
		int valid = 1;
		pc_u32u_t *dedupe_index;	/* buffer offsets are unaligned */
		uint64_t dedupe_index_sz = 0;
		rabin_blockentry_t *be;
		DEBUG_STAT_EN(uint32_t delta_calls, delta_fails, merge_count, hash_collisions);
		DEBUG_STAT_EN(double w1 = 0);
		DEBUG_STAT_EN(double w2 = 0);
		DEBUG_STAT_EN(delta_calls = 0);
		DEBUG_STAT_EN(delta_fails = 0);
		DEBUG_STAT_EN(hash_collisions = 0);

		/*
		 * If global dedupe is enabled then process it here.
		 */
		if (ctx->arc) {
			uchar_t *g_dedupe_idx, *tgt, *src;

			/*
			 * First compute all the rabin chunk/block cryptographic hashes.
			 */
#if defined(_OPENMP)
#	pragma omp parallel for
#endif
			for (i=0; i<blknum; i++) {
				compute_checksum(ctx->g_blocks[i].cksum,
					ctx->arc->chunk_cksum_type, buf1+ctx->g_blocks[i].offset,
					ctx->g_blocks[i].length, 0, 0);
			}

			/*
			 * Index table within this segment.
			 */
			g_dedupe_idx = ctx->cbuf + RABIN_HDR_SIZE;
			dedupe_index_sz = 0;

			/*
			 * First entry in table is the original file offset where this
			 * data segment begins.
			 */
			U64_P(g_dedupe_idx) = LE64(ctx->file_offset);
			g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);
			dedupe_index_sz += 2;
			matchlen = 0;

			if (ctx->arc->dedupe_mode == MODE_SIMPLE) {
				/*======================================================================
				 * This code block implements Global Dedupe with simple in-memory index.
				 *======================================================================
				 */
				/*
				 * Now lookup blocks in index. First wait for our semaphore to be
				 * signaled. If the previous thread in sequence is using the index
				 * it will finish and then signal our semaphore. So we can have
				 * predictable serialization of index access in a sequence of
				 * threads without locking.
				 */
				length = 0;
				DEBUG_STAT_EN(w1 = get_wtime_millis());
				Sem_Wait(ctx->index_sem);
				DEBUG_STAT_EN(w2 = get_wtime_millis());
				for (i=0; i<blknum; i++) {
					hash_entry_t *he;

					he = db_lookup_insert_s(ctx->arc, ctx->g_blocks[i].cksum, 0,
						ctx->file_offset + ctx->g_blocks[i].offset,
						ctx->g_blocks[i].length, 1);
					if (!he) {
						/*
						 * Block match in index not found.
						 * Block was added to index. Merge this block.
						 */
						if (length + ctx->g_blocks[i].length >= RABIN_MAX_BLOCK_SIZE) {
							U32_P(g_dedupe_idx) = LE32(length);
							g_dedupe_idx += RABIN_ENTRY_SIZE;
							length = 0;
							dedupe_index_sz++;
						}
						length += ctx->g_blocks[i].length;
					} else {
						/*
						 * Block match in index was found.
						 */
						if (length > 0) {
							/*
							 * Write pending accumulated block length value.
							 */
							U32_P(g_dedupe_idx) = LE32(length);
							g_dedupe_idx += RABIN_ENTRY_SIZE;
							length = 0;
							dedupe_index_sz++;
						}

						/*
						 * Add a reference entry to the dedupe array.
						 */
						U32_P(g_dedupe_idx) = LE32((he->item_size | RABIN_INDEX_FLAG) &
							CLEAR_SIMILARITY_FLAG);
						g_dedupe_idx += RABIN_ENTRY_SIZE;
						U64_P(g_dedupe_idx) = LE64(he->item_offset);
						g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);
						matchlen += he->item_size;
						dedupe_index_sz += 3;
					}
				}

				/*
				 * Signal the next thread in sequence to access the index.
				 */
				Sem_Post(ctx->index_sem_next);

				/*
				 * Write final pending block length value (if any).
				 */
				if (length > 0) {
					U32_P(g_dedupe_idx) = LE32(length);
					g_dedupe_idx += RABIN_ENTRY_SIZE;
					length = 0;
					dedupe_index_sz++;
				}

				blknum = dedupe_index_sz; // Number of entries in block list
				tgt = g_dedupe_idx;
				g_dedupe_idx = ctx->cbuf + RABIN_HDR_SIZE;
				dedupe_index_sz = tgt - g_dedupe_idx;
				src = buf1;
				g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);

			} else {
				uchar_t *seg_heap, *sim_ck, *sim_offsets;
				archive_config_t *cfg;
				uint32_t len, blks, o_blks, k;
				global_blockentry_t *seg_blocks;
				uint64_t seg_offset, offset;
				global_blockentry_t **htab, *be;
				int sub_i;

				/*======================================================================
				 * This code block implements Segmented similarity based Dedupe with
				 * in-memory index for very large datasets.
				 * ======================================================================
				 */

				cfg = ctx->arc;
				assert(cfg->similarity_cksum_sz == sizeof (uint64_t));
				seg_heap = (uchar_t *)(ctx->g_blocks) - cfg->segment_sz * cfg->chunk_cksum_sz;
				ary_sz = (cfg->sub_intervals * cfg->similarity_cksum_sz + sizeof (blks) + 1) *
				    (blknum / cfg->segment_sz + 1) + 3;
				sim_offsets = seg_heap - ary_sz;
				src = sim_offsets;
				ary_sz = cfg->segment_sz * sizeof (global_blockentry_t **);
				htab = (global_blockentry_t **)(src - ary_sz);
				for (i=0; i<blknum;) {
					uint64_t crc, off1;
					uint64_t a, b;
					length = 0;

					/*
					 * Compute length of current segment.
					 */
					blks = cfg->segment_sz;
					if (blks > blknum-i) blks = blknum-i;
					length = 0;
					tgt = seg_heap;
#ifdef	__USE_SSE_INTRIN__
					if ((cfg->chunk_cksum_sz & 15) == 0) {
						for (j=0; j<blks; j++) {
							__m128i s;
							uchar_t *sc;
							k = cfg->chunk_cksum_sz;
							sc = ctx->g_blocks[j+i].cksum;

							/*
							 * Use SSE2 to copy 16 bytes at a time avoiding a call
							 * to memcpy() since hash sizes are typically multiple
							 * of 16 bytes: 256-bit or 512-bit.
							 */
							while (k > 0) {
								s = _mm_loadu_si128((__m128i *)sc);
								_mm_storeu_si128((__m128i *)tgt, s);
								tgt += 16;
								sc += 16;
								k -= 16;
							}
							length += cfg->chunk_cksum_sz;
						}
					} else {
#else
					{
#endif
						for (j=0; j<blks; j++) {
							memcpy(tgt, ctx->g_blocks[j+i].cksum, cfg->chunk_cksum_sz);
							length += cfg->chunk_cksum_sz;
							tgt += cfg->chunk_cksum_sz;
						}
					}
					U32_P(src) = blks;
					src += sizeof (blks);
					blks = j+i;

					/*
					 * Assume the concatenated chunk hash buffer as an array of 64-bit
					 * integers and sort them in ascending order.
					 */
					do_qsort((pc_u64u_t *)seg_heap, length/8);

					/*
					 * Compute the K min values sketch where K == 20 in this case.
					 */
					sim_ck = ctx->similarity_cksums;
					tgt = seg_heap;
					sub_i = 0;

					U64_P(sim_ck) = 0;
					a = 0;
					for (j = 0; j < length && sub_i < cfg->sub_intervals;) {
						b = U64_P(tgt);
						tgt += sizeof (uint64_t);
						j += sizeof (uint64_t);
						if (b != a) {
							U64_P(sim_ck) = b;
							sim_ck += sizeof (uint64_t);
							a = b;
							sub_i++;
						}
					}

					/*
					 * Begin shared index access and write segment metadata to cache
					 * first.
					 */
					if (i == 0) {
						DEBUG_STAT_EN(w1 = get_wtime_millis());
						Sem_Wait(ctx->index_sem);
						DEBUG_STAT_EN(w2 = get_wtime_millis());
					}

					seg_offset = db_segcache_pos(cfg, ctx->id);
					len = (blks-i) * sizeof (global_blockentry_t);
					if (db_segcache_write(cfg, ctx->id, (uchar_t *)&(ctx->g_blocks[i]),
					    len, blks-i, ctx->file_offset) == -1) {
						Sem_Post(ctx->index_sem_next);
						ctx->valid = 0;
						return (0);
					}

					/*
					 * Now lookup all the similarity hashes.
					 * The matching segment offsets in the segcache are stored in a list. Entries
					 * that were not found are stored with offset of UINT64_MAX.
					 */
					sim_ck = ctx->similarity_cksums;
					tgt = src + 1; // One byte for number of entries
					crc = 0;
					off1 = UINT64_MAX;
					k = 0;

					for (j=0; j < sub_i; j++) {
						hash_entry_t *he = NULL;
						he = db_lookup_insert_s(cfg, sim_ck, 0, seg_offset, 0, 1);
						if (he) {
							U64_P(tgt) = he->item_offset;
						} else {
							U64_P(tgt) = UINT64_MAX;
						}
						sim_ck += cfg->similarity_cksum_sz;
						tgt += cfg->similarity_cksum_sz;
					}

					/*
					 * At this point we have a list of segment offsets from the segcache
					 * file. Sort the offsets to avoid subsequent random access.
					 */
					tgt = src + 1;
					isort_uint64((pc_u64u_t *)tgt, sub_i);

					/*
					 * Now eliminate duplicate offsets and UINT64_MAX offset entries which
					 * indicate entries that were not found.
					 */
					sim_ck = tgt;
					for (j=0; j < sub_i; j++) {
						if (off1 != U64_P(sim_ck) && U64_P(sim_ck) != UINT64_MAX) {
							off1 = U64_P(sim_ck);
							U64_P(tgt) = off1;
							tgt += cfg->similarity_cksum_sz;
							k++;
						}
						sim_ck += cfg->similarity_cksum_sz;
					}
					*src = k; // Number of entries
					src = tgt;
					i = blks;
				}

				/*
				 * Signal the next thread in sequence to access the index.
				 */
				Sem_Post(ctx->index_sem_next);

				/*
				 * Now go through all the matching segments for all the current segments
				 * and perform actual deduplication.
				 */
				src = sim_offsets;
				for (i=0; i<blknum;) {
					blks = U32_P(src) + i;
					src += sizeof (blks);

					/*
					 * Insert current segment blocks into local hashtable and do partial
					 * in-segment deduplication.
					 */
					be = NULL;
					memset(htab, 0, ary_sz);
					for (k=i; k<blks; k++) {
						uint32_t hent;
						hent = XXH32(ctx->g_blocks[k].cksum, cfg->chunk_cksum_sz, 0);
						hent ^= (hent / cfg->chunk_cksum_sz);
						hent = hent % cfg->segment_sz;

						if (htab[hent] == NULL) {
							htab[hent] = &(ctx->g_blocks[k]);
							ctx->g_blocks[k].offset += ctx->file_offset;
							ctx->g_blocks[k].next = NULL;
							be = NULL;
						} else {
							be = htab[hent];
							do {
								if (ckcmp(ctx->g_blocks[k].cksum,
								    be->cksum, cfg->chunk_cksum_sz) == 0 &&
								    ctx->g_blocks[k].length == be->length) {
									global_blockentry_t *en;

									/*
									 * Block match in index was found. Update g_blocks
									 * array.
									 */
									en = &(ctx->g_blocks[k]);
									en->length = (en->length | RABIN_INDEX_FLAG) &
										CLEAR_SIMILARITY_FLAG;
									en->offset = be->offset;
									break;
								}
								if (be->next) {
									be = be->next;
								} else {
									be->next = &(ctx->g_blocks[k]);
									be->next->offset += ctx->file_offset;
									be->next->next = NULL;
									break;
								}
							} while(1);
						}
					}

					/*
					 * Now go through segment match list which was prepared earlier
					 * and deduplicate with the matching segment blocks.
					 */
					sub_i = *src;
					src++;
					sim_ck = src;
					for (j=0; j < sub_i; j++) {
						/*
						 * Load segment metadata from disk and perform identity deduplication
						 * with the segment chunks.
						 */
						offset = U64_P(sim_ck);
						if (db_segcache_map(cfg, ctx->id, &o_blks, &offset,
						    (uchar_t **)&seg_blocks) == -1) {
							log_msg(LOG_ERR, 0, "** Segment cache mmap failed.\n");
							ctx->valid = 0;
							return (0);
						}

						/*
						 * Now lookup loaded segment blocks in hashtable. If match is
						 * found then the hashtable entry is updated to point to the
						 * loaded segment block.
						 */
						for (k=0; k<o_blks; k++) {
							uint32_t hent;
							hent = XXH32(seg_blocks[k].cksum, cfg->chunk_cksum_sz, 0);
							hent ^= (hent / cfg->chunk_cksum_sz);
							hent = hent % cfg->segment_sz;

							if (htab[hent] != NULL) {
								be = htab[hent];
								do {
									if (be->length & RABIN_INDEX_FLAG)
										goto next_ent;
									if (ckcmp(seg_blocks[k].cksum,
									    be->cksum, cfg->chunk_cksum_sz) == 0 &&
									    seg_blocks[k].length == be->length) {
										be->length = (be->length |
											RABIN_INDEX_FLAG) &
											CLEAR_SIMILARITY_FLAG;
										be->offset = seg_blocks[k].offset +
											offset;
										break;
									}
next_ent:
									if (be->next)
										be = be->next;
									else
										break;
								} while(1);
							}
						}
						sim_ck += cfg->similarity_cksum_sz;
					}
					src = sim_ck;
					i = blks;
				}

				/*======================================================================
				 * Finally scan the blocks array and update dedupe index.
				 *======================================================================
				 */
				length = 0;
				for (i=0; i<blknum; i++) {

					if (!(ctx->g_blocks[i].length & RABIN_INDEX_FLAG)) {
						/*
						 * Block match in index was not found.
						 * Block was added to index. Merge this block.
						 */
						if (length + ctx->g_blocks[i].length > RABIN_MAX_BLOCK_SIZE) {
							U32_P(g_dedupe_idx) = LE32(length);
							g_dedupe_idx += RABIN_ENTRY_SIZE;
							length = 0;
							dedupe_index_sz++;
						}
						length += ctx->g_blocks[i].length;
					} else {
						/*
						 * Block match in index was found.
						 */
						if (length > 0) {
							/*
							 * Write pending accumulated block length value.
							 */
							U32_P(g_dedupe_idx) = LE32(length);
							g_dedupe_idx += RABIN_ENTRY_SIZE;
							length = 0;
							dedupe_index_sz++;
						}
						/*
						 * Add a reference entry to the dedupe array.
						 */
						U32_P(g_dedupe_idx) = LE32(ctx->g_blocks[i].length);
						g_dedupe_idx += RABIN_ENTRY_SIZE;
						U64_P(g_dedupe_idx) = LE64(ctx->g_blocks[i].offset);
						g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);
						matchlen += (ctx->g_blocks[i].length & RABIN_INDEX_VALUE);
						dedupe_index_sz += 3;
					}
				}

				/*
				 * Write final pending block length value (if any).
				 */
				if (length > 0) {
					U32_P(g_dedupe_idx) = LE32(length);
					g_dedupe_idx += RABIN_ENTRY_SIZE;
					length = 0;
					dedupe_index_sz++;
				}

				blknum = dedupe_index_sz; // Number of entries in block list
				tgt = g_dedupe_idx;
				g_dedupe_idx = ctx->cbuf + RABIN_HDR_SIZE;
				dedupe_index_sz = tgt - g_dedupe_idx;
				src = buf1;
				g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);
			}

			/*
			 * Deduplication reduction should at least be greater than block list metadata.
			 */
			if (matchlen < dedupe_index_sz) {
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunking speed %.3f MB/s, Overall Dedupe speed %.3f MB/s\n", 
					get_mb_s(*size, strt, en_1), get_mb_s(*size, strt, en - (w2 - w1))));
				DEBUG_STAT_EN(fprintf(stderr, "No Dedupe possible.\n"));
				ctx->valid = 0;
				return (0);
			}

			/*
			 * Now copy the block data;
			 */
			for (i=0; i<blknum-2;) {
				length = LE32(U32_P(g_dedupe_idx));
				g_dedupe_idx += RABIN_ENTRY_SIZE;
				++i;

				j = length & RABIN_INDEX_FLAG;
				length = length & RABIN_INDEX_VALUE;
				if (!j) {
					memcpy(tgt, src, length);
					tgt += length;
					src += length;
				} else {
					src += length;
					g_dedupe_idx += (RABIN_ENTRY_SIZE * 2);
					i += 2;
				}
			}
			pos1 = tgt - ctx->cbuf;
			blknum |= GLOBAL_FLAG;
			goto dedupe_done;
		}

		/*
		 * Subsequent processing below is for per-segment Deduplication.
		 */

		/*
		 * Compute hash signature for each block. We do this in a separate loop to 
		 * have a fast linear scan through the buffer.
		 */
		if (ctx->delta_flag) {
#if defined(_OPENMP)
#	pragma omp parallel for if (mt)
#endif
			for (i=0; i<blknum; i++) {
				ctx->blocks[i]->hash = XXH32(buf1+ctx->blocks[i]->offset,
								ctx->blocks[i]->length, 0);
			}
		} else {
#if defined(_OPENMP)
#	pragma omp parallel for if (mt)
#endif
			for (i=0; i<blknum; i++) {
				ctx->blocks[i]->hash = XXH32(buf1+ctx->blocks[i]->offset,
								ctx->blocks[i]->length, 0);
				ctx->blocks[i]->similarity_hash = ctx->blocks[i]->hash;
			}
		}

		ary_sz = (blknum << 1) * sizeof (rabin_blockentry_t *);
		htab = (rabin_blockentry_t **)SCRATCH_TAIL(ctx, ary_sz);
		memset(htab, 0, ary_sz);

		/*
		 * Perform hash-matching of blocks and use a bucket-chained hashtable to match
		 * for duplicates and similar blocks. Unique blocks are inserted and duplicates
		 * and similar ones are marked in the block array.
		 *
		 * Hashtable memory is not allocated. We just use available space in the
		 * target buffer.
		 */
		matchlen = 0;
		for (i=0; i<blknum; i++) {
			uint64_t ck;

			/*
			 * Bias hash with length for fewer collisions. If Delta Compression is
			 * not enabled then value of similarity_hash == hash.
			 */
			ck = ctx->blocks[i]->similarity_hash;
			ck ^= (ck / ctx->blocks[i]->length);
			j = ck % (blknum << 1);

			if (htab[j] == 0) {
				/*
				 * Hash bucket empty. So add block into table.
				 */
				htab[j] = ctx->blocks[i];
				ctx->blocks[i]->other = 0;
				ctx->blocks[i]->next = 0;
				ctx->blocks[i]->similar = 0;
			} else {
				be = htab[j];
				length = 0;

				/*
				 * Look for exact duplicates. Same cksum, length and memcmp()
				 */
				while (1) {
					if (be->hash == ctx->blocks[i]->hash &&
					    be->length == ctx->blocks[i]->length &&
					    memcmp(buf1 + be->offset, buf1 + ctx->blocks[i]->offset,
					    be->length) == 0) {
						ctx->blocks[i]->similar = SIMILAR_EXACT;
						ctx->blocks[i]->other = be;
						be->similar = SIMILAR_REF;
						matchlen += be->length;
						length = 1;
						break;
					}
					if (be->next)
						be = be->next;
					else
						break;
				}

				if (ctx->delta_flag && !length) {
					/*
					 * Look for similar blocks.
					 */
					be = htab[j];
					while (1) {
						if (be->similarity_hash == ctx->blocks[i]->similarity_hash &&
						    be->length == ctx->blocks[i]->length) {
							uint64_t off_diff;
							if (be->offset > ctx->blocks[i]->offset)
								off_diff = be->offset - ctx->blocks[i]->offset;
							else
								off_diff = ctx->blocks[i]->offset - be->offset;

							if (off_diff > ctx->deltac_min_distance) {
								ctx->blocks[i]->similar = SIMILAR_PARTIAL;
								ctx->blocks[i]->other = be;
								be->similar = SIMILAR_REF;
								matchlen += (be->length>>1);
								length = 1;
								break;
							}
						}
						if (be->next)
							be = be->next;
						else
							break;
					}
				}
				/*
				 * No duplicate in table for this block. So add it to
				 * the bucket chain.
				 */
				if (!length) {
					ctx->blocks[i]->other = 0;
					ctx->blocks[i]->next = 0;
					ctx->blocks[i]->similar = 0;
					be->next = ctx->blocks[i];
					DEBUG_STAT_EN(++hash_collisions);
				}
			}
		}
		DEBUG_STAT_EN(fprintf(stderr, "Total Hashtable bucket collisions: %u\n", hash_collisions));

		dedupe_index_sz = (uint64_t)blknum * RABIN_ENTRY_SIZE;
		if (matchlen < dedupe_index_sz) {
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunking speed %.3f MB/s, Overall Dedupe speed %.3f MB/s\n",
					      get_mb_s(*size, strt, en_1), get_mb_s(*size, strt, en)));
			DEBUG_STAT_EN(fprintf(stderr, "No Dedupe possible.\n"));
			ctx->valid = 0;
			return (0);
		}

		dedupe_index = (pc_u32u_t *)(ctx->cbuf + RABIN_HDR_SIZE);
		pos = 0;
		DEBUG_STAT_EN(merge_count = 0);

		/*
		 * Merge runs of unique blocks into a single block entry to reduce
		 * dedupe index size.
		 */
		for (i=0; i<blknum;) {
			dedupe_index[pos] = i;
			ctx->blocks[i]->index = pos;
			++pos;
			length = 0;
			j = i;
			if (ctx->blocks[i]->similar == 0) {
				while (i< blknum && ctx->blocks[i]->similar == 0 &&
				   length < RABIN_MAX_BLOCK_SIZE) {
					length += ctx->blocks[i]->length;
					++i;
					DEBUG_STAT_EN(++merge_count);
				}
				ctx->blocks[j]->length = length;
			} else {
				++i;
			}
		}
		DEBUG_STAT_EN(fprintf(stderr, "Merge count: %u\n", merge_count));

		/*
		 * Final pass update dedupe index and copy data.
		 */
		blknum = pos;
		dedupe_index_sz = (uint64_t)blknum * RABIN_ENTRY_SIZE;
		pos1 = dedupe_index_sz + RABIN_HDR_SIZE;
		matchlen = ctx->real_chunksize - *size;
		for (i=0; i<blknum; i++) {
			be = ctx->blocks[dedupe_index[i]];
			if (be->similar == 0 || be->similar == SIMILAR_REF) {
				/* Just copy. */
				dedupe_index[i] = htonl(be->length);
				memcpy(ctx->cbuf + pos1, buf1 + be->offset, be->length);
				pos1 += be->length;
			} else {
				if (be->similar == SIMILAR_EXACT) {
					dedupe_index[i] = htonl((be->other->index | RABIN_INDEX_FLAG) &
					    CLEAR_SIMILARITY_FLAG);
				} else {
					uchar_t *oldbuf, *newbuf;
					int32_t bsz;
					/*
					 * Perform bsdiff.
					 */
					oldbuf = buf1 + be->other->offset;
					newbuf = buf1 + be->offset;
					DEBUG_STAT_EN(++delta_calls);

					bsz = bsdiff(oldbuf, be->other->length, newbuf, be->length,
					    ctx->cbuf + pos1, buf1 + *size, matchlen);
					if (bsz == 0) {
						DEBUG_STAT_EN(++delta_fails);
						memcpy(ctx->cbuf + pos1, newbuf, be->length);
						dedupe_index[i] = htonl(be->length);
						pos1 += be->length;
					} else {
						dedupe_index[i] = htonl(be->other->index |
						    RABIN_INDEX_FLAG | SET_SIMILARITY_FLAG);
						pos1 += bsz;
					}
				}
			}
		}

dedupe_done:
		if (valid) {
			uchar_t *cbuf = ctx->cbuf;
			pc_u64u_t *entries;	/* at offset 4: unaligned */
			DEBUG_STAT_EN(uint64_t sz);
			DEBUG_STAT_EN(sz = *size);
			U32_P(cbuf) = htonl(blknum);
			cbuf += sizeof (uint32_t);
			entries = (pc_u64u_t *)cbuf;
			entries[0] = htonll(*size);
			entries[1] = 0;
			entries[2] = htonll(pos1 - dedupe_index_sz - RABIN_HDR_SIZE);
			*size = pos1;
			ctx->valid = 1;
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Deduped size: %" PRId64 ", blknum: %u, delta_calls: %u, delta_fails: %u\n",
					     *size, (unsigned int)(blknum & CLEAR_GLOBAL_FLAG), delta_calls, delta_fails));
			DEBUG_STAT_EN(fprintf(stderr, "Chunking speed %.3f MB/s, Overall Dedupe speed %.3f MB/s\n",
					      get_mb_s(sz, strt, en_1), get_mb_s(sz, strt, en)));
			/*
			 * Remaining header entries: size of compressed index and size of
			 * compressed data are inserted later via rabin_update_hdr, after actual compression!
			 */
			return (dedupe_index_sz);
		}
	}
	return (0);
}

void
update_dedupe_hdr(uchar_t *buf, uint64_t dedupe_index_sz_cmp, uint64_t dedupe_data_sz_cmp)
{
	pc_u64u_t *entries;	/* at offset 4: unaligned */

	buf += sizeof (uint32_t);
	entries = (pc_u64u_t *)buf;
	entries[1] = htonll(dedupe_index_sz_cmp);
	entries[3] = htonll(dedupe_data_sz_cmp);
}

void
parse_dedupe_hdr(uchar_t *buf, uint32_t *blknum, uint64_t *dedupe_index_sz,
		uint64_t *dedupe_data_sz, uint64_t *dedupe_index_sz_cmp,
		uint64_t *dedupe_data_sz_cmp, uint64_t *deduped_size)
{
	pc_u64u_t *entries;	/* at offset 4: unaligned */

	*blknum = ntohl(U32_P(buf));
	buf += sizeof (uint32_t);

	entries = (pc_u64u_t *)buf;
	*dedupe_data_sz = ntohll(entries[0]);
	*dedupe_index_sz = (uint64_t)(*blknum & CLEAR_GLOBAL_FLAG) * RABIN_ENTRY_SIZE;
	*dedupe_index_sz_cmp =  ntohll(entries[1]);
	*deduped_size = ntohll(entries[2]);
	*dedupe_data_sz_cmp = ntohll(entries[3]);
}

/*
 * Copy len bytes of a global dedupe reference to pos1 (a file offset)
 * into ctx->cbuf at sz.  The segment being rebuilt starts at file offset
 * `offset`: a reference at or after it must lie in the sz bytes already
 * rebuilt, and one before it in the output file written so far.  Dedupe
 * only makes backward references, so either holds for a valid file.
 * Returns 0, or -1 for a reference that points elsewhere.  NB: the
 * caller has checked that sz + len fits the output.
 */
static int
dedupe_copy_ref(dedupe_context_t *ctx, uint64_t pos1, uint32_t len, uint64_t offset,
    uint64_t sz)
{
	uchar_t *src;
	uint64_t adj;
	struct stat sb;

	if (pos1 >= offset) {
		if (pos1 - offset > sz || len > sz - (pos1 - offset))
			return (-1);
		memcpy(ctx->cbuf + sz, ctx->cbuf + (pos1 - offset), len);
		return (0);
	}

	/*
	 * Output can be short if an earlier chunk failed; an mmap() beyond
	 * EOF would SIGBUS.
	 */
	if (len > offset - pos1 || fstat(ctx->out_fd, &sb) == -1 ||
	    pos1 > (uint64_t)sb.st_size || len > (uint64_t)sb.st_size - pos1)
		return (-1);
	adj = pos1 % ctx->pagesize;
	src = mmap(NULL, len + adj, PROT_READ, MAP_SHARED, ctx->out_fd, pos1 - adj);
	if (src == MAP_FAILED) {
		log_msg(LOG_ERR, 1, "MMAP failed ");
		return (-1);
	}
	memcpy(ctx->cbuf + sz, src + adj, len);
	munmap(src, len + adj);
	return (0);
}

/*
 * Rebuild a global dedupe (-G) chunk into ctx->cbuf.  buf is the chunk
 * (header, an index of nent 32-bit entries, then dlen bytes of block
 * data) and data_sz the size to restore.  The index starts with the
 * segment's file offset (two entries); then a literal is one entry, its
 * bytes next in the data, and a reference three, its length and a file
 * offset.  Returns the bytes restored, clearing ctx->valid on an entry
 * that does not fit.  NB: the caller holds the index token.
 */
static uint64_t
dedupe_decompress_global(dedupe_context_t *ctx, uchar_t *buf, uint32_t nent,
    uint64_t dlen, uint64_t data_sz)
{
	uchar_t *idx, *src;
	uint64_t offset, pos1, sz, dpos;
	uint32_t blk, len, flag;

	if (nent < 2) {
		log_msg(LOG_ERR, 0, "Dedup index too short.\n");
		ctx->valid = 0;
		return (0);
	}
	idx = buf + RABIN_HDR_SIZE;
	src = idx + (uint64_t)nent * RABIN_ENTRY_SIZE;
	offset = LE64(U64_P(idx));
	idx += (RABIN_ENTRY_SIZE * 2);
	sz = 0;
	dpos = 0;
	for (blk = 2; blk < nent; blk++) {
		len = LE32(U32_P(idx));
		idx += RABIN_ENTRY_SIZE;
		flag = len & RABIN_INDEX_FLAG;
		len &= RABIN_INDEX_VALUE;

		if (len > data_sz - sz) {
			log_msg(LOG_ERR, 0, "Dedup data overflows chunk.\n");
			ctx->valid = 0;
			break;
		}
		if (flag == 0) {
			if (len > dlen - dpos) {
				log_msg(LOG_ERR, 0, "Dedup block past the chunk data.\n");
				ctx->valid = 0;
				break;
			}
			memcpy(ctx->cbuf + sz, src + dpos, len);
			dpos += len;
		} else {
			/* A reference: its file offset takes two more entries. */
			if (nent - blk - 1 < 2) {
				log_msg(LOG_ERR, 0, "Dedup index too short.\n");
				ctx->valid = 0;
				break;
			}
			pos1 = LE64(U64_P(idx));
			idx += (RABIN_ENTRY_SIZE * 2);
			blk += 2;
			if (dedupe_copy_ref(ctx, pos1, len, offset, sz) != 0) {
				log_msg(LOG_ERR, 0, "Invalid dedup reference.\n");
				ctx->valid = 0;
				break;
			}
		}
		sz += len;
	}
	return (sz);
}

/*
 * First pass of per-segment dedupe recovery: fill ctx->blocks[] from the
 * chunk's index of blknum entries, which buf holds after the header,
 * followed by dlen bytes of block data.  A literal block gets its offset
 * in buf and its length; a duplicate the index of the block it repeats;
 * a delta block that index (with SET_SIMILARITY_FLAG) and the offset of
 * its patch.  Every entry other than a literal gets length 0, so a
 * reference to it can be told from a reference to a literal.  Returns 0,
 * or -1 if a block or patch runs past the data.
 */
static int
dedupe_index_blocks(dedupe_context_t *ctx, uchar_t *buf, uint32_t blknum, uint64_t dlen)
{
	pc_u32u_t *dedupe_index;	/* buffer offsets are unaligned */
	uint64_t pos1, end;
	uint32_t blk, len;
	bsize_t blen;

	dedupe_index = (pc_u32u_t *)(buf + RABIN_HDR_SIZE);
	pos1 = (uint64_t)blknum * RABIN_ENTRY_SIZE + RABIN_HDR_SIZE;
	end = pos1 + dlen;
	for (blk = 0; blk < blknum; blk++) {
		rabin_blockentry_t *be;

		if (ctx->blocks[blk] == 0)
			ctx->blocks[blk] = (rabin_blockentry_t *)slab_alloc(NULL,
			    sizeof (rabin_blockentry_t));
		if ((be = ctx->blocks[blk]) == NULL) {
			log_msg(LOG_ERR, 0, "Out of memory.\n");
			return (-1);
		}
		len = ntohl(U32_P(&dedupe_index[blk]));	/* U32_P: see utils.h */
		be->hash = 0;
		be->length = 0;
		if (len == 0) {
			be->hash = 1;
		} else if (!(len & RABIN_INDEX_FLAG)) {
			if (len > end - pos1)
				return (-1);
			be->length = len;
			be->offset = pos1;
			pos1 += len;
		} else if (len & GET_SIMILARITY_FLAG) {
			blen = get_bsdiff_sz(buf + pos1, end - pos1);
			if (blen == 0)
				return (-1);
			be->offset = pos1;
			be->index = (len & RABIN_INDEX_VALUE) | SET_SIMILARITY_FLAG;
			pos1 += blen;
		} else {
			be->index = len & RABIN_INDEX_VALUE;
		}
	}
	return (0);
}

/*
 * Second pass of per-segment dedupe recovery: copy the blocks that
 * dedupe_index_blocks() described into ctx->cbuf, up to data_sz bytes.
 * Duplicate and delta blocks must refer to a literal block.  Returns the
 * bytes restored, clearing ctx->valid on a block that does not fit.
 */
static uint64_t
dedupe_restore_blocks(dedupe_context_t *ctx, uchar_t *buf, uint32_t blknum,
    uint64_t data_sz)
{
	rabin_blockentry_t *be, *ob;
	uint64_t sz;
	uint32_t blk, oblk;
	bsize_t newsz;

	sz = 0;
	for (blk = 0; blk < blknum; blk++) {
		be = ctx->blocks[blk];
		if (be->hash == 1)
			continue;
		ob = be;
		if (be->length == 0) {
			oblk = be->index & CLEAR_SIMILARITY_FLAG;
			if (oblk >= blknum || ctx->blocks[oblk]->length == 0) {
				log_msg(LOG_ERR, 0, "Invalid dedup block reference.\n");
				ctx->valid = 0;
				break;
			}
			ob = ctx->blocks[oblk];
		}
		if (be->length == 0 && (be->index & GET_SIMILARITY_FLAG)) {
			newsz = data_sz - sz > INT32_MAX ? INT32_MAX : (bsize_t)(data_sz - sz);
			if (bspatch(buf + be->offset, buf + ob->offset, ob->length,
			    ctx->cbuf + sz, &newsz) == 0) {
				log_msg(LOG_ERR, 0, "Failed to bspatch block.\n");
				ctx->valid = 0;
				break;
			}
			sz += newsz;
			continue;
		}
		if (ob->length > data_sz - sz) {
			log_msg(LOG_ERR, 0, "Dedup data overflows chunk.\n");
			ctx->valid = 0;
			break;
		}
		memcpy(ctx->cbuf + sz, buf + ob->offset, ob->length);
		sz += ob->length;
	}
	return (sz);
}

/*
 * Rebuild a deduplicated chunk into ctx->cbuf.  buf holds the dedupe
 * header, the block index and the block data, and *size on entry the
 * number of block data bytes (what the data decompressor produced);
 * *size returns the restored size.  Everything in buf comes from the
 * file: each count, index, offset and length is checked before use, and
 * any that does not fit clears ctx->valid.  A global chunk (GLOBAL_FLAG)
 * takes the index token, ctx->index_sem; it is refused, without waiting,
 * if ctx was not set up for global dedupe.
 */
void
dedupe_decompress(dedupe_context_t *ctx, uchar_t *buf, uint64_t *size)
{
	uint32_t blknum;
	uint64_t data_sz, sz, indx_cmp, data_sz_cmp, deduped_sz, dedupe_index_sz, dlen;

	parse_dedupe_hdr(buf, &blknum, &dedupe_index_sz, &data_sz, &indx_cmp, &data_sz_cmp,
	    &deduped_sz);
	dlen = *size;
	*size = data_sz;
	ctx->valid = 1;

	if (blknum & GLOBAL_FLAG) {
		if (ctx->dedupe_flag != RABIN_DEDUPE_FILE_GLOBAL) {
			log_msg(LOG_ERR, 0, "Global dedup chunk in a file without it.\n");
			ctx->valid = 0;
			return;
		}
		Sem_Wait(ctx->index_sem);
		sz = dedupe_decompress_global(ctx, buf, blknum & CLEAR_GLOBAL_FLAG, dlen,
		    data_sz);
	} else {
		if (ctx->blocks == NULL || blknum > ctx->blknum) {
			log_msg(LOG_ERR, 0, "Invalid dedup block count.\n");
			ctx->valid = 0;
			return;
		}
		if (dedupe_index_blocks(ctx, buf, blknum, dlen) != 0) {
			log_msg(LOG_ERR, 0, "Dedup block past the chunk data.\n");
			ctx->valid = 0;
			return;
		}
		sz = dedupe_restore_blocks(ctx, buf, blknum, data_sz);
	}
	if (ctx->valid && sz < data_sz) {
		log_msg(LOG_ERR, 0, "Too little dedup data processed.\n");
		ctx->valid = 0;
	}
}
