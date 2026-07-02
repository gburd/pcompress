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

/**
 * @file pcompress.h
 * @brief Public API for the Pcompress compression library.
 *
 * This header defines the main data structures and functions for using
 * Pcompress as a library (libpcompress). It provides chunked parallel
 * multi-algorithm lossless compression and decompression with support for
 * archiving, deduplication, encryption, and various pre-processing filters.
 *
 * Typical usage:
 * @code
 *   pc_ctx_t *pctx = create_pc_context();
 *   init_pc_context(pctx, argc, argv);
 *   start_pcompress(pctx);
 *   destroy_pc_context(pctx);
 * @endcode
 */

#ifndef	_PCOMPRESS_H
#define	_PCOMPRESS_H

#include <sys/types.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef	__cplusplus
extern "C" {
#endif

#include <rabin_dedup.h>
#include <crypto_utils.h>
#include <filters/analyzer/analyzer.h>
#include <meta_stream.h>

/** @name File Format Constants */
/**@{*/
#define	CHUNK_FLAG_SZ	1       /**< Size of chunk flag field in bytes. */
#define	ALGO_SZ		8       /**< Size of algorithm name field in file header. */
#define	MIN_CHUNK	2048    /**< Minimum allowed chunk size in bytes. */
#define	VERSION		10      /**< Current file format version number. */
#define	FLAG_DEDUP	1       /**< File flag: Rabin deduplication enabled. */
#define	FLAG_DEDUP_FIXED	2 /**< File flag: fixed-block deduplication enabled. */
#define	FLAG_SINGLE_CHUNK	4 /**< File flag: entire file in one chunk. */
#define FLAG_META_STREAM	4096 /**< File flag: metadata streams present. */
#define	FLAG_ARCHIVE	2048    /**< File flag: archive mode (PAX). */
#define	UTILITY_VERSION	"4.0.0"   /**< Human-readable utility version string. */
#define	MASK_CRYPTO_ALG	0x30    /**< Bitmask for crypto algorithm in flags. */
#define	MAX_LEVEL	14      /**< Maximum compression level. */
/**@}*/

#ifndef _MPLV2_LICENSE_
#define	LICENSE_STRING "LGPLv3"
#else
#define	LICENSE_STRING "MPLv2"
#endif

#define	COMPRESSED	1
#define	UNCOMPRESSED	0
#define	CHSIZE_MASK	0x80
#define	BZIP2_A_NUM	16
#define	LZMA_A_NUM	32
#define	CHUNK_FLAG_DEDUP	2
#define	CHUNK_FLAG_PREPROC	4
#define	COMP_EXTN	".pz"

#define	PREPROC_TYPE_LZP	1
#define	PREPROC_TYPE_DELTA2	2
#define	PREPROC_TYPE_DISPACK	4
#define	PREPROC_TYPE_DICT	8
#define	PREPROC_TYPE_E8E9	16
#define	PREPROC_COMPRESSED	128

/*
 * Sizes of chunk header components.
 */
#define	COMPRESSED_CHUNKSZ	(sizeof (uint64_t))
#define	ORIGINAL_CHUNKSZ	(sizeof (uint64_t))
#define	CHUNK_HDR_SZ		(COMPRESSED_CHUNKSZ + pctx->cksum_bytes + ORIGINAL_CHUNKSZ + CHUNK_FLAG_SZ)

/*
 * lower 3 bits in higher nibble indicate chunk compression algorithm
 * in adaptive modes.
 */
#define	ADAPT_COMPRESS_NONE	0
#define	ADAPT_COMPRESS_LZMA	1
#define	ADAPT_COMPRESS_BZIP2	2
#define	ADAPT_COMPRESS_PPMD	3
#define	ADAPT_COMPRESS_BSC	4
/*
 * This is used in adaptive modes in cases where the data is deemed totally incompressible.
 * We can still have zero padding and archive headers that can be compressed. So we use the
 * fastest algo at our disposal for these cases.
 */
#define	ADAPT_COMPRESS_LZ4	5
#define	ADAPT_COMPRESS_ZSTD	6
#define	CHDR_ALGO_MASK	7
#define	CHDR_ALGO(x) (((x)>>4) & CHDR_ALGO_MASK)

/** @name Buffer Size Helpers */
/**@{*/
extern uint32_t zlib_buf_extra(uint64_t buflen);
extern int lz4_buf_extra(uint64_t buflen);
#ifdef ENABLE_PC_ZSTD
extern int zstd_buf_extra(uint64_t buflen);
#endif
/**@}*/

/**
 * @name Compression Functions
 * All compression functions share the same signature.
 * @param src       Input buffer to compress.
 * @param srclen    Length of the input buffer.
 * @param dst       Output buffer for compressed data (pre-allocated).
 * @param destlen   On input, size of dst. On output, actual compressed size.
 * @param level     Compression level (1-14).
 * @param chdr      Chunk header byte (adaptive mode flags).
 * @param btype     Data type hint from analyzer (DATA_TEXT, DATA_BINARY, etc.).
 * @param data      Algorithm-private state from the corresponding init function.
 * @return 0 on success, -1 on failure.
 */
/**@{*/
extern int zlib_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *destlen, int level, uchar_t chdr, int btype, void *data);
extern int lzma_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *destlen, int level, uchar_t chdr, int btype, void *data);
extern int bzip2_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *destlen, int level, uchar_t chdr, int btype, void *data);
extern int adapt_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int ppmd_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int lz_fx_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int lz4_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int none_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
#ifdef ENABLE_PC_ZSTD
extern int zstd_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
#endif
/**@}*/

/**
 * @name Decompression Functions
 * All decompression functions share the same signature as compression functions.
 * @param src       Input buffer with compressed data.
 * @param srclen    Length of compressed data.
 * @param dst       Output buffer for decompressed data (pre-allocated).
 * @param dstlen    On input, size of dst. On output, actual decompressed size.
 * @param level     Compression level used during compression.
 * @param chdr      Chunk header byte.
 * @param btype     Data type hint.
 * @param data      Algorithm-private state.
 * @return 0 on success, -1 on failure.
 */
/**@{*/
extern int zlib_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int lzma_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int bzip2_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int adapt_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int ppmd_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int lz_fx_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int lz4_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int none_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
#ifdef ENABLE_PC_ZSTD
extern int zstd_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
#endif
/**@}*/

/**
 * @name Algorithm Init Functions
 * Initialize algorithm-specific state before compression or decompression.
 * @param data          Output: pointer to allocated algorithm-private state.
 * @param level         In/out: compression level (may be clamped to valid range).
 * @param nthreads      Number of worker threads.
 * @param chunksize     Chunk size in bytes.
 * @param file_version  File format version for backward compatibility.
 * @param op            COMPRESS or DECOMPRESS.
 * @return 0 on success, -1 on failure.
 */
/**@{*/
extern int adapt_init(void **data, int *level, int nthreads, uint64_t chunksize,
		      int file_version, compress_op_t op);
extern int adapt2_init(void **data, int *level, int nthreads, uint64_t chunksize,
		       int file_version, compress_op_t op);
extern int lzma_init(void **data, int *level, int nthreads, uint64_t chunksize,
		     int file_version, compress_op_t op);
extern int ppmd_init(void **data, int *level, int nthreads, uint64_t chunksize,
		     int file_version, compress_op_t op);
extern int bzip2_init(void **data, int *level, int nthreads, uint64_t chunksize,
		      int file_version, compress_op_t op);
extern int zlib_init(void **data, int *level, int nthreads, uint64_t chunksize,
		     int file_version, compress_op_t op);
extern int lz_fx_init(void **data, int *level, int nthreads, uint64_t chunksize,
		      int file_version, compress_op_t op);
extern int lz4_init(void **data, int *level, int nthreads, uint64_t chunksize,
		    int file_version, compress_op_t op);
extern int none_init(void **data, int *level, int nthreads, uint64_t chunksize,
		     int file_version, compress_op_t op);
#ifdef ENABLE_PC_ZSTD
extern int zstd_init(void **data, int *level, int nthreads, uint64_t chunksize,
		     int file_version, compress_op_t op);
#endif
/**@}*/

/** Set the data analyzer context for adaptive compression mode. */
extern void adapt_set_analyzer_ctx(void *data, analyzer_ctx_t *actx);

/**
 * @name Algorithm Properties Functions
 * Populate algo_props_t with buffer requirements and threading capabilities.
 * @param data       Output: algorithm properties structure.
 * @param level      Compression level.
 * @param chunksize  Chunk size in bytes.
 */
/**@{*/
extern void lzma_props(algo_props_t *data, int level, uint64_t chunksize);
extern void lzma_mt_props(algo_props_t *data, int level, uint64_t chunksize);
extern void lz4_props(algo_props_t *data, int level, uint64_t chunksize);
extern void zlib_props(algo_props_t *data, int level, uint64_t chunksize);
extern void ppmd_props(algo_props_t *data, int level, uint64_t chunksize);
extern void lz_fx_props(algo_props_t *data, int level, uint64_t chunksize);
extern void bzip2_props(algo_props_t *data, int level, uint64_t chunksize);
extern void adapt_props(algo_props_t *data, int level, uint64_t chunksize);
extern void none_props(algo_props_t *data, int level, uint64_t chunksize);
#ifdef ENABLE_PC_ZSTD
extern void zstd_props(algo_props_t *data, int level, uint64_t chunksize);
#endif
/**@}*/

/**
 * @name Algorithm Deinit Functions
 * Release algorithm-specific state allocated by the corresponding init function.
 * @param data  In/out: pointer to algorithm-private state. Set to NULL on return.
 * @return 0 on success.
 */
/**@{*/
extern int zlib_deinit(void **data);
extern int adapt_deinit(void **data);
extern int lzma_deinit(void **data);
extern int ppmd_deinit(void **data);
extern int lz_fx_deinit(void **data);
extern int lz4_deinit(void **data);
extern int none_deinit(void **data);
#ifdef ENABLE_PC_ZSTD
extern int zstd_deinit(void **data);
#endif
/**@}*/

/**
 * @name Algorithm Statistics Functions
 * Print compression statistics when the -C flag is used.
 * @param show  Controls verbosity: 0 = summary, 1 = detailed.
 */
/**@{*/
extern void adapt_stats(int show);
extern void ppmd_stats(int show);
extern void lzma_stats(int show);
extern void bzip2_stats(int show);
extern void zlib_stats(int show);
extern void lz_fx_stats(int show);
extern void lz4_stats(int show);
extern void none_stats(int show);
#ifdef ENABLE_PC_ZSTD
extern void zstd_stats(int show);
#endif
/**@}*/

#ifdef ENABLE_PC_LIBBSC
extern int libbsc_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int libbsc_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int libbsc_init(void **data, int *level, int nthreads, uint64_t chunksize,
	int file_version, compress_op_t op);
extern void libbsc_props(algo_props_t *data, int level, uint64_t chunksize);
extern int libbsc_deinit(void **data);
extern void libbsc_stats(int show);
#endif

/**
 * @brief Main Pcompress context structure.
 *
 * Holds all configuration, state, and function pointers for a compression
 * or decompression session. Created with create_pc_context(), configured
 * with init_pc_context(), and freed with destroy_pc_context().
 */
typedef struct pc_ctx {
	compress_func_ptr _compress_func;
	compress_func_ptr _decompress_func;
	init_func_ptr _init_func;
	deinit_func_ptr _deinit_func;
	stats_func_ptr _stats_func;
	props_func_ptr _props_func;

	int inited;
	int main_cancel;
	int adapt_mode;
	int pipe_mode, pipe_out;
	int nthreads;
	int hide_mem_stats;
	int hide_cmp_stats;
	int show_chunks;
	int enable_rabin_scan;
	int enable_rabin_global;
	int enable_delta_encode;
	int enable_delta2_encode;
	int delta2_nstrides;
	int enable_rabin_split;
	int enable_fixed_scan;
	int enable_analyzer;
	int preprocess_mode;
	int lzp_preprocess;
	int exe_preprocess;
	int encrypt_type;
	int archive_mode;
	int enable_archive_sort;
	long pagesize;
	int force_archive_perms;
	int no_overwrite_newer;
	int advanced_opts;
	int meta_stream;

	/*
	 * Archiving related context data.
	 */
	char archive_members_file[MAXPATHLEN];
	int archive_members_fd;
	uint32_t archive_members_count;
	void *archive_ctx, *archive_sort_buf;
	pthread_t archive_thread;
	char archive_temp_file[MAXPATHLEN];
	int archive_temp_fd;
	uint64_t archive_temp_size, archive_size;
	uchar_t *temp_mmap_buf;
	uint64_t temp_mmap_pos, temp_file_pos;
	uint64_t temp_mmap_len;
	struct fn_list *fn;
	Sem_t read_sem, write_sem;
	pthread_mutex_t write_mutex;
	uchar_t *arc_buf;
	uint64_t arc_buf_size, arc_buf_pos;
	int arc_closed, arc_writing;
	int btype, ctype;
	int interesting;
	int min_chunk;
	int enable_packjpg;
	int enable_wavpack;
	int list_mode;
	FILE *err_paths_fd;
	uint32_t errored_count;

	unsigned int chunk_num;
	uint64_t largest_chunk, smallest_chunk, avg_chunk;
	uint64_t chunksize;
	const char *algo, *filename;
	char *to_filename;
	char *exec_name;
	int do_compress, level;
	int do_uncompress;
	int cksum_bytes, mac_bytes;
	int cksum, t_errored;
	int rab_blk_size, keylen;
	crypto_ctx_t crypto_ctx;
	unsigned char *user_pw;
	int user_pw_len;
	char *pwd_file, *f_name;
	meta_ctx_t *meta_ctx;
} pc_ctx_t;

/**
 * @brief Per-thread data structure for compression and decompression.
 *
 * Each worker thread owns one instance containing pre-allocated buffers,
 * synchronization semaphores, and algorithm function pointers. Threads
 * are signaled via semaphores to process chunks in parallel while
 * maintaining output ordering.
 */
struct cmp_data {
	uchar_t *cmp_seg;
	uchar_t *compressed_chunk;
	uchar_t *uncompressed_chunk;
	dedupe_context_t *rctx;
	int64_t rbytes;
	uint64_t chunksize;
	uint64_t len_cmp, len_cmp_be;
	uchar_t checksum[CKSUM_MAX_BYTES];
	int level, cksum_mt, out_fd;
	unsigned int id;
	compress_func_ptr compress;
	compress_func_ptr decompress;
	int cancel;
	int interesting;
	Sem_t start_sem;
	Sem_t cmp_done_sem;
	Sem_t write_done_sem;
	Sem_t index_sem;
	void *data;
	pthread_t thr;
	mac_ctx_t chunk_hmac;
	algo_props_t *props;
	int decompressing;
	int btype;
	pc_ctx_t *pctx;
};

/** @name Library Public API */
/**@{*/

/** Print usage/help text to stderr. */
void usage(pc_ctx_t *pctx);

/**
 * Allocate and zero-initialize a new Pcompress context.
 * @return Pointer to allocated context, or NULL on failure.
 */
pc_ctx_t *create_pc_context(void);

/**
 * Initialize a context from a single argument string (space-delimited).
 * @param pctx  Context created by create_pc_context().
 * @param args  Space-separated argument string (as if from command line).
 * @return 0 on success, 2 if help requested, -1 on error.
 */
int init_pc_context_argstr(pc_ctx_t *pctx, char *args);

/**
 * Initialize a context from argc/argv (as from main()).
 * @param pctx  Context created by create_pc_context().
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return 0 on success, 2 if help requested, -1 on error.
 */
int init_pc_context(pc_ctx_t *pctx, int argc, char *argv[]);

/**
 * Free all resources associated with a Pcompress context.
 * @param pctx  Context to destroy. Safe to call with NULL.
 */
void destroy_pc_context(pc_ctx_t *pctx);

/**
 * Set the encryption password for a context.
 * @param pctx    Context to set password on.
 * @param pwdata  Password bytes (copied internally).
 * @param pwlen   Length of password.
 */
void pc_set_userpw(pc_ctx_t *pctx, unsigned char *pwdata, int pwlen);

/**
 * Run the main compression or decompression operation as configured.
 * @param pctx  Fully initialized context.
 * @return 0 on success, non-zero on failure.
 */
int start_pcompress(pc_ctx_t *pctx);

/**
 * Directly start compression of a single file (library API).
 * @param pctx       Initialized context.
 * @param filename   Path to input file.
 * @param chunksize  Chunk size in bytes.
 * @param level      Compression level (1-14).
 * @return 0 on success, non-zero on failure.
 */
int start_compress(pc_ctx_t *pctx, const char *filename, uint64_t chunksize, int level);

/**
 * Directly start decompression of a .pz file (library API).
 * @param pctx         Initialized context.
 * @param filename     Path to compressed .pz file.
 * @param to_filename  Output file or directory path.
 * @return 0 on success, non-zero on failure.
 */
int start_decompress(pc_ctx_t *pctx, const char *filename, char *to_filename);

/**@}*/

#ifdef	__cplusplus
}
#endif

#endif
