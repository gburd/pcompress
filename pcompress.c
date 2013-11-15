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

/*
 * pcompress - Do a chunked parallel compression/decompression of a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#if defined(sun) || defined(__sun)
#include <sys/byteorder.h>
#else
#include <byteswap.h>
#endif
#include <libgen.h>
#include <utils.h>
#include <pcompress.h>
#include <allocator.h>
#include <rabin_dedup.h>
#include <lzp.h>
#include <transpose.h>
#include <delta2/delta2.h>
#include <crypto/crypto_utils.h>
#include <crypto_xsalsa20.h>
#include <ctype.h>
#include <errno.h>
#include <pc_archive.h>

/*
 * We use 8MB chunks by default.
 */
#define	DEFAULT_CHUNKSIZE	(8 * 1024 * 1024)
#define	EIGHTY_PCT(x) ((x) - ((x)/5))

struct wdata {
	struct cmp_data **dary;
	int wfd;
	int nprocs;
	int64_t chunksize;
	pc_ctx_t *pctx;
};

pthread_mutex_t opt_parse = PTHREAD_MUTEX_INITIALIZER;

static void * writer_thread(void *dat);
static int init_algo(pc_ctx_t *pctx, const char *algo, int bail);
extern uint32_t lzma_crc32(const uint8_t *buf, uint64_t size, uint32_t crc);

void DLL_EXPORT
usage(pc_ctx_t *pctx)
{
	fprintf(stderr,
	    "\nPcompress Version %s\n\n"
	    "Usage:\n"
	    "1) To compress a file:\n"
	    "   %s -c <algorithm> [-l <compress level>] [-s <chunk size>] <file> [<target file>]\n"
	    "   Where <algorithm> can be the folowing:\n"
	    "   lzfx   - Very fast and small algorithm based on LZF.\n"
	    "   lz4    - Ultra fast, high-throughput algorithm reaching RAM B/W at level1.\n"
	    "   zlib   - The base Zlib format compression (not Gzip).\n"
	    "   lzma   - The LZMA (Lempel-Ziv Markov) algorithm from 7Zip.\n"
	    "   lzmaMt - Multithreaded version of LZMA. This is a faster version but\n"
	    "            uses more memory for the dictionary. Thread count is balanced\n"
	    "            between chunk processing threads and algorithm threads.\n"
	    "   bzip2  - Bzip2 Algorithm from libbzip2.\n"
	    "   ppmd   - The PPMd algorithm excellent for textual data. PPMd requires\n"
	    "            at least 64MB X core-count more memory than the other modes.\n"
#ifdef ENABLE_PC_LIBBSC
	    "   libbsc - A Block Sorting Compressor using the Burrows Wheeler Transform\n"
	    "            like Bzip2 but runs faster and gives better compression than\n"
	    "            Bzip2 (See: libbsc.com).\n"
#endif
	    "   adapt  - Adaptive mode where ppmd or bzip2 will be used per chunk,\n"
	    "            depending on which one produces better compression. This mode\n"
	    "            is obviously fairly slow and requires lots of memory.\n"
	    "   adapt2 - Adaptive mode which includes ppmd and lzma. This requires\n"
	    "            more memory than adapt mode, is slower and potentially gives\n"
	    "            the best compression.\n"
	    "   none   - No compression. This is only meaningful with -D and -E so Dedupe\n"
	    "            can be done for post-processing with an external utility.\n"
	    "   <chunk_size> - This can be in bytes or can use the following suffixes:\n"
	    "            g - Gigabyte, m - Megabyte, k - Kilobyte.\n"
	    "            Larger chunks produce better compression at the cost of memory.\n"
	    "   <compress_level> - Can be a number from 0 meaning minimum and 14 meaning\n"
	    "            maximum compression.\n\n"
	    "   <target file>    - Optional argument specifying the destination compressed\n"
	    "            file. The '.pz' extension is appended. If this is '-' then\n"
	    "            compressed output goes to stdout. If this argument is omitted then\n"
	    "            source filename is used with the extension '.pz' appended.\n"
	    "2) To decompress a file compressed using above command:\n"
	    "   %s -d <compressed file> <target file>\n"
	    "3) To operate as a pipe, read from stdin and write to stdout:\n"
	    "   %s -p ...\n"
	    "4) Attempt Rabin fingerprinting based deduplication on a per-chunk basis:\n"
	    "   %s -D ...\n"
	    "5) Perform Deduplication across the entire dataset (Global Dedupe):\n"
	    "   %s -G <-D|-F> - This option requires one of '-D' or '-F' to be specified\n"
	    "             to identify the block splitting method.\n"
	    "6) Perform Delta Encoding in addition to Identical Dedupe:\n"
	    "   %s -E ... - This also implies '-D'. This checks for at least 60%% similarity.\n"
	    "   The flag can be repeated as in '-EE' to indicate at least 40%% similarity.\n\n"
	    "7) Number of threads can optionally be specified: -t <1 - 256 count>\n"
	    "8) Other flags:\n"
	    "   '-L'    - Enable LZP pre-compression. This improves compression ratio of all\n"
	    "             algorithms with some extra CPU and very low RAM overhead.\n"
	    "   '-P'    - Enable Adaptive Delta Encoding. It can improve compresion ratio for\n"
	    "             data containing tables of numerical values especially if those are in\n"
	    "             an arithmetic series.\n"
	    "   NOTE    - Both -L and -P can be used together to give maximum benefit on most.\n"
	    "             datasets.\n"
	    "   '-S' <cksum>\n"
	    "           - Specify chunk checksum to use:\n\n",
	    UTILITY_VERSION, pctx->exec_name, pctx->exec_name, pctx->exec_name, pctx->exec_name,
	    pctx->exec_name, pctx->exec_name);
	list_checksums(stderr, "             ");
	fprintf(stderr, "\n"
	    "   '-F'    - Perform Fixed-Block Deduplication. Faster than '-D' but with lower\n"
	    "             deduplication ratio.\n"
	    "   '-B' <1..5>\n"
	    "           - Specify an average Dedupe block size. 1 - 4K, 2 - 8K ... 5 - 64K.\n"
	    "   '-B' 0\n"
	    "           - Use ultra-small 2KB blocks for deduplication. See README for caveats.\n"
	    "   '-M'    - Display memory allocator statistics\n"
	    "   '-C'    - Display compression statistics\n\n");
	fprintf(stderr, "\n"
	    "8) Encryption flags:\n"
	    "   '-e <ALGO>'\n"
	    "           - Encrypt chunks with the given encrption algorithm. The ALGO parameter\n"
	    "             can be one of AES or SALSA20. Both are used in CTR stream encryption\n"
	    "             mode. The password can be prompted from the user or read from a file.\n"
	    "             Unique keys are generated every time pcompress is run even when giving\n"
	    "             the same password. Default key length is 256-bits (see -k below).\n"
	    "   '-w <pathname>'\n"
	    "           - Provide a file which contains the encryption password. This file must\n"
	    "             be readable and writable since it is zeroed out after the password is\n"
	    "             read.\n"
	    "   '-k <key length>\n"
	    "           - Specify key length. Can be 16 for 128 bit or 32 for 256 bit. Default\n"
	    "             is 32 for 256 bit keys.\n\n");
}

static void
show_compression_stats(pc_ctx_t *pctx)
{
	log_msg(LOG_INFO, 0, "\nCompression Statistics");
	log_msg(LOG_INFO, 0, "======================");
	log_msg(LOG_INFO, 0, "Total chunks           : %u", pctx->chunk_num);
	if (pctx->chunk_num == 0) {
		log_msg(LOG_INFO, 0, "No statistics to display.");
	} else {
		log_msg(LOG_INFO, 0, "Best compressed chunk  : %s(%.2f%%)",
		    bytes_to_size(pctx->smallest_chunk), (double)pctx->smallest_chunk/(double)pctx->chunksize*100);
		log_msg(LOG_INFO, 0, "Worst compressed chunk : %s(%.2f%%)",
		    bytes_to_size(pctx->largest_chunk), (double)pctx->largest_chunk/(double)pctx->chunksize*100);
		pctx->avg_chunk /= pctx->chunk_num;
		log_msg(LOG_INFO, 0, "Avg compressed chunk   : %s(%.2f%%)\n",
		    bytes_to_size(pctx->avg_chunk), (double)pctx->avg_chunk/(double)pctx->chunksize*100);
	}
}

/*
 * Wrapper functions to pre-process the buffer and then call the main compression routine.
 * At present only LZP pre-compression is used below. Some extra metadata is added:
 * 
 * Byte 0: A flag to indicate which pre-processor was used.
 * Byte 1 - Byte 8: Size of buffer after pre-processing
 * 
 * It is possible for a buffer to be only pre-processed and not compressed by the final
 * algorithm if the final one fails to compress for some reason. However the vice versa
 * is not allowed.
 */
static int
preproc_compress(pc_ctx_t *pctx, compress_func_ptr cmp_func, void *src, uint64_t srclen,
	void *dst, uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data, algo_props_t *props)
{
	uchar_t *dest = (uchar_t *)dst, type = 0;
	int64_t result;
	uint64_t _dstlen;
	DEBUG_STAT_EN(double strt, en);

	_dstlen = *dstlen;
	if (pctx->lzp_preprocess) {
		int hashsize;

		hashsize = lzp_hash_size(level);
		result = lzp_compress((const uchar_t *)src, (uchar_t *)dst, srclen,
				      hashsize, LZP_DEFAULT_LZPMINLEN, 0);
		if (result < 0 || result == srclen) {
			if (!pctx->enable_delta2_encode)
				return (-1);
		} else {
			type |= PREPROC_TYPE_LZP;
			srclen = result;
			memcpy(src, dst, srclen);
		}

	} else if (!pctx->enable_delta2_encode)  {
		/*
		 * Execution won't come here but just in case ...
		 */
		log_msg(LOG_ERR, 0, "Invalid preprocessing mode");
		return (-1);
	}

	if (pctx->enable_delta2_encode && props->delta2_span > 0) {
		_dstlen = srclen;
		result = delta2_encode((uchar_t *)src, srclen, (uchar_t *)dst,
				       &_dstlen, props->delta2_span);
		if (result != -1) {
			memcpy(src, dst, _dstlen);
			srclen = _dstlen;
			type |= PREPROC_TYPE_DELTA2;
		}
	}

	*dest = type;
	U64_P(dest + 1) = htonll(srclen);
	_dstlen = srclen;
	DEBUG_STAT_EN(strt = get_wtime_millis());
	result = cmp_func(src, srclen, dest+9, &_dstlen, level, chdr, btype, data);
	DEBUG_STAT_EN(en = get_wtime_millis());

	if (result > -1 && _dstlen < srclen) {
		*dest |= PREPROC_COMPRESSED;
		*dstlen = _dstlen + 9;
		DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n", get_mb_s(srclen, strt, en)));
	} else {
		DEBUG_STAT_EN(fprintf(stderr, "Chunk did not compress.\n"));
		memcpy(dest+1, src, srclen);
		*dstlen = srclen + 1;
		/*
		 * If compression failed but one of the pre-processing succeeded then
		 * type flags will be non-zero. In that case we still indicate a success
		 * result so that decompression will reverse the pre-processing. The
		 * type flags will indicate that compression was not done and the
		 * decompress routine will not be called.
		 */
		if (type > 0)
			result = 0;
	}
	return (result);
}

static int
preproc_decompress(pc_ctx_t *pctx, compress_func_ptr dec_func, void *src, uint64_t srclen,
	void *dst, uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data, algo_props_t *props)
{
	uchar_t *sorc = (uchar_t *)src, type;
	int64_t result;
	uint64_t _dstlen = *dstlen;
	DEBUG_STAT_EN(double strt, en);

	type = *sorc;
	++sorc;
	--srclen;
	if (type & PREPROC_COMPRESSED) {
		*dstlen = ntohll(U64_P(sorc));
		sorc += 8;
		srclen -= 8;
		DEBUG_STAT_EN(strt = get_wtime_millis());
		result = dec_func(sorc, srclen, dst, dstlen, level, chdr, btype, data);
		DEBUG_STAT_EN(en = get_wtime_millis());

		if (result < 0) return (result);
		DEBUG_STAT_EN(fprintf(stderr, "Chunk decompression speed %.3f MB/s\n", get_mb_s(srclen, strt, en)));
		memcpy(src, dst, *dstlen);
		srclen = *dstlen;
	} else {
		src = sorc;
	}

	if (type & PREPROC_TYPE_DELTA2) {
		result = delta2_decode((uchar_t *)src, srclen, (uchar_t *)dst, &_dstlen);
		if (result != -1) {
			memcpy(src, dst, _dstlen);
			srclen = _dstlen;
			*dstlen = _dstlen;
	} else {
			return (result);
		}
	}

	if (type & PREPROC_TYPE_LZP) {
		int hashsize;
		hashsize = lzp_hash_size(level);
		result = lzp_decompress((const uchar_t *)src, (uchar_t *)dst, srclen,
					hashsize, LZP_DEFAULT_LZPMINLEN, 0);
		if (result < 0) {
			log_msg(LOG_ERR, 0, "LZP decompression failed.");
			return (-1);
		}
		*dstlen = result;
	}

	if (!(type & (PREPROC_COMPRESSED | PREPROC_TYPE_DELTA2 | PREPROC_TYPE_LZP)) && type > 0) {
		log_msg(LOG_ERR, 0, "Invalid preprocessing flags: %d", type);
		return (-1);
	}
	return (0);
}

/*
 * This routine is called in multiple threads. Calls the decompression handler
 * as encoded in the file header. For adaptive mode the handler adapt_decompress()
 * in turns looks at the chunk header and calls the actual decompression
 * routine.
 */
static void *
perform_decompress(void *dat)
{
	struct cmp_data *tdat = (struct cmp_data *)dat;
	uint64_t _chunksize;
	uint64_t dedupe_index_sz, dedupe_data_sz, dedupe_index_sz_cmp, dedupe_data_sz_cmp;
	int rv = 0;
	unsigned int blknum;
	uchar_t checksum[CKSUM_MAX_BYTES];
	uchar_t HDR;
	uchar_t *cseg;
	pc_ctx_t *pctx;

	pctx = tdat->pctx;
redo:
	sem_wait(&tdat->start_sem);
	if (unlikely(tdat->cancel)) {
		tdat->len_cmp = 0;
		sem_post(&tdat->cmp_done_sem);
		return (0);
	}

	/*
	 * If the last read returned a 0 quit.
	 */
	if (tdat->rbytes == 0) {
		tdat->len_cmp = 0;
		goto cont;
	}

	cseg = tdat->compressed_chunk + pctx->cksum_bytes + pctx->mac_bytes;
	HDR = *cseg;
	cseg += CHUNK_FLAG_SZ;
	_chunksize = tdat->chunksize;
	if (HDR & CHSIZE_MASK) {
		uchar_t *rseg;

		tdat->rbytes -= ORIGINAL_CHUNKSZ;
		tdat->len_cmp -= ORIGINAL_CHUNKSZ;
		rseg = tdat->compressed_chunk + tdat->rbytes;
		_chunksize = ntohll(*((int64_t *)rseg));
	}

	/*
	 * If this was encrypted:
	 * Verify HMAC first before anything else and then decrypt compressed data.
	 */
	if (pctx->encrypt_type) {
		unsigned int len;
		DEBUG_STAT_EN(double strt, en);

		DEBUG_STAT_EN(strt = get_wtime_millis());
		len = pctx->mac_bytes;
		deserialize_checksum(checksum, tdat->compressed_chunk + pctx->cksum_bytes, pctx->mac_bytes);
		memset(tdat->compressed_chunk + pctx->cksum_bytes, 0, pctx->mac_bytes);
		hmac_reinit(&tdat->chunk_hmac);
		hmac_update(&tdat->chunk_hmac, (uchar_t *)&tdat->len_cmp_be, sizeof (tdat->len_cmp_be));
		hmac_update(&tdat->chunk_hmac, tdat->compressed_chunk, tdat->rbytes);
		if (HDR & CHSIZE_MASK) {
			uchar_t *rseg;
			rseg = tdat->compressed_chunk + tdat->rbytes;
			hmac_update(&tdat->chunk_hmac, rseg, ORIGINAL_CHUNKSZ);
		}
		hmac_final(&tdat->chunk_hmac, tdat->checksum, &len);
		if (memcmp(checksum, tdat->checksum, len) != 0) {
			/*
			 * HMAC verification failure is fatal.
			 */
			log_msg(LOG_ERR, 0, "Chunk %d, HMAC verification failed", tdat->id);
			pctx->main_cancel = 1;
			tdat->len_cmp = 0;
			pctx->t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "HMAC Verification speed %.3f MB/s",
			      get_mb_s(tdat->rbytes + sizeof (tdat->len_cmp_be), strt, en)));

		/*
		 * Encryption algorithm should not change the size and
		 * encryption is in-place.
		 */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		rv = crypto_buf(&(pctx->crypto_ctx), cseg, cseg, tdat->len_cmp, tdat->id);
		if (rv == -1) {
			/*
			 * Decryption failure is fatal.
			 */
			pctx->main_cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "Decryption speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	} else if (pctx->mac_bytes > 0) {
		/*
		 * Verify header CRC32 in non-crypto mode.
		 */
		uint32_t crc1, crc2;

		crc1 = htonl(U32_P(tdat->compressed_chunk + pctx->cksum_bytes));
		memset(tdat->compressed_chunk + pctx->cksum_bytes, 0, pctx->mac_bytes);
		crc2 = lzma_crc32((uchar_t *)&tdat->len_cmp_be, sizeof (tdat->len_cmp_be), 0);
		crc2 = lzma_crc32(tdat->compressed_chunk,
		    pctx->cksum_bytes + pctx->mac_bytes + CHUNK_FLAG_SZ, crc2);
		if (HDR & CHSIZE_MASK) {
			uchar_t *rseg;
			rseg = tdat->compressed_chunk + tdat->rbytes;
			crc2 = lzma_crc32(rseg, ORIGINAL_CHUNKSZ, crc2);
		}

		if (crc1 != crc2) {
			/*
			 * Header CRC32 verification failure is fatal.
			 */
			log_msg(LOG_ERR, 0, "Chunk %d, Header CRC verification failed", tdat->id);
			pctx->main_cancel = 1;
			tdat->len_cmp = 0;
			pctx->t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (NULL);
		}

		/*
		 * Now that header CRC32 was verified, recover the stored message
		 * digest.
		 */
		deserialize_checksum(tdat->checksum, tdat->compressed_chunk, pctx->cksum_bytes);
	}

	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) &&
	    (HDR & CHUNK_FLAG_DEDUP)) {
		uchar_t *cmpbuf, *ubuf;

		/* Extract various sizes from dedupe header. */
		parse_dedupe_hdr(cseg, &blknum, &dedupe_index_sz, &dedupe_data_sz,
				&dedupe_index_sz_cmp, &dedupe_data_sz_cmp, &_chunksize);
		memcpy(tdat->uncompressed_chunk, cseg, RABIN_HDR_SIZE);

		/*
		 * Uncompress the data chunk first and then uncompress the index.
		 * The uncompress routines can use extra bytes at the end for temporary
		 * state/dictionary info. Since data chunk directly follows index
		 * uncompressing index first corrupts the data.
		 */
		cmpbuf = cseg + RABIN_HDR_SIZE + dedupe_index_sz_cmp;
		ubuf = tdat->uncompressed_chunk + RABIN_HDR_SIZE + dedupe_index_sz;
		if (HDR & COMPRESSED) {
			if (HDR & CHUNK_FLAG_PREPROC) {
				rv = preproc_decompress(pctx, tdat->decompress, cmpbuf, dedupe_data_sz_cmp,
				    ubuf, &_chunksize, tdat->level, HDR, pctx->btype, tdat->data, tdat->props);
			} else {
				DEBUG_STAT_EN(double strt, en);

				DEBUG_STAT_EN(strt = get_wtime_millis());
				rv = tdat->decompress(cmpbuf, dedupe_data_sz_cmp, ubuf, &_chunksize,
				    tdat->level, HDR, pctx->btype, tdat->data);
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunk %d decompression speed %.3f MB/s\n",
						      tdat->id, get_mb_s(_chunksize, strt, en)));
			}
			if (rv == -1) {
				tdat->len_cmp = 0;
				log_msg(LOG_ERR, 0, "ERROR: Chunk %d, decompression failed.", tdat->id);
				pctx->t_errored = 1;
				goto cont;
			}
		} else {
			memcpy(ubuf, cmpbuf, _chunksize);
		}

		rv = 0;
		cmpbuf = cseg + RABIN_HDR_SIZE;
		ubuf = tdat->uncompressed_chunk + RABIN_HDR_SIZE;

		if (dedupe_index_sz >= 90 && dedupe_index_sz > dedupe_index_sz_cmp) {
			/* Index should be at least 90 bytes to have been compressed. */
			rv = lzma_decompress(cmpbuf, dedupe_index_sz_cmp, ubuf,
			    &dedupe_index_sz, tdat->rctx->level, 0, TYPE_BINARY, tdat->rctx->lzma_data);
		} else {
			memcpy(ubuf, cmpbuf, dedupe_index_sz);
		}

		/*
		 * Recover from transposed index.
		 */
		transpose(ubuf, cmpbuf, dedupe_index_sz, sizeof (uint32_t), COL);
		memcpy(ubuf, cmpbuf, dedupe_index_sz);

	} else {
		if (HDR & COMPRESSED) {
			if (HDR & CHUNK_FLAG_PREPROC) {
				rv = preproc_decompress(pctx, tdat->decompress, cseg, tdat->len_cmp,
				    tdat->uncompressed_chunk, &_chunksize, tdat->level, HDR, pctx->btype,
				    tdat->data, tdat->props);
			} else {
				DEBUG_STAT_EN(double strt, en);

				DEBUG_STAT_EN(strt = get_wtime_millis());
				rv = tdat->decompress(cseg, tdat->len_cmp, tdat->uncompressed_chunk,
				    &_chunksize, tdat->level, HDR, pctx->btype, tdat->data);
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunk decompression speed %.3f MB/s\n",
						get_mb_s(_chunksize, strt, en)));
			}
		} else {
			memcpy(tdat->uncompressed_chunk, cseg, _chunksize);
		}
	}
	tdat->len_cmp = _chunksize;

	if (rv == -1) {
		tdat->len_cmp = 0;
		log_msg(LOG_ERR, 0, "ERROR: Chunk %d, decompression failed.", tdat->id);
		pctx->t_errored = 1;
		goto cont;
	}
	/* Rebuild chunk from dedup blocks. */
	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && (HDR & CHUNK_FLAG_DEDUP)) {
		dedupe_context_t *rctx;
		uchar_t *tmp;

		rctx = tdat->rctx;
		reset_dedupe_context(tdat->rctx);
		rctx->cbuf = tdat->compressed_chunk;
		dedupe_decompress(rctx, tdat->uncompressed_chunk, &(tdat->len_cmp));
		if (!rctx->valid) {
			log_msg(LOG_ERR, 0, "ERROR: Chunk %d, dedup recovery failed.", tdat->id);
			rv = -1;
			tdat->len_cmp = 0;
			pctx->t_errored = 1;
			goto cont;
		}
		_chunksize = tdat->len_cmp;
		tmp = tdat->uncompressed_chunk;
		tdat->uncompressed_chunk = tdat->compressed_chunk;
		tdat->compressed_chunk = tmp;
		tdat->cmp_seg = tdat->uncompressed_chunk;
	} else {
		/*
		 * This chunk was not deduplicated, however we still need to down the
		 * semaphore in order to maintain proper thread coordination. We do this after
		 * decompression to achieve better concurrency. Decompression does not need
		 * to wait for the previous thread's dedupe recovery to complete.
		 */
		if (pctx->enable_rabin_global) {
			sem_wait(tdat->rctx->index_sem);
		}
	}

	if (!pctx->encrypt_type) {
		/*
		 * Re-compute checksum of original uncompressed chunk.
		 * If it does not match we set length of chunk to 0 to indicate
		 * exit to the writer thread.
		 */
		compute_checksum(checksum, pctx->cksum, tdat->uncompressed_chunk, _chunksize, tdat->cksum_mt, 1);
		if (memcmp(checksum, tdat->checksum, pctx->cksum_bytes) != 0) {
			tdat->len_cmp = 0;
			log_msg(LOG_ERR, 0, "ERROR: Chunk %d, checksums do not match.", tdat->id);
			pctx->t_errored = 1;
		}
	}

cont:
	sem_post(&tdat->cmp_done_sem);
	goto redo;
}

/*
 * File decompression routine.
 *
 * Compressed file Format
 * ----------------------
 * File Header:
 * Algorithm string:  8 bytes.
 * Version number:    2 bytes.
 * Global Flags:      2 bytes.
 * Chunk size:        8 bytes.
 * Compression Level: 4 bytes.
 *
 * Chunk Header:
 * Compressed length: 8 bytes.
 * Checksum:          Upto 64 bytes.
 * Chunk flags:       1 byte.
 * 
 * Chunk Flags, 8 bits:
 * I  I  I  I  I  I  I  I
 * |  |     |     |  |  |
 * |  '-----'     |  |  `- 0 - Uncompressed
 * |     |        |  |     1 - Compressed
 * |     |        |  |   
 * |     |        |  `---- 1 - Chunk was Deduped
 * |     |        `------- 1 - Chunk was pre-compressed
 * |     |
 * |     |                 1 - Bzip2 (Adaptive Mode)
 * |     `---------------- 2 - Lzma (Adaptive Mode)
 * |                       3 - PPMD (Adaptive Mode)
 * |
 * `---------------------- 1 - Chunk size flag (if original chunk is of variable length)
 *
 * A file trailer to indicate end.
 * Zero Compressed length: 8 zero bytes.
 */
#define UNCOMP_BAIL err = 1; goto uncomp_done

int DLL_EXPORT
start_decompress(pc_ctx_t *pctx, const char *filename, char *to_filename)
{
	char algorithm[ALGO_SZ];
	struct stat sbuf;
	struct wdata w;
	int compfd = -1, p, dedupe_flag;
	int uncompfd = -1, err, np, bail;
	int nprocs = 1, thread = 0, level;
	unsigned int i;
	unsigned short version, flags;
	int64_t chunksize, compressed_chunksize;
	struct cmp_data **dary, *tdat;
	pthread_t writer_thr;
	algo_props_t props;

	err = 0;
	flags = 0;
	thread = 0;
	dary = NULL;
	init_algo_props(&props);

	/*
	 * Open files and do sanity checks.
	 */
	if (!pctx->pipe_mode) {
		if (filename == NULL) {
			compfd = fileno(stdin);
			if (compfd == -1) {
				log_msg(LOG_ERR, 1, "fileno ");
				UNCOMP_BAIL;
			}
			sbuf.st_size = 0;
		} else {
			if ((compfd = open(filename, O_RDONLY, 0)) == -1) {
				log_msg(LOG_ERR, 1, "Cannot open: %s", filename);
				return (1);
			}

			if (fstat(compfd, &sbuf) == -1) {
				log_msg(LOG_ERR, 1, "Cannot stat: %s", filename);
				return (1);
			}
			if (sbuf.st_size == 0)
				return (1);
		}
	} else {
		compfd = fileno(stdin);
		if (compfd == -1) {
			log_msg(LOG_ERR, 1, "fileno ");
			UNCOMP_BAIL;
		}
	}

	/*
	 * Read file header pieces and verify.
	 */
	if (Read(compfd, algorithm, ALGO_SZ) < ALGO_SZ) {
		log_msg(LOG_ERR, 1, "Read: ");
		UNCOMP_BAIL;
	}
	if (init_algo(pctx, algorithm, 0) != 0) {
		if (pctx->pipe_mode || filename == NULL)
			log_msg(LOG_ERR, 0, "Input stream is not pcompressed.");
		else
			log_msg(LOG_ERR, 0, "%s is not a pcompressed file.", filename);
		UNCOMP_BAIL;
	}
	pctx->algo = algorithm;

	if (Read(compfd, &version, sizeof (version)) < sizeof (version) ||
	    Read(compfd, &flags, sizeof (flags)) < sizeof (flags) ||
	    Read(compfd, &chunksize, sizeof (chunksize)) < sizeof (chunksize) ||
	    Read(compfd, &level, sizeof (level)) < sizeof (level)) {
		log_msg(LOG_ERR, 1, "Read: ");
		UNCOMP_BAIL;
	}

	version = ntohs(version);
	flags = ntohs(flags);
	chunksize = ntohll(chunksize);
	level = ntohl(level);

	/*
	 * Check for ridiculous values (malicious tampering or otherwise).
	 */
	if (version > VERSION) {
		log_msg(LOG_ERR, 0, "Cannot handle newer archive version %d, capability %d",
			version, VERSION);
		err = 1;
		goto uncomp_done;
	}
	if (chunksize > EIGHTY_PCT(get_total_ram())) {
		log_msg(LOG_ERR, 0, "Chunk size must not exceed 80%% of total RAM.");
		err = 1;
		goto uncomp_done;
	}
	if (level > MAX_LEVEL || level < 0) {
		log_msg(LOG_ERR, 0, "Invalid compression level in header: %d", level);
		err = 1;
		goto uncomp_done;
	}
	if (version < VERSION-3) {
		log_msg(LOG_ERR, 0, "Unsupported version: %d", version);
		err = 1;
		goto uncomp_done;
	}

	/*
	 * First check for archive mode. In that case the to_filename must be a directory.
	 */
	if (flags & FLAG_ARCHIVE) {
		/*
		 * If to_filename is not set, we just use the current directory.
		 */
		if (to_filename == NULL) {
			to_filename = ".";
			pctx->to_filename = ".";
		}
		pctx->archive_mode = 1;
		if (stat(to_filename, &sbuf) == -1) {
			if (errno != ENOENT) {
				log_msg(LOG_ERR, 1, "Target path is not a directory.");
				err = 1;
				goto uncomp_done;
			}
			if (mkdir(to_filename, S_IRUSR|S_IWUSR) == -1) {
				log_msg(LOG_ERR, 1, "Unable to create target directory %s.", to_filename);
				err = 1;
				goto uncomp_done;
			}
		}
		if (!S_ISDIR(sbuf.st_mode)) {
			log_msg(LOG_ERR, 0, "Target path is not a directory.", to_filename);
			err = 1;
			goto uncomp_done;
		}
	} else {
		const char *origf;

		if (to_filename == NULL) {
			char *pos;

			/*
			 * Use unused space in archive_members_file buffer to hold generated
			 * filename so that it need not be explicitly freed at the end.
			 */
			to_filename = pctx->archive_members_file;
			pctx->to_filename = pctx->archive_members_file;
			pos = strrchr(filename, '.');
			if (pos != NULL) {
				if ((pos[0] == 'p' || pos[0] == 'P') && (pos[1] == 'z' || pos[1] == 'Z')) {
					memcpy(to_filename, filename, pos - filename);
				} else {
					pos = NULL;
				}
			}

			/*
			 * If no .pz extension is found then use <filename>.out as the
			 * decompressed file name.
			 */
			if (pos == NULL) {
				strcpy(to_filename, filename);
				strcat(to_filename, ".out");
				log_msg(LOG_WARN, 0, "Using %s for output file name.", to_filename);
			}
		}
		origf = to_filename;
		if ((to_filename = realpath(origf, NULL)) != NULL) {
			free((void *)(to_filename));
			log_msg(LOG_ERR, 0, "File %s exists", origf);
			err = 1;
			goto uncomp_done;
		}
	}


	compressed_chunksize = chunksize + CHUNK_HDR_SZ + zlib_buf_extra(chunksize);

	if (pctx->_props_func) {
		pctx->_props_func(&props, level, chunksize);
		if (chunksize + props.buf_extra > compressed_chunksize) {
			compressed_chunksize += (chunksize + props.buf_extra - 
			    compressed_chunksize);
		}
	}

	dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler
	if (flags & FLAG_DEDUP) {
		pctx->enable_rabin_scan = 1;
		dedupe_flag = RABIN_DEDUPE_SEGMENTED;

		if (flags & FLAG_DEDUP_FIXED) {
			if (version > 7) {
				if (pctx->pipe_mode) {
					log_msg(LOG_ERR, 0, "Global Deduplication is not supported with pipe mode.");
					err = 1;
					goto uncomp_done;
				}
				pctx->enable_rabin_global = 1;
				dedupe_flag = RABIN_DEDUPE_FILE_GLOBAL;
			} else {
				log_msg(LOG_ERR, 0, "Invalid file deduplication flags.");
				err = 1;
				goto uncomp_done;
			}
		}
	} else if (flags & FLAG_DEDUP_FIXED) {
		pctx->enable_fixed_scan = 1;
		dedupe_flag = RABIN_DEDUPE_FIXED;
	}

	if (flags & FLAG_SINGLE_CHUNK) {
		props.is_single_chunk = 1;
	}

	pctx->cksum = flags & CKSUM_MASK;

	/*
	 * Backward compatibility check for SKEIN in archives version 5 or below.
	 * In newer versions BLAKE uses same IDs as SKEIN.
	 */
	if (version <= 5) {
		if (pctx->cksum == CKSUM_BLAKE256) pctx->cksum = CKSUM_SKEIN256;
		if (pctx->cksum == CKSUM_BLAKE512) pctx->cksum = CKSUM_SKEIN512;
	}
	if (get_checksum_props(NULL, &(pctx->cksum), &(pctx->cksum_bytes), &(pctx->mac_bytes), 1) == -1) {
		log_msg(LOG_ERR, 0, "Invalid checksum algorithm code: %d. File corrupt ?", pctx->cksum);
		UNCOMP_BAIL;
	}

	/*
	 * Archives older than version 5 did not support MACs.
	 */
	if (version < 5)
		pctx->mac_bytes = 0;

	/*
	 * If encryption is enabled initialize crypto.
	 */
	if (flags & MASK_CRYPTO_ALG) {
		int saltlen, noncelen;
		uchar_t *salt1, *salt2;
		uchar_t nonce[MAX_NONCE], n1[MAX_NONCE];
		uchar_t pw[MAX_PW_LEN];
		int pw_len;
		mac_ctx_t hdr_mac;
		uchar_t hdr_hash1[pctx->mac_bytes], hdr_hash2[pctx->mac_bytes];
		unsigned int hlen;
		unsigned short d1;
		unsigned int d2;
		uint64_t d3;

		/*
		 * In encrypted files we do not have a normal digest. The HMAC
		 * is computed over header and encrypted data.
		 */
		pctx->cksum_bytes = 0;
		pw_len = -1;
		compressed_chunksize += pctx->mac_bytes;
		pctx->encrypt_type = flags & MASK_CRYPTO_ALG;
		if (version < 7)
			pctx->keylen = OLD_KEYLEN;

		if (pctx->encrypt_type == CRYPTO_ALG_AES) {
			noncelen = 8;
		} else if (pctx->encrypt_type == CRYPTO_ALG_SALSA20) {
			noncelen = XSALSA20_CRYPTO_NONCEBYTES;
		} else {
			log_msg(LOG_ERR, 0, "Invalid Encryption algorithm code: %d. File corrupt ?",
				pctx->encrypt_type);
			UNCOMP_BAIL;
		}
		if (Read(compfd, &saltlen, sizeof (saltlen)) < sizeof (saltlen)) {
			log_msg(LOG_ERR, 1, "Read: ");
			UNCOMP_BAIL;
		}
		saltlen = ntohl(saltlen);
		salt1 = (uchar_t *)malloc(saltlen);
		salt2 = (uchar_t *)malloc(saltlen);
		if (Read(compfd, salt1, saltlen) < saltlen) {
			free(salt1);  free(salt2);
			log_msg(LOG_ERR, 1, "Read: ");
			UNCOMP_BAIL;
		}
		deserialize_checksum(salt2, salt1, saltlen);

		if (Read(compfd, n1, noncelen) < noncelen) {
			memset(salt2, 0, saltlen);
			free(salt2);
			memset(salt1, 0, saltlen);
			free(salt1);
			log_msg(LOG_ERR, 1, "Read: ");
			UNCOMP_BAIL;
		}

		if (pctx->encrypt_type == CRYPTO_ALG_AES) {
			U64_P(nonce) = ntohll(U64_P(n1));

		} else if (pctx->encrypt_type == CRYPTO_ALG_SALSA20) {
			deserialize_checksum(nonce, n1, noncelen);
		}

		if (version > 6) {
			if (Read(compfd, &(pctx->keylen), sizeof (pctx->keylen)) < sizeof (pctx->keylen)) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				log_msg(LOG_ERR, 1, "Read: ");
				UNCOMP_BAIL;
			}
			pctx->keylen = ntohl(pctx->keylen);
		}

		if (Read(compfd, hdr_hash1, pctx->mac_bytes) < pctx->mac_bytes) {
			memset(salt2, 0, saltlen);
			free(salt2);
			memset(salt1, 0, saltlen);
			free(salt1);
			log_msg(LOG_ERR, 1, "Read: ");
			UNCOMP_BAIL;
		}
		deserialize_checksum(hdr_hash2, hdr_hash1, pctx->mac_bytes);

		if (!pctx->pwd_file && !pctx->user_pw) {
			pw_len = get_pw_string(pw,
				"Please enter decryption password", 0);
			if (pw_len == -1) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				log_msg(LOG_ERR, 0, "Failed to get password.");
				UNCOMP_BAIL;
			}
		} else if (!pctx->user_pw) {
			int fd, len;
			uchar_t zero[MAX_PW_LEN];

			/*
			 * Read password from a file and zero out the file after reading.
			 */
			memset(zero, 0, MAX_PW_LEN);
			fd = open(pctx->pwd_file, O_RDWR);
			if (fd != -1) {
				pw_len = lseek(fd, 0, SEEK_END);
				if (pw_len != -1) {
					if (pw_len > MAX_PW_LEN) pw_len = MAX_PW_LEN-1;
					lseek(fd, 0, SEEK_SET);
					len = Read(fd, pw, pw_len);
					if (len != -1 && len == pw_len) {
						pw[pw_len] = '\0';
						if (isspace(pw[pw_len - 1]))
							pw[pw_len-1] = '\0';
						lseek(fd, 0, SEEK_SET);
						Write(fd, zero, pw_len);
						len = ftruncate(fd, 0);
						/*^^^ Make compiler happy. */
					} else {
						pw_len = -1;
					}
				}
			}
			if (pw_len == -1) {
				log_msg(LOG_ERR, 1, " ");
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				log_msg(LOG_ERR, 0, "Failed to get password.");
				UNCOMP_BAIL;
			}
			close(fd);
		}

		if (pctx->user_pw) {
			if (init_crypto(&(pctx->crypto_ctx), pctx->user_pw, pctx->user_pw_len,
			    pctx->encrypt_type, salt2, saltlen, pctx->keylen, nonce, DECRYPT_FLAG) == -1) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				memset(pctx->user_pw, 0, pctx->user_pw_len);
				log_msg(LOG_ERR, 0, "Failed to initialize crypto");
				UNCOMP_BAIL;
			}
			memset(pctx->user_pw, 0, pctx->user_pw_len);
			pctx->user_pw = NULL;
			pctx->user_pw_len = 0;
		} else {
			if (init_crypto(&(pctx->crypto_ctx), pw, pw_len, pctx->encrypt_type, salt2,
			    saltlen, pctx->keylen, nonce, DECRYPT_FLAG) == -1) {
				memset(salt2, 0, saltlen);
				free(salt2);
				memset(salt1, 0, saltlen);
				free(salt1);
				memset(pw, 0, MAX_PW_LEN);
				log_msg(LOG_ERR, 0, "Failed to initialize crypto");
				UNCOMP_BAIL;
			}
			memset(pw, 0, MAX_PW_LEN);
		}
		memset(salt2, 0, saltlen);
		free(salt2);
		memset(nonce, 0, noncelen);

		/*
		 * Verify file header HMAC.
		 */
		if (hmac_init(&hdr_mac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
			log_msg(LOG_ERR, 0, "Cannot initialize header hmac.");
			UNCOMP_BAIL;
		}
		hmac_update(&hdr_mac, (uchar_t *)pctx->algo, ALGO_SZ);
		d1 = htons(version);
		hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (version));
		d1 = htons(flags);
		hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (flags));
		d3 = htonll(chunksize);
		hmac_update(&hdr_mac, (uchar_t *)&d3, sizeof (chunksize));
		d2 = htonl(level);
		hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (level));
		if (version > 6) {
			d2 = htonl(saltlen);
			hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (saltlen));
			hmac_update(&hdr_mac, salt1, saltlen);
			hmac_update(&hdr_mac, n1, noncelen);
			d2 = htonl(pctx->keylen);
			hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (pctx->keylen));
		}
		hmac_final(&hdr_mac, hdr_hash1, &hlen);
		hmac_cleanup(&hdr_mac);
		memset(salt1, 0, saltlen);
		free(salt1);
		memset(n1, 0, noncelen);
		if (memcmp(hdr_hash2, hdr_hash1, pctx->mac_bytes) != 0) {
			log_msg(LOG_ERR, 0, "Header verification failed! File tampered or wrong password.");
			UNCOMP_BAIL;
		}
	} else if (version >= 5) {
		uint32_t crc1, crc2;
		unsigned short d1;
		unsigned int d2;
		uint64_t ch;

		/*
		 * Verify file header CRC32 in non-crypto mode.
		 */
		if (Read(compfd, &crc1, sizeof (crc1)) < sizeof (crc1)) {
			log_msg(LOG_ERR, 1, "Read: ");
			UNCOMP_BAIL;
		}
		crc1 = htonl(crc1);
		pctx->mac_bytes = sizeof (uint32_t);

		crc2 = lzma_crc32((uchar_t *)pctx->algo, ALGO_SZ, 0);
		d1 = htons(version);
		crc2 = lzma_crc32((uchar_t *)&d1, sizeof (version), crc2);
		d1 = htons(flags);
		crc2 = lzma_crc32((uchar_t *)&d1, sizeof (version), crc2);
		ch = htonll(chunksize);
		crc2 = lzma_crc32((uchar_t *)&ch, sizeof (ch), crc2);
		d2 = htonl(level);
		crc2 = lzma_crc32((uchar_t *)&d2, sizeof (level), crc2);
		if (crc1 != crc2) {
			log_msg(LOG_ERR, 0, "Header verification failed! File tampered or wrong password.");
			UNCOMP_BAIL;
		}
	}

	if (flags & FLAG_ARCHIVE) {
		if (pctx->enable_rabin_global) {
			strcpy(pctx->archive_temp_file, to_filename);
			strcat(pctx->archive_temp_file, "/.data");
			if ((pctx->archive_temp_fd = open(pctx->archive_temp_file,
			O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) == -1) {
				log_msg(LOG_ERR, 1, "Cannot open temporary data file in target directory.");
				UNCOMP_BAIL;
			}
			add_fname(pctx->archive_temp_file);
		}
		uncompfd = -1;
		if (setup_extractor(pctx) == -1) {
			log_msg(LOG_ERR, 0, "Setup of extraction context failed.");
			UNCOMP_BAIL;
		}

		if (start_extractor(pctx) == -1) {
			log_msg(LOG_ERR, 0, "Unable to start extraction thread.");
			UNCOMP_BAIL;
		}
	} else {
		if (!pctx->pipe_mode) {
			if ((uncompfd = open(to_filename, O_WRONLY|O_CREAT|O_TRUNC,
			    S_IRUSR|S_IWUSR)) == -1) {
				log_msg(LOG_ERR, 1, "Cannot open: %s", to_filename);
				UNCOMP_BAIL;
			}
		} else {
			uncompfd = fileno(stdout);
			if (uncompfd == -1) {
				log_msg(LOG_ERR, 1, "fileno ");
				UNCOMP_BAIL;
			}
		}
	}

	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (pctx->nthreads > 0 && pctx->nthreads < nprocs)
		nprocs = pctx->nthreads;
	else
		pctx->nthreads = nprocs;

	set_threadcounts(&props, &(pctx->nthreads), nprocs, DECOMPRESS_THREADS);
	if (props.is_single_chunk)
		pctx->nthreads = 1;
	if (pctx->nthreads * props.nthreads > 1)
		log_msg(LOG_INFO, 0, "Scaling to %d threads", pctx->nthreads * props.nthreads);
	else
		log_msg(LOG_INFO, 0, "Scaling to 1 thread");
	nprocs = pctx->nthreads;
	slab_cache_add(compressed_chunksize);
	slab_cache_add(chunksize);
	slab_cache_add(sizeof (struct cmp_data));

	dary = (struct cmp_data **)slab_calloc(NULL, nprocs, sizeof (struct cmp_data *));
	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			log_msg(LOG_ERR, 0, "1: Out of memory");
			UNCOMP_BAIL;
		}
		tdat = dary[i];
		tdat->pctx = pctx;
		tdat->compressed_chunk = NULL;
		tdat->uncompressed_chunk = NULL;
		tdat->chunksize = chunksize;
		tdat->compress = pctx->_compress_func;
		tdat->decompress = pctx->_decompress_func;
		tdat->cancel = 0;
		tdat->decompressing = 1;
		if (props.is_single_chunk) {
			tdat->cksum_mt = 1;
			if (version == 6) {
				tdat->cksum_mt = 2; // Indicate old format parallel hash
			}
		} else {
			tdat->cksum_mt = 0;
		}
		tdat->level = level;
		tdat->data = NULL;
		tdat->props = &props;
		sem_init(&(tdat->start_sem), 0, 0);
		sem_init(&(tdat->cmp_done_sem), 0, 0);
		sem_init(&(tdat->write_done_sem), 0, 1);
		sem_init(&(tdat->index_sem), 0, 0);

		if (pctx->_init_func) {
			if (pctx->_init_func(&(tdat->data), &(tdat->level), props.nthreads, chunksize,
			    version, DECOMPRESS) != 0) {
				UNCOMP_BAIL;
			}
		}
		if (pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) {
			tdat->rctx = create_dedupe_context(chunksize, compressed_chunksize, pctx->rab_blk_size,
			    pctx->algo, &props, pctx->enable_delta_encode, dedupe_flag, version, DECOMPRESS, 0,
			    NULL, pctx->pipe_mode, nprocs);
			if (tdat->rctx == NULL) {
				UNCOMP_BAIL;
			}
			if (pctx->enable_rabin_global) {
				if (pctx->archive_mode) {
					if ((tdat->rctx->out_fd = open(pctx->archive_temp_file,
					    O_RDONLY, 0)) == -1) {
						log_msg(LOG_ERR, 1, "Unable to get new read handle"
						    " to output file");
						UNCOMP_BAIL;
					}
				} else {
					if ((tdat->rctx->out_fd = open(to_filename, O_RDONLY, 0)) == -1) {
						log_msg(LOG_ERR, 1, "Unable to get new read handle"
						    " to output file");
						UNCOMP_BAIL;
					}
				}
			}
			tdat->rctx->index_sem = &(tdat->index_sem);
		} else {
			tdat->rctx = NULL;
		}

		if (pctx->encrypt_type) {
			if (hmac_init(&tdat->chunk_hmac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
				log_msg(LOG_ERR, 0, "Cannot initialize chunk hmac.");
				UNCOMP_BAIL;
			}
		}
		if (pthread_create(&(tdat->thr), NULL, perform_decompress,
		    (void *)tdat) != 0) {
			log_msg(LOG_ERR, 1, "Error in thread creation: ");
			UNCOMP_BAIL;
		}
	}
	thread = 1;

	if (pctx->enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx->index_sem_next = &(dary[(i + 1) % nprocs]->index_sem);
		}
	}
	// When doing global dedupe first thread does not wait to start dedupe recovery.
	sem_post(&(dary[0]->index_sem));

	if (pctx->encrypt_type) {
		/* Erase encryption key bytes stored as a plain array. No longer reqd. */
		crypto_clean_pkey(&(pctx->crypto_ctx));
	}

	w.dary = dary;
	w.wfd = uncompfd;
	w.nprocs = nprocs;
	w.chunksize = chunksize;
	w.pctx = pctx;
	if (pthread_create(&writer_thr, NULL, writer_thread, (void *)(&w)) != 0) {
		log_msg(LOG_ERR, 1, "Error in thread creation: ");
		UNCOMP_BAIL;
	}

	/*
	 * Now read from the compressed file in variable compressed chunk size.
	 * First the size is read from the chunk header and then as many bytes +
	 * checksum size are read and passed to decompression thread.
	 * Chunk sequencing is ensured.
	 */
	pctx->chunk_num = 0;
	np = 0;
	bail = 0;
	while (!bail) {
		int64_t rb;

		if (pctx->main_cancel) break;
		for (p = 0; p < nprocs; p++) {
			np = p;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
			if (pctx->main_cancel) break;
			tdat->id = pctx->chunk_num;
			if (tdat->rctx) tdat->rctx->id = tdat->id;

			/*
			 * First read length of compressed chunk.
			 */
			rb = Read(compfd, &tdat->len_cmp, sizeof (tdat->len_cmp));
			if (rb != sizeof (tdat->len_cmp)) {
				if (rb < 0) log_msg(LOG_ERR, 1, "Read: ");
				else
					log_msg(LOG_ERR, 0, "Incomplete chunk %d header,"
					    "file corrupt", pctx->chunk_num);
				UNCOMP_BAIL;
			}
			tdat->len_cmp_be = tdat->len_cmp; // Needed for HMAC
			tdat->len_cmp = htonll(tdat->len_cmp);

			/*
			 * Check for ridiculous length.
			 */
			if (tdat->len_cmp > chunksize + 256) {
				log_msg(LOG_ERR, 0, "Compressed length too big for chunk: %d",
				    pctx->chunk_num);
				UNCOMP_BAIL;
			}

			/*
			 * Zero compressed len means end of file.
			 */
			if (tdat->len_cmp == 0) {
				bail = 1;
				break;
			}

			/*
			 * Delayed allocation. Allocate chunks if not already done. The compressed
			 * file format does not provide any info on how many chunks are there in
			 * order to allow pipe mode operation. So delayed allocation during
			 * decompression allows to avoid allocating per-thread chunks which will
			 * never be used. This can happen if chunk count < thread count.
			 */
			if (!tdat->compressed_chunk) {
				tdat->compressed_chunk = (uchar_t *)slab_alloc(NULL,
				    compressed_chunksize);
				if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan))
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
					    compressed_chunksize);
				else
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
					    chunksize);
				if (!tdat->compressed_chunk || !tdat->uncompressed_chunk) {
					log_msg(LOG_ERR, 0, "2: Out of memory");
					UNCOMP_BAIL;
				}
				tdat->cmp_seg = tdat->uncompressed_chunk;
			}

			if (tdat->len_cmp > pctx->largest_chunk)
				pctx->largest_chunk = tdat->len_cmp;
			if (tdat->len_cmp < pctx->smallest_chunk)
				pctx->smallest_chunk = tdat->len_cmp;
			pctx->avg_chunk += tdat->len_cmp;

			/*
			 * Now read compressed chunk including the checksum.
			 */
			tdat->rbytes = Read(compfd, tdat->compressed_chunk,
			    tdat->len_cmp + pctx->cksum_bytes + pctx->mac_bytes + CHUNK_FLAG_SZ);
			if (pctx->main_cancel) break;
			if (tdat->rbytes < tdat->len_cmp + pctx->cksum_bytes + pctx->mac_bytes + CHUNK_FLAG_SZ) {
				if (tdat->rbytes < 0) {
					log_msg(LOG_ERR, 1, "Read: ");
					UNCOMP_BAIL;
				} else {
					log_msg(LOG_ERR, 0, "Incomplete chunk %d, file corrupt.",
					    pctx->chunk_num);
					UNCOMP_BAIL;
				}
			}
			sem_post(&tdat->start_sem);
			++(pctx->chunk_num);
		}
	}


	if (!pctx->main_cancel) {
		for (p = 0; p < nprocs; p++) {
			if (p == np) continue;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
		}
	}
uncomp_done:
	if (pctx->t_errored) err = pctx->t_errored;
	if (thread) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->start_sem);
			sem_post(&tdat->cmp_done_sem);
			pthread_join(tdat->thr, NULL);
		}
		pthread_join(writer_thr, NULL);
	}

	/*
	 * Ownership and mode of target should be same as original.
	 */
	if (filename != NULL && uncompfd != -1) {
		fchmod(uncompfd, sbuf.st_mode);
		if (fchown(uncompfd, sbuf.st_uid, sbuf.st_gid) == -1)
			log_msg(LOG_ERR, 1, "Chown ");
	}
	if (dary != NULL) {
		for (i = 0; i < nprocs; i++) {
			if (!dary[i]) continue;
			if (dary[i]->uncompressed_chunk)
				slab_free(NULL, dary[i]->uncompressed_chunk);
			if (dary[i]->compressed_chunk)
				slab_free(NULL, dary[i]->compressed_chunk);
			if (pctx->_deinit_func)
				pctx->_deinit_func(&(dary[i]->data));
			if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
				destroy_dedupe_context(dary[i]->rctx);
			}
			slab_free(NULL, dary[i]);
		}
		slab_free(NULL, dary);
	}
	if (!pctx->pipe_mode) {
		if (filename && compfd != -1) close(compfd);
		if (uncompfd != -1) close(uncompfd);
	}
	if (pctx->archive_mode) {
		pthread_join(pctx->archive_thread, NULL);
		if (pctx->enable_rabin_global) {
			close(pctx->archive_temp_fd);
			unlink(pctx->archive_temp_file);
		}
	}

	if (!pctx->hide_cmp_stats) show_compression_stats(pctx);

	return (err);
}

static void *
perform_compress(void *dat) {
	struct cmp_data *tdat = (struct cmp_data *)dat;
	typeof (tdat->chunksize) _chunksize, len_cmp, dedupe_index_sz, index_size_cmp;
	int type, rv;
	uchar_t *compressed_chunk;
	int64_t rbytes;
	pc_ctx_t *pctx;

	pctx = tdat->pctx;
redo:
	sem_wait(&tdat->start_sem);
	if (unlikely(tdat->cancel)) {
		tdat->len_cmp = 0;
		sem_post(&tdat->cmp_done_sem);
		return (0);
	}

	compressed_chunk = tdat->compressed_chunk + CHUNK_FLAG_SZ;
	rbytes = tdat->rbytes;
	dedupe_index_sz = 0;
	type = COMPRESSED;

	/* Perform Dedup if enabled. */
	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
		dedupe_context_t *rctx;

		/*
		 * Compute checksum of original uncompressed chunk. When doing dedup
		 * cmp_seg hold original data instead of uncompressed_chunk. We dedup
		 * into uncompressed_chunk so that compress transforms uncompressed_chunk
		 * back into cmp_seg. Avoids an extra memcpy().
		 */
		if (!pctx->encrypt_type)
			compute_checksum(tdat->checksum, pctx->cksum, tdat->cmp_seg, tdat->rbytes,
					 tdat->cksum_mt, 1);

		rctx = tdat->rctx;
		reset_dedupe_context(tdat->rctx);
		rctx->cbuf = tdat->uncompressed_chunk;
		dedupe_index_sz = dedupe_compress(tdat->rctx, tdat->cmp_seg, &(tdat->rbytes), 0,
						  NULL, tdat->cksum_mt);
		if (!rctx->valid) {
			memcpy(tdat->uncompressed_chunk, tdat->cmp_seg, rbytes);
			tdat->rbytes = rbytes;
		}
	} else {
		/*
		 * Compute checksum of original uncompressed chunk.
		 */
		if (!pctx->encrypt_type)
			compute_checksum(tdat->checksum, pctx->cksum, tdat->uncompressed_chunk,
					 tdat->rbytes, tdat->cksum_mt, 1);
	}

	/*
	 * If doing dedup we compress rabin index and deduped data separately.
	 * The rabin index array values can pollute the compressor's dictionary thereby
	 * reducing compression effectiveness of the data chunk. So we separate them.
	 */
	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && tdat->rctx->valid) {
		uint64_t o_chunksize;
		_chunksize = tdat->rbytes - dedupe_index_sz - RABIN_HDR_SIZE;
		index_size_cmp = dedupe_index_sz;
		rv = 0;

		/*
		 * Do a matrix transpose of the index table with the hope of improving
		 * compression ratio subsequently.
		 */
		transpose(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
		    compressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz,
		    sizeof (uint32_t), ROW);
		memcpy(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
		    compressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz);

		if (dedupe_index_sz >= 90) {
			/* Compress index if it is at least 90 bytes. */
			rv = lzma_compress(tdat->uncompressed_chunk + RABIN_HDR_SIZE,
			    dedupe_index_sz, compressed_chunk + RABIN_HDR_SIZE,
			    &index_size_cmp, tdat->rctx->level, 255, TYPE_BINARY,
			    tdat->rctx->lzma_data);

			/* 
			 * If index compression fails or does not produce a smaller result
			 * retain it as is. In that case compressed size == original size
			 * and it will be handled correctly during decompression.
			 */
			if (rv != 0 || index_size_cmp >= dedupe_index_sz) {
				index_size_cmp = dedupe_index_sz;
				goto plain_index;
			}
		} else {
plain_index:
			memcpy(compressed_chunk + RABIN_HDR_SIZE,
			    tdat->uncompressed_chunk + RABIN_HDR_SIZE, dedupe_index_sz);
		}

		index_size_cmp += RABIN_HDR_SIZE;
		dedupe_index_sz += RABIN_HDR_SIZE;
		memcpy(compressed_chunk, tdat->uncompressed_chunk, RABIN_HDR_SIZE);
		o_chunksize = _chunksize;

		/* Compress data chunk. */
		if (_chunksize == 0) {
			rv = -1;
		} else if ((pctx->lzp_preprocess || pctx->enable_delta2_encode)) {
			rv = preproc_compress(pctx, tdat->compress,
			    tdat->uncompressed_chunk + dedupe_index_sz, _chunksize,
			    compressed_chunk + index_size_cmp, &_chunksize, tdat->level, 0,
			    tdat->btype, tdat->data, tdat->props);
		} else {
			DEBUG_STAT_EN(double strt, en);

			DEBUG_STAT_EN(strt = get_wtime_millis());
			rv = tdat->compress(tdat->uncompressed_chunk + dedupe_index_sz,
			    _chunksize, compressed_chunk + index_size_cmp, &_chunksize,
			    tdat->level, 0, tdat->btype, tdat->data);
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n",
					      get_mb_s(_chunksize, strt, en)));
		}

		/* Can't compress data just retain as-is. */
		if (rv < 0 || _chunksize >= o_chunksize) {
			_chunksize = o_chunksize;
			type = UNCOMPRESSED;
			memcpy(compressed_chunk + index_size_cmp,
			    tdat->uncompressed_chunk + dedupe_index_sz, _chunksize);
		}
		/* Now update rabin header with the compressed sizes. */
		update_dedupe_hdr(compressed_chunk, index_size_cmp - RABIN_HDR_SIZE, _chunksize);
		_chunksize += index_size_cmp;
	} else {
		_chunksize = tdat->rbytes;
		if (pctx->lzp_preprocess || pctx->enable_delta2_encode) {
			rv = preproc_compress(pctx, tdat->compress, tdat->uncompressed_chunk,
			    tdat->rbytes, compressed_chunk, &_chunksize, tdat->level, 0,
			    tdat->btype, tdat->data, tdat->props);
		} else {
			DEBUG_STAT_EN(double strt, en);

			DEBUG_STAT_EN(strt = get_wtime_millis());
			rv = tdat->compress(tdat->uncompressed_chunk, tdat->rbytes,
			    compressed_chunk, &_chunksize, tdat->level, 0, tdat->btype,
			    tdat->data);
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n",
					      get_mb_s(_chunksize, strt, en)));
		}
	}

	/*
	 * Sanity check to ensure compressed data is lesser than original.
	 * If at all compression expands/does not shrink data then the chunk
	 * will be left uncompressed. Also if the compression errored the
	 * chunk will be left uncompressed.
	 */
	tdat->len_cmp = _chunksize;
	if (_chunksize >= tdat->rbytes || rv < 0) {
		if (!(pctx->enable_rabin_scan || pctx->enable_fixed_scan) || !tdat->rctx->valid)
			memcpy(compressed_chunk, tdat->uncompressed_chunk, tdat->rbytes);
		type = UNCOMPRESSED;
		tdat->len_cmp = tdat->rbytes;
		if (rv < 0) rv = COMPRESS_NONE;
	}

	/*
	 * Now perform encryption on the compressed data, if requested.
	 */
	if (pctx->encrypt_type) {
		int ret;
		DEBUG_STAT_EN(double strt, en);

		/*
		 * Encryption algorithm must not change the size and
		 * encryption is in-place.
		 */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		ret = crypto_buf(&(pctx->crypto_ctx), compressed_chunk, compressed_chunk,
			tdat->len_cmp, tdat->id);
		if (ret == -1) {
			/*
			 * Encryption failure is fatal.
			 */
			pctx->main_cancel = 1;
			tdat->len_cmp = 0;
			pctx->t_errored = 1;
			sem_post(&tdat->cmp_done_sem);
			return (0);
		}
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "Encryption speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	}

	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && tdat->rctx->valid) {
		type |= CHUNK_FLAG_DEDUP;
	}
	if (pctx->lzp_preprocess || pctx->enable_delta2_encode) {
		type |= CHUNK_FLAG_PREPROC;
	}

	/*
	 * Insert compressed chunk length and checksum into chunk header.
	 */
	len_cmp = tdat->len_cmp;
	*((typeof (len_cmp) *)(tdat->cmp_seg)) = htonll(tdat->len_cmp);
	if (!pctx->encrypt_type)
		serialize_checksum(tdat->checksum, tdat->cmp_seg + sizeof (tdat->len_cmp), pctx->cksum_bytes);
	tdat->len_cmp += CHUNK_FLAG_SZ;
	tdat->len_cmp += sizeof (len_cmp);
	tdat->len_cmp += (pctx->cksum_bytes + pctx->mac_bytes);
	rbytes = tdat->len_cmp - len_cmp; // HDR size for HMAC

	if (pctx->adapt_mode)
		type |= (rv << 4);

	/*
	 * If chunk is less than max chunksize, store this length as well.
	 */
	if (tdat->rbytes < tdat->chunksize) {
		type |= CHSIZE_MASK;
		*((typeof (tdat->rbytes) *)(tdat->cmp_seg + tdat->len_cmp)) = htonll(tdat->rbytes);
		tdat->len_cmp += ORIGINAL_CHUNKSZ;
		len_cmp += ORIGINAL_CHUNKSZ;
		*((typeof (len_cmp) *)(tdat->cmp_seg)) = htonll(len_cmp);
	}
	/*
	 * Set the chunk header flags.
	 */
	*(tdat->compressed_chunk) = type;

	/*
	 * If encrypting, compute HMAC for full chunk including header.
	 */
	if (pctx->encrypt_type) {
		uchar_t *mac_ptr;
		unsigned int hlen;
		uchar_t chash[pctx->mac_bytes];
		DEBUG_STAT_EN(double strt, en);

		/* Clean out mac_bytes to 0 for stable HMAC. */
		DEBUG_STAT_EN(strt = get_wtime_millis());
		mac_ptr = tdat->cmp_seg + sizeof (tdat->len_cmp) + pctx->cksum_bytes;
		memset(mac_ptr, 0, pctx->mac_bytes);
		hmac_reinit(&tdat->chunk_hmac);
		hmac_update(&tdat->chunk_hmac, tdat->cmp_seg, tdat->len_cmp);
		hmac_final(&tdat->chunk_hmac, chash, &hlen);
		serialize_checksum(chash, mac_ptr, hlen);
		DEBUG_STAT_EN(en = get_wtime_millis());
		DEBUG_STAT_EN(fprintf(stderr, "HMAC Computation speed %.3f MB/s\n",
			      get_mb_s(tdat->len_cmp, strt, en)));
	} else {
		/*
		 * Compute header CRC32 in non-crypto mode.
		 */
		uchar_t *mac_ptr;
		uint32_t crc;

		/* Clean out mac_bytes to 0 for stable CRC32. */
		mac_ptr = tdat->cmp_seg + sizeof (tdat->len_cmp) + pctx->cksum_bytes;
		memset(mac_ptr, 0, pctx->mac_bytes);
		crc = lzma_crc32(tdat->cmp_seg, rbytes, 0);
		if (type & CHSIZE_MASK)
			crc = lzma_crc32(tdat->cmp_seg + tdat->len_cmp - ORIGINAL_CHUNKSZ,
			    ORIGINAL_CHUNKSZ, crc);
		U32_P(mac_ptr) = htonl(crc);
	}
	
	sem_post(&tdat->cmp_done_sem);
	goto redo;
}

static void *
writer_thread(void *dat) {
	int p;
	struct wdata *w = (struct wdata *)dat;
	struct cmp_data *tdat;
	int64_t wbytes;
	pc_ctx_t *pctx;

	pctx = w->pctx;
repeat:
	for (p = 0; p < w->nprocs; p++) {
		tdat = w->dary[p];
		sem_wait(&tdat->cmp_done_sem);
		if (tdat->len_cmp == 0) {
			goto do_cancel;
		}

		if (pctx->do_compress) {
			if (tdat->len_cmp > pctx->largest_chunk)
				pctx->largest_chunk = tdat->len_cmp;
			if (tdat->len_cmp < pctx->smallest_chunk)
				pctx->smallest_chunk = tdat->len_cmp;
			pctx->avg_chunk += tdat->len_cmp;
		}

		if (pctx->archive_mode && tdat->decompressing) {
			wbytes = archiver_write(pctx, tdat->cmp_seg, tdat->len_cmp);
		} else {
			wbytes = Write(w->wfd, tdat->cmp_seg, tdat->len_cmp);
		}
		if (pctx->archive_temp_fd != -1 && wbytes == tdat->len_cmp) {
			wbytes = Write(pctx->archive_temp_fd, tdat->cmp_seg, tdat->len_cmp);
		}
		if (unlikely(wbytes != tdat->len_cmp)) {
			log_msg(LOG_ERR, 1, "Chunk Write (expected: %" PRIu64 ", written: %" PRIu64 ") : ",
				tdat->len_cmp, wbytes);
do_cancel:
			pctx->main_cancel = 1;
			tdat->cancel = 1;
			sem_post(&tdat->start_sem);
			if (tdat->rctx && pctx->enable_rabin_global)
				sem_post(tdat->rctx->index_sem_next);
			sem_post(&tdat->write_done_sem);
			return (0);
		}
		if (tdat->decompressing && tdat->rctx && pctx->enable_rabin_global) {
			sem_post(tdat->rctx->index_sem_next);
		}
		sem_post(&tdat->write_done_sem);
	}
	goto repeat;
}

/*
 * File compression routine. Can use as many threads as there are
 * logical cores unless user specified something different. There is
 * not much to gain from nthreads > n logical cores however.
 */
#define COMP_BAIL err = 1; goto comp_done

int DLL_EXPORT
start_compress(pc_ctx_t *pctx, const char *filename, uint64_t chunksize, int level)
{
	struct wdata w;
	char tmpfile1[MAXPATHLEN], tmpdir[MAXPATHLEN];
	char to_filename[MAXPATHLEN];
	uint64_t compressed_chunksize, n_chunksize, file_offset;
	int64_t rbytes, rabin_count;
	unsigned short version, flags;
	struct stat sbuf;
	int compfd = -1, uncompfd = -1, err;
	int thread, wthread, bail, single_chunk;
	uint32_t i, nprocs, np, p, dedupe_flag;
	struct cmp_data **dary = NULL, *tdat;
	pthread_t writer_thr;
	uchar_t *cread_buf, *pos;
	dedupe_context_t *rctx;
	algo_props_t props;

	init_algo_props(&props);
	props.cksum = pctx->cksum;
	props.buf_extra = 0;
	cread_buf = NULL;
	pctx->btype = TYPE_UNKNOWN;
	flags = 0;
	sbuf.st_size = 0;
	err = 0;
	thread = 0;
	wthread = 0;
	dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler

	if (pctx->encrypt_type) {
		uchar_t pw[MAX_PW_LEN];
		int pw_len = -1;

		compressed_chunksize += pctx->mac_bytes;
		if (!pctx->pwd_file && !pctx->user_pw) {
			pw_len = get_pw_string(pw,
				"Please enter encryption password", 1);
			if (pw_len == -1) {
				log_msg(LOG_ERR, 0, "Failed to get password.");
				return (1);
			}
		} else if (!pctx->user_pw) {
			int fd, len;
			uchar_t zero[MAX_PW_LEN];

			/*
			 * Read password from a file and zero out the file after reading.
			 */
			memset(zero, 0, MAX_PW_LEN);
			fd = open(pctx->pwd_file, O_RDWR);
			if (fd != -1) {
				pw_len = lseek(fd, 0, SEEK_END);
				if (pw_len != -1) {
					if (pw_len > MAX_PW_LEN) pw_len = MAX_PW_LEN-1;
					lseek(fd, 0, SEEK_SET);
					len = Read(fd, pw, pw_len);
					if (len != -1 && len == pw_len) {
						pw[pw_len] = '\0';
						if (isspace(pw[pw_len - 1]))
							pw[pw_len-1] = '\0';
						lseek(fd, 0, SEEK_SET);
						Write(fd, zero, pw_len);
					} else {
						pw_len = -1;
					}
				}
			}
			if (pw_len == -1) {
				log_msg(LOG_ERR, 1, "Failed to get password.");
				return (1);
			}
			close(fd);
		}
		if (pctx->user_pw) {
			if (init_crypto(&(pctx->crypto_ctx), pctx->user_pw, pctx->user_pw_len, pctx->encrypt_type,
			    NULL, 0, pctx->keylen, 0, ENCRYPT_FLAG) == -1) {
				memset(pctx->user_pw, 0, pctx->user_pw_len);
				log_msg(LOG_ERR, 0, "Failed to initialize crypto.");
				return (1);
			}
			memset(pctx->user_pw, 0, pctx->user_pw_len);
			pctx->user_pw = NULL;
			pctx->user_pw_len = 0;
		} else {
			if (init_crypto(&(pctx->crypto_ctx), pw, pw_len, pctx->encrypt_type, NULL,
			    0, pctx->keylen, 0, ENCRYPT_FLAG) == -1) {
				memset(pw, 0, MAX_PW_LEN);
				log_msg(LOG_ERR, 0, "Failed to initialize crypto.");
				return (1);
			}
			memset(pw, 0, MAX_PW_LEN);
		}
	}

	single_chunk = 0;
	rctx = NULL;

	nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (pctx->nthreads > 0 && pctx->nthreads < nprocs)
		nprocs = pctx->nthreads;
	else
		pctx->nthreads = nprocs;

	/* A host of sanity checks. */
	if (!pctx->pipe_mode) {
		char *tmp;
		if (!(pctx->archive_mode)) {
			if ((uncompfd = open(filename, O_RDONLY, 0)) == -1) {
				log_msg(LOG_ERR, 1, "Cannot open: %s", filename);
				return (1);
			}

			if (fstat(uncompfd, &sbuf) == -1) {
				close(uncompfd);
				log_msg(LOG_ERR, 1, "Cannot stat: %s", filename);
				return (1);
			}

			if (!S_ISREG(sbuf.st_mode)) {
				close(uncompfd);
				log_msg(LOG_ERR, 0, "File %s is not a regular file.", filename);
				return (1);
			}

			if (sbuf.st_size == 0) {
				close(uncompfd);
				return (1);
			}
		} else {
			if (setup_archiver(pctx, &sbuf) == -1) {
				log_msg(LOG_ERR, 0, "Setup archiver failed.");
				return (1);
			}
		}

		/*
		 * Adjust chunk size for small files. We then get an archive with
		 * a single chunk for the entire file.
		 * This is not valid for archive mode since we cannot accurately estimate
		 * final archive size.
		 */
		if (sbuf.st_size <= chunksize && !(pctx->archive_mode)) {
			chunksize = sbuf.st_size;
			pctx->enable_rabin_split = 0; // Do not split for whole files.
			pctx->nthreads = 1;
			single_chunk = 1;
			props.is_single_chunk = 1;
			flags |= FLAG_SINGLE_CHUNK;

			/*
			 * Switch to simple Deduplication if global is enabled.
			 */
			if (pctx->enable_rabin_global) {
				unsigned short flg;
				pctx->enable_rabin_scan = 1;
				pctx->enable_rabin_global = 0;
				dedupe_flag = RABIN_DEDUPE_SEGMENTED;
				flg = FLAG_DEDUP_FIXED;
				flags &= ~flg;
			}
		} else {
			if (pctx->nthreads == 0 || pctx->nthreads > sbuf.st_size / chunksize) {
				pctx->nthreads = sbuf.st_size / chunksize;
				if (sbuf.st_size % chunksize)
					pctx->nthreads++;
			}
		}

		/*
		 * Create a temporary file to hold compressed data which is renamed at
		 * the end. The target file name is same as original file with the '.pz'
		 * extension appended unless '-' was specified to output to stdout.
		 */
		if (filename) {
			strcpy(tmpfile1, filename);
			strcpy(tmpfile1, dirname(tmpfile1));
		} else {
			char *tmp1;
			if (!(pctx->archive_mode)) {
				log_msg(LOG_ERR, 0, "Inconsistent NULL Filename when Not archiving.");
				COMP_BAIL;
			}
			tmp1 = get_temp_dir();
			strcpy(tmpfile1, tmp1);
			free(tmp1);
		}

		tmp = getenv("PCOMPRESS_CACHE_DIR");
		if (tmp == NULL || !chk_dir(tmp)) {
			strcpy(tmpdir, tmpfile1);
		} else {
			strcpy(tmpdir, tmp);
		}

		if (pctx->pipe_out) {
			compfd = fileno(stdout);
			if (compfd == -1) {
				log_msg(LOG_ERR, 1, "fileno ");
				COMP_BAIL;
			}
		} else {
			if (pctx->to_filename == NULL) {
				strcat(tmpfile1, "/.pcompXXXXXX");
				snprintf(to_filename, sizeof (to_filename), "%s" COMP_EXTN, filename);
				if ((compfd = mkstemp(tmpfile1)) == -1) {
					log_msg(LOG_ERR, 1, "mkstemp ");
					COMP_BAIL;
				}
				add_fname(tmpfile1);
			} else {
				snprintf(to_filename, sizeof (to_filename), "%s" COMP_EXTN, pctx->to_filename);
				if ((compfd = open(to_filename, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR)) == -1) {
					log_msg(LOG_ERR, 1, "open ");
					COMP_BAIL;
				}
				add_fname(to_filename);
			}
		}
	} else {
		char *tmp;

		/*
		 * Use stdin/stdout for pipe mode.
		 */
		compfd = fileno(stdout);
		if (compfd == -1) {
			log_msg(LOG_ERR, 1, "fileno ");
			COMP_BAIL;
		}
		uncompfd = fileno(stdin);
		if (uncompfd == -1) {
			log_msg(LOG_ERR, 1, "fileno ");
			COMP_BAIL;
		}

		/*
		 * Get a workable temporary dir. Required if global dedupe is enabled.
		 */
		tmp = get_temp_dir();
		strcpy(tmpdir, tmp);
		free(tmp);
	}

	if (pctx->enable_rabin_global) {
		my_sysinfo msys_info;

		get_sys_limits(&msys_info);
		global_dedupe_bufadjust(pctx->rab_blk_size, &chunksize, 0, pctx->algo, pctx->cksum,
			CKSUM_BLAKE256, sbuf.st_size, msys_info.freeram, pctx->nthreads, pctx->pipe_mode);
	}

	/*
	 * Compressed buffer size must include zlib/dedup scratch space and
	 * chunk header space.
	 * See http://www.zlib.net/manual.html#compress2
	 * 
	 * We do this unconditionally whether user mentioned zlib or not
	 * to keep it simple. While zlib scratch space is only needed at
	 * runtime, chunk header is stored in the file.
	 *
	 * See start_decompress() routine for details of chunk header.
	 * We also keep extra 8-byte space for the last chunk's size.
	 */
	compressed_chunksize = chunksize + CHUNK_HDR_SZ + zlib_buf_extra(chunksize);
	if (chunksize + props.buf_extra > compressed_chunksize) {
		compressed_chunksize += (chunksize + props.buf_extra - 
		    compressed_chunksize);
	}

	if (pctx->_props_func) {
		pctx->_props_func(&props, level, chunksize);
		if (chunksize + props.buf_extra > compressed_chunksize) {
			compressed_chunksize += (chunksize + props.buf_extra - 
			    compressed_chunksize);
		}
	}

	if (pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) {
		if (pctx->enable_rabin_global) {
			flags |= (FLAG_DEDUP | FLAG_DEDUP_FIXED);
			dedupe_flag = RABIN_DEDUPE_FILE_GLOBAL;
		} else if (pctx->enable_rabin_scan) {
			flags |= FLAG_DEDUP;
			dedupe_flag = RABIN_DEDUPE_SEGMENTED;
		} else {
			flags |= FLAG_DEDUP_FIXED;
			dedupe_flag = RABIN_DEDUPE_FIXED;
		}
		/* Additional scratch space for dedup arrays. */
		if (chunksize + dedupe_buf_extra(chunksize, 0, pctx->algo, pctx->enable_delta_encode)
		    > compressed_chunksize) {
			compressed_chunksize += (chunksize +
			    dedupe_buf_extra(chunksize, 0, pctx->algo, pctx->enable_delta_encode)) -
			    compressed_chunksize;
		}
	}

	slab_cache_add(chunksize);
	slab_cache_add(compressed_chunksize);
	slab_cache_add(sizeof (struct cmp_data));

	if (pctx->encrypt_type)
		flags |= pctx->encrypt_type;

	set_threadcounts(&props, &(pctx->nthreads), nprocs, COMPRESS_THREADS);
	if (pctx->nthreads * props.nthreads > 1)
		log_msg(LOG_INFO, 0, "Scaling to %d threads", pctx->nthreads * props.nthreads);
	else
		log_msg(LOG_INFO, 0, "Scaling to 1 thread");
	nprocs = pctx->nthreads;
	dary = (struct cmp_data **)slab_calloc(NULL, nprocs, sizeof (struct cmp_data *));
	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan))
		cread_buf = (uchar_t *)slab_alloc(NULL, compressed_chunksize);
	else
		cread_buf = (uchar_t *)slab_alloc(NULL, chunksize);
	if (!cread_buf) {
		log_msg(LOG_ERR, 0, "3: Out of memory");
		COMP_BAIL;
	}

	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			log_msg(LOG_ERR, 0, "4: Out of memory");
			COMP_BAIL;
		}
		tdat = dary[i];
		tdat->pctx = pctx;
		tdat->cmp_seg = NULL;
		tdat->chunksize = chunksize;
		tdat->compress = pctx->_compress_func;
		tdat->decompress = pctx->_decompress_func;
		tdat->uncompressed_chunk = (uchar_t *)1;
		tdat->cancel = 0;
		tdat->decompressing = 0;
		if (single_chunk)
			tdat->cksum_mt = 1;
		else
			tdat->cksum_mt = 0;
		tdat->level = level;
		tdat->data = NULL;
		tdat->rctx = NULL;
		tdat->props = &props;
		sem_init(&(tdat->start_sem), 0, 0);
		sem_init(&(tdat->cmp_done_sem), 0, 0);
		sem_init(&(tdat->write_done_sem), 0, 1);
		sem_init(&(tdat->index_sem), 0, 0);

		if (pctx->_init_func) {
			if (pctx->_init_func(&(tdat->data), &(tdat->level), props.nthreads, chunksize,
			    VERSION, COMPRESS) != 0) {
				COMP_BAIL;
			}
		}

		if (pctx->encrypt_type) {
			if (hmac_init(&tdat->chunk_hmac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
				log_msg(LOG_ERR, 0, "Cannot initialize chunk hmac.");
				COMP_BAIL;
			}
		}
		if (pthread_create(&(tdat->thr), NULL, perform_compress,
		    (void *)tdat) != 0) {
			log_msg(LOG_ERR, 1, "Error in thread creation: ");
			COMP_BAIL;
		}
	}
	thread = 1;

	/*
	 * initialize Dedupe Context here after all other allocations so that index size can be correctly
	 * computed based on free memory.
	 */
	if (pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx = create_dedupe_context(chunksize, compressed_chunksize, pctx->rab_blk_size,
			    pctx->algo, &props, pctx->enable_delta_encode, dedupe_flag, VERSION, COMPRESS, sbuf.st_size,
			    tmpdir, pctx->pipe_mode, nprocs);
			if (tdat->rctx == NULL) {
				COMP_BAIL;
			}

			tdat->rctx->index_sem = &(tdat->index_sem);
			tdat->rctx->id = i;
		}
	}
	if (pctx->enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx->index_sem_next = &(dary[(i + 1) % nprocs]->index_sem);
		}
		// When doing global dedupe first thread does not wait to access the index.
		sem_post(&(dary[0]->index_sem));
	}

	w.dary = dary;
	w.wfd = compfd;
	w.nprocs = nprocs;
	w.pctx = pctx;
	if (pthread_create(&writer_thr, NULL, writer_thread, (void *)(&w)) != 0) {
		log_msg(LOG_ERR, 1, "Error in thread creation: ");
		COMP_BAIL;
	}
	wthread = 1;

	/*
	 * Start the archiver thread if needed.
	 */
	if (pctx->archive_mode) {
		if (start_archiver(pctx) != 0) {
			COMP_BAIL;
		}
		flags |= FLAG_ARCHIVE;
	}

	/*
	 * Write out file header. First insert hdr elements into mem buffer
	 * then write out the full hdr in one shot.
	 */
	flags |= pctx->cksum;
	memset(cread_buf, 0, ALGO_SZ);
	strncpy((char *)cread_buf, pctx->algo, ALGO_SZ);
	version = htons(VERSION);
	flags = htons(flags);
	n_chunksize = htonll(chunksize);
	level = htonl(level);
	pos = cread_buf + ALGO_SZ;
	memcpy(pos, &version, sizeof (version));
	pos += sizeof (version);
	memcpy(pos, &flags, sizeof (flags));
	pos += sizeof (flags);
	memcpy(pos, &n_chunksize, sizeof (n_chunksize));
	pos += sizeof (n_chunksize);
	memcpy(pos, &level, sizeof (level));
	pos += sizeof (level);

	/*
	 * If encryption is enabled, include salt, nonce and keylen in the header
	 * to be HMAC-ed (archive version 7 and greater).
	 */
	if (pctx->encrypt_type) {
		*((int *)pos) = htonl(pctx->crypto_ctx.saltlen);
		pos += sizeof (int);
		serialize_checksum(pctx->crypto_ctx.salt, pos, pctx->crypto_ctx.saltlen);
		pos += pctx->crypto_ctx.saltlen;
		if (pctx->encrypt_type == CRYPTO_ALG_AES) {
			U64_P(pos) = htonll(U64_P(crypto_nonce(&(pctx->crypto_ctx))));
			pos += 8;

		} else if (pctx->encrypt_type == CRYPTO_ALG_SALSA20) {
			serialize_checksum(crypto_nonce(&(pctx->crypto_ctx)), pos, XSALSA20_CRYPTO_NONCEBYTES);
			pos += XSALSA20_CRYPTO_NONCEBYTES;
		}
		*((int *)pos) = htonl(pctx->keylen);
		pos += sizeof (int);
	}
	if (Write(compfd, cread_buf, pos - cread_buf) != pos - cread_buf) {
		log_msg(LOG_ERR, 1, "Write ");
		COMP_BAIL;
	}

	/*
	 * If encryption is enabled, compute header HMAC and write it.
	 */
	if (pctx->encrypt_type) {
		mac_ctx_t hdr_mac;
		uchar_t hdr_hash[pctx->mac_bytes];
		unsigned int hlen;

		if (hmac_init(&hdr_mac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
			log_msg(LOG_ERR, 0, "Cannot initialize header hmac.");
			COMP_BAIL;
		}
		hmac_update(&hdr_mac, cread_buf, pos - cread_buf);
		hmac_final(&hdr_mac, hdr_hash, &hlen);
		hmac_cleanup(&hdr_mac);

		/* Erase encryption key bytes stored as a plain array. No longer reqd. */
		crypto_clean_pkey(&(pctx->crypto_ctx));

		pos = cread_buf;
		serialize_checksum(hdr_hash, pos, hlen);
		pos += hlen;
		if (Write(compfd, cread_buf, pos - cread_buf) != pos - cread_buf) {
			log_msg(LOG_ERR, 1, "Write ");
			COMP_BAIL;
		}
	} else {
		/*
		 * Compute header CRC32 and store that. Only archive version 5 and above.
		 */
		uint32_t crc = lzma_crc32(cread_buf, pos - cread_buf, 0);
		U32_P(cread_buf) = htonl(crc);
		if (Write(compfd, cread_buf, sizeof (uint32_t)) != sizeof (uint32_t)) {
			log_msg(LOG_ERR, 1, "Write ");
			COMP_BAIL;
		}
	}

	/*
	 * Now read from the uncompressed file in 'chunksize' sized chunks, independently
	 * compress each chunk and write it out. Chunk sequencing is ensured.
	 */
	pctx->chunk_num = 0;
	np = 0;
	bail = 0;
	pctx->largest_chunk = 0;
	pctx->smallest_chunk = chunksize;
	pctx->avg_chunk = 0;
	rabin_count = 0;

	/*
	 * Read the first chunk into a spare buffer (a simple double-buffering).
	 */
	file_offset = 0;
	if (pctx->enable_rabin_split) {
		rctx = create_dedupe_context(chunksize, 0, pctx->rab_blk_size, pctx->algo, &props,
		    pctx->enable_delta_encode, pctx->enable_fixed_scan, VERSION, COMPRESS, 0, NULL,
		    pctx->pipe_mode, nprocs);
		if (pctx->archive_mode)
			rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize, &rabin_count, rctx, pctx);
		else
			rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize, &rabin_count, rctx, NULL);
	} else {
		if (pctx->archive_mode)
			rbytes = archiver_read(pctx, cread_buf, chunksize);
		else
			rbytes = Read(uncompfd, cread_buf, chunksize);
	}

	while (!bail) {
		uchar_t *tmp;

		if (pctx->main_cancel) break;
		for (p = 0; p < nprocs; p++) {
			np = p;
			tdat = dary[p];
			if (pctx->main_cancel) break;
			/* Wait for previous chunk compression to complete. */
			sem_wait(&tdat->write_done_sem);
			if (pctx->main_cancel) break;

			if (rbytes == 0) { /* EOF */
				bail = 1;
				break;
			}
			/*
			 * Delayed allocation. Allocate chunks if not already done.
			 */
			if (!tdat->cmp_seg) {
				if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
					if (single_chunk)
						tdat->cmp_seg = (uchar_t *)1;
					else
						tdat->cmp_seg = (uchar_t *)slab_alloc(NULL,
							compressed_chunksize);
					tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
						compressed_chunksize);
				} else {
					if (single_chunk)
						tdat->uncompressed_chunk = (uchar_t *)1;
					else
						tdat->uncompressed_chunk =
						    (uchar_t *)slab_alloc(NULL, chunksize);
					tdat->cmp_seg = (uchar_t *)slab_alloc(NULL,
						compressed_chunksize);
				}
				tdat->compressed_chunk = tdat->cmp_seg + COMPRESSED_CHUNKSZ +
				    pctx->cksum_bytes + pctx->mac_bytes;
				if (!tdat->cmp_seg || !tdat->uncompressed_chunk) {
					log_msg(LOG_ERR, 0, "5: Out of memory");
					COMP_BAIL;
				}
			}

			/*
			 * Once previous chunk is done swap already read buffer and
			 * it's size into the thread data.
			 * Normally it goes into uncompressed_chunk, because that's what it is.
			 * With dedup enabled however, we do some jugglery to save additional
			 * memory usage and avoid a memcpy, so it goes into the compressed_chunk
			 * area:
			 * cmp_seg -> dedup -> uncompressed_chunk -> compression -> cmp_seg
			 */
			tdat->id = pctx->chunk_num;
			tdat->rbytes = rbytes;
			tdat->btype = pctx->btype; // Have to copy btype for this buffer as pctx->btype will change
			if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global)) {
				tmp = tdat->cmp_seg;
				tdat->cmp_seg = cread_buf;
				cread_buf = tmp;
				tdat->compressed_chunk = tdat->cmp_seg + COMPRESSED_CHUNKSZ +
				    pctx->cksum_bytes + pctx->mac_bytes;
				if (tdat->rctx) tdat->rctx->file_offset = file_offset;

				/*
				 * If there is data after the last rabin boundary in the chunk, then
				 * rabin_count will be non-zero. We carry over the data to the beginning
				 * of the next chunk.
				 */
				if (rabin_count) {
					memcpy(cread_buf,
					    tdat->cmp_seg + rabin_count, rbytes - rabin_count);
					tdat->rbytes = rabin_count;
					rabin_count = rbytes - rabin_count;
				}
			} else {
				tmp = tdat->uncompressed_chunk;
				tdat->uncompressed_chunk = cread_buf;
				cread_buf = tmp;
			}
			file_offset += tdat->rbytes;

			if (rbytes < chunksize) {
				if (rbytes < 0) {
					bail = 1;
					log_msg(LOG_ERR, 1, "Read: ");
					COMP_BAIL;
				}
			}

			/* Signal the compression thread to start */
			sem_post(&tdat->start_sem);
			++(pctx->chunk_num);

			if (single_chunk) {
				rbytes = 0;
				continue;
			}

			/*
			 * Read the next buffer we want to process while previous
			 * buffer is in progress.
			 */
			if (pctx->enable_rabin_split) {
				if (pctx->archive_mode)
					rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize,
					    &rabin_count, rctx, pctx);
				else
					rbytes = Read_Adjusted(uncompfd, cread_buf, chunksize,
					    &rabin_count, rctx, NULL);
			} else {
				if (pctx->archive_mode)
					rbytes = archiver_read(pctx, cread_buf, chunksize);
				else
					rbytes = Read(uncompfd, cread_buf, chunksize);
			}
		}
	}

	if (!pctx->main_cancel) {
		/* Wait for all remaining chunks to finish. */
		for (p = 0; p < nprocs; p++) {
			if (p == np) continue;
			tdat = dary[p];
			sem_wait(&tdat->write_done_sem);
		}
	} else {
		err = 1;
	}

comp_done:
	/*
	 * First close the input fd of uncompressed data. If archiving this will cause
	 * the archive thread to exit and cleanup.
	 */
	if (!pctx->pipe_mode) {
		if (uncompfd != -1) close(uncompfd);
		if (pctx->archive_mode)
			archiver_close(pctx);
	}

	if (pctx->t_errored) err = pctx->t_errored;
	if (thread) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->cancel = 1;
			tdat->len_cmp = 0;
			sem_post(&tdat->start_sem);
			sem_post(&tdat->cmp_done_sem);
			pthread_join(tdat->thr, NULL);
			if (pctx->encrypt_type)
				hmac_cleanup(&tdat->chunk_hmac);
		}
		if (wthread)
			pthread_join(writer_thr, NULL);
	}

	if (err) {
		if (compfd != -1 && !pctx->pipe_mode && !pctx->pipe_out) {
			unlink(tmpfile1);
			rm_fname(tmpfile1);
		}
		if (filename)
			log_msg(LOG_ERR, 0, "Error compressing file: %s", filename);
		else
			log_msg(LOG_ERR, 0, "Error compressing");
	} else {
		/*
		* Write a trailer of zero chunk length.
		*/
		compressed_chunksize = 0;
		if (Write(compfd, &compressed_chunksize,
		    sizeof (compressed_chunksize)) < 0) {
			log_msg(LOG_ERR, 1, "Write ");
			err = 1;
		}

		/*
		 * Rename the temporary file to the actual compressed file
		 * unless we are in a pipe.
		 */
		if (!pctx->pipe_mode && !pctx->pipe_out) {
			/*
			 * Ownership and mode of target should be same as original.
			 */
			fchmod(compfd, sbuf.st_mode);
			if (fchown(compfd, sbuf.st_uid, sbuf.st_gid) == -1)
				log_msg(LOG_ERR, 1, "chown ");
			close(compfd);

			if (pctx->to_filename == NULL) {
				if (rename(tmpfile1, to_filename) == -1) {
					log_msg(LOG_ERR, 1, "Cannot rename temporary file ");
					unlink(tmpfile1);
				}
				rm_fname(tmpfile1);
			} else {
				rm_fname(to_filename);
			}
		}
	}
	if (dary != NULL) {
		for (i = 0; i < nprocs; i++) {
			if (!dary[i]) continue;
			if (dary[i]->uncompressed_chunk != (uchar_t *)1)
				slab_free(NULL, dary[i]->uncompressed_chunk);
			if (dary[i]->cmp_seg != (uchar_t *)1)
				slab_free(NULL, dary[i]->cmp_seg);
			if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
				destroy_dedupe_context(dary[i]->rctx);
			}
			if (pctx->_deinit_func)
				pctx->_deinit_func(&(dary[i]->data));
			slab_free(NULL, dary[i]);
		}
		slab_free(NULL, dary);
	}
	if (pctx->enable_rabin_split) destroy_dedupe_context(rctx);
	if (cread_buf != (uchar_t *)1)
		slab_free(NULL, cread_buf);
	if (!pctx->pipe_mode) {
		if (compfd != -1) close(compfd);
	}

	if (pctx->archive_mode) {
		struct fn_list *fn, *fn1;

		pthread_join(pctx->archive_thread, NULL);
		fn = pctx->fn;
		while (fn) {
			fn1 = fn;
			fn = fn->next;
			slab_free(NULL, fn1);
		}
	}
	if (!pctx->hide_cmp_stats) show_compression_stats(pctx);
	pctx->_stats_func(!pctx->hide_cmp_stats);

	return (err);
}

/*
 * Check the algorithm requested and set the callback routine pointers.
 */
static int
init_algo(pc_ctx_t *pctx, const char *algo, int bail)
{
	int rv = 1;
	char algorithm[8];

	/* Copy given string into known length buffer to avoid memcmp() overruns. */
	strncpy(algorithm, algo, 8);
	pctx->_props_func = NULL;
	if (memcmp(algorithm, "zlib", 4) == 0) {
		pctx->_compress_func = zlib_compress;
		pctx->_decompress_func = zlib_decompress;
		pctx->_init_func = zlib_init;
		pctx->_deinit_func = zlib_deinit;
		pctx->_stats_func = zlib_stats;
		pctx->_props_func = zlib_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzmaMt", 6) == 0) {
		pctx->_compress_func = lzma_compress;
		pctx->_decompress_func = lzma_decompress;
		pctx->_init_func = lzma_init;
		pctx->_deinit_func = lzma_deinit;
		pctx->_stats_func = lzma_stats;
		pctx->_props_func = lzma_mt_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzma", 4) == 0) {
		pctx->_compress_func = lzma_compress;
		pctx->_decompress_func = lzma_decompress;
		pctx->_init_func = lzma_init;
		pctx->_deinit_func = lzma_deinit;
		pctx->_stats_func = lzma_stats;
		pctx->_props_func = lzma_props;
		rv = 0;

	} else if (memcmp(algorithm, "bzip2", 5) == 0) {
		pctx->_compress_func = bzip2_compress;
		pctx->_decompress_func = bzip2_decompress;
		pctx->_init_func = bzip2_init;
		pctx->_deinit_func = NULL;
		pctx->_stats_func = bzip2_stats;
		pctx->_props_func = bzip2_props;
		rv = 0;

	} else if (memcmp(algorithm, "ppmd", 4) == 0) {
		pctx->_compress_func = ppmd_compress;
		pctx->_decompress_func = ppmd_decompress;
		pctx->_init_func = ppmd_init;
		pctx->_deinit_func = ppmd_deinit;
		pctx->_stats_func = ppmd_stats;
		pctx->_props_func = ppmd_props;
		rv = 0;

	} else if (memcmp(algorithm, "lzfx", 4) == 0) {
		pctx->_compress_func = lz_fx_compress;
		pctx->_decompress_func = lz_fx_decompress;
		pctx->_init_func = lz_fx_init;
		pctx->_deinit_func = lz_fx_deinit;
		pctx->_stats_func = lz_fx_stats;
		pctx->_props_func = lz_fx_props;
		rv = 0;

	} else if (memcmp(algorithm, "lz4", 3) == 0) {
		pctx->_compress_func = lz4_compress;
		pctx->_decompress_func = lz4_decompress;
		pctx->_init_func = lz4_init;
		pctx->_deinit_func = lz4_deinit;
		pctx->_stats_func = lz4_stats;
		pctx->_props_func = lz4_props;
		rv = 0;

	} else if (memcmp(algorithm, "none", 4) == 0) {
		pctx->_compress_func = none_compress;
		pctx->_decompress_func = none_decompress;
		pctx->_init_func = none_init;
		pctx->_deinit_func = none_deinit;
		pctx->_stats_func = none_stats;
		pctx->_props_func = none_props;
		rv = 0;

	/* adapt2 and adapt ordering of the checks matter here. */
	} else if (memcmp(algorithm, "adapt2", 6) == 0) {
		pctx->_compress_func = adapt_compress;
		pctx->_decompress_func = adapt_decompress;
		pctx->_init_func = adapt2_init;
		pctx->_deinit_func = adapt_deinit;
		pctx->_stats_func = adapt_stats;
		pctx->_props_func = adapt_props;
		pctx->adapt_mode = 1;
		rv = 0;

	} else if (memcmp(algorithm, "adapt", 5) == 0) {
		pctx->_compress_func = adapt_compress;
		pctx->_decompress_func = adapt_decompress;
		pctx->_init_func = adapt_init;
		pctx->_deinit_func = adapt_deinit;
		pctx->_stats_func = adapt_stats;
		pctx->_props_func = adapt_props;
		pctx->adapt_mode = 1;
		rv = 0;
#ifdef ENABLE_PC_LIBBSC
	} else if (memcmp(algorithm, "libbsc", 6) == 0) {
		pctx->_compress_func = libbsc_compress;
		pctx->_decompress_func = libbsc_decompress;
		pctx->_init_func = libbsc_init;
		pctx->_deinit_func = libbsc_deinit;
		pctx->_stats_func = libbsc_stats;
		pctx->_props_func = libbsc_props;
		pctx->adapt_mode = 1;
		rv = 0;
#endif
	}

	return (rv);
}

/*
 * Pcompress context handling functions.
 */
pc_ctx_t DLL_EXPORT * 
create_pc_context(void)
{
	pc_ctx_t *ctx = (pc_ctx_t *)malloc(sizeof (pc_ctx_t));

	slab_init();
	init_pcompress();
	init_archive_mod();

	memset(ctx, 0, sizeof (pc_ctx_t));
	ctx->exec_name = (char *)malloc(NAME_MAX);
	ctx->hide_mem_stats = 1;
	ctx->hide_cmp_stats = 1;
	ctx->enable_rabin_split = 1;
	ctx->rab_blk_size = -1;
	ctx->archive_temp_fd = -1;
	ctx->pagesize = sysconf(_SC_PAGE_SIZE);

	return (ctx);
}

void DLL_EXPORT
destroy_pc_context(pc_ctx_t *pctx)
{
	if (pctx->do_compress)
		free((void *)(pctx->filename));
	if (pctx->pwd_file)
		free(pctx->pwd_file);
	free((void *)(pctx->exec_name));
	slab_cleanup(pctx->hide_mem_stats);
	free(pctx);
}

int DLL_EXPORT
init_pc_context_argstr(pc_ctx_t *pctx, char *args)
{
	int ac;
	char *av[128];
	char *sptr, *tok;

	ac = 0;
	tok = strtok_r(args, " ", &sptr);

	while (tok != NULL && ac < 128) {
		av[ac++] = tok;
		tok = strtok_r(NULL, " ", &sptr);
	}
	if (ac > 0)
		return (init_pc_context(pctx, ac, av));
	return (0);
}

int DLL_EXPORT
init_pc_context(pc_ctx_t *pctx, int argc, char *argv[])
{
	int opt, num_rem, err, my_optind;
	char *pos;

	pctx->level = -1;
	err = 0;
	pctx->keylen = DEFAULT_KEYLEN;
	pctx->chunksize = DEFAULT_CHUNKSIZE;
	pos = argv[0] + strlen(argv[0]);
	while (*pos != '/' && pos > argv[0]) pos--;
	if (*pos == '/') pos++;
	strcpy(pctx->exec_name, pos);

	pthread_mutex_lock(&opt_parse);
	while ((opt = getopt(argc, argv, "dc:s:l:pt:MCDGEe:w:LPS:B:Fk:avnmK")) != -1) {
		int ovr;
		int64_t chunksize;

		switch (opt) {
		    case 'd':
			pctx->do_uncompress = 1;
			break;

		    case 'c':
			pctx->do_compress = 1;
			pctx->algo = optarg;
			if (init_algo(pctx, pctx->algo, 1) != 0) {
				log_msg(LOG_ERR, 0, "Invalid algorithm %s", optarg);
				return (1);
			}
			break;

		    case 's':
			ovr = parse_numeric(&chunksize, optarg);
			if (ovr == 1) {
				log_msg(LOG_ERR, 0, "Chunk size too large %s", optarg);
				return (1);

			} else if (ovr == 2) {
				log_msg(LOG_ERR, 0, "Invalid number %s", optarg);
				return (1);
			}
			pctx->chunksize = chunksize;

			if (pctx->chunksize < MIN_CHUNK) {
				log_msg(LOG_ERR, 0, "Minimum chunk size is %ld", MIN_CHUNK);
				return (1);
			}
			if (pctx->chunksize > EIGHTY_PCT(get_total_ram())) {
				log_msg(LOG_ERR, 0, "Chunk size must not exceed 80%% of total RAM.");
				return (1);
			}
			break;

		    case 'l':
			pctx->level = atoi(optarg);
			if (pctx->level < 0 || pctx->level > MAX_LEVEL) {
				log_msg(LOG_ERR, 0, "Compression level should be in range 0 - 14");
				return (1);
			}
			break;

		    case 'B':
			pctx->rab_blk_size = atoi(optarg);
			if (pctx->rab_blk_size < 0 || pctx->rab_blk_size > 5) {
				log_msg(LOG_ERR, 0, "Average Dedupe block size must be in range 0 (2k), 1 (4k) .. 5 (64k)");
				return (1);
			}
			break;

		    case 'p':
			pctx->pipe_mode = 1;
			break;

		    case 't':
			pctx->nthreads = atoi(optarg);
			if (pctx->nthreads < 1 || pctx->nthreads > 256) {
				log_msg(LOG_ERR, 0, "Thread count should be in range 1 - 256");
				return (1);
			}
			break;

		    case 'M':
			pctx->hide_mem_stats = 0;
			break;

		    case 'C':
			pctx->hide_cmp_stats = 0;
			break;

		    case 'D':
			pctx->enable_rabin_scan = 1;
			break;

		    case 'G':
			pctx->enable_rabin_global = 1;
			break;

		    case 'E':
			pctx->enable_rabin_scan = 1;
			if (!pctx->enable_delta_encode)
				pctx->enable_delta_encode = DELTA_NORMAL;
			else
				pctx->enable_delta_encode = DELTA_EXTRA;
			break;

		    case 'e':
			pctx->encrypt_type = get_crypto_alg(optarg);
			if (pctx->encrypt_type == 0) {
				log_msg(LOG_ERR, 0, "Invalid encryption algorithm. Should be AES or SALSA20.", optarg);
				return (1);
			}
			break;

		    case 'w':
			pctx->pwd_file = strdup(optarg);
			break;

		    case 'F':
			pctx->enable_fixed_scan = 1;
			pctx->enable_rabin_split = 0;
			break;

		    case 'L':
			pctx->lzp_preprocess = 1;
			break;

		    case 'P':
			pctx->enable_delta2_encode = 1;
			break;

		    case 'k':
			pctx->keylen = atoi(optarg);
			if ((pctx->keylen != 16 && pctx->keylen != 32) || pctx->keylen > MAX_KEYLEN) {
				log_msg(LOG_ERR, 0, "Encryption KEY length should be 16 or 32.", optarg);
				return (1);
			}
			break;

		    case 'S':
			if (get_checksum_props(optarg, &(pctx->cksum), &(pctx->cksum_bytes),
			    &(pctx->mac_bytes), 0) == -1) {
				log_msg(LOG_ERR, 0, "Invalid checksum type %s", optarg);
				return (1);
			}
			break;

		    case 'a':
			pctx->archive_mode = 1;
			break;

		    case 'v':
			pctx->verbose = 1;
			break;

		    case 'n':
			pctx->enable_archive_sort = -1;
			break;

		    case 'm':
			pctx->force_archive_perms = 1;
			break;

		    case 'K':
			pctx->no_overwrite_newer = 1;
			break;

		    case '?':
		    default:
			return (2);
			break;
		}
	}
	my_optind = optind;
	optind = 0;
	pthread_mutex_unlock(&opt_parse);

	if ((pctx->do_compress && pctx->do_uncompress) || (!pctx->do_compress && !pctx->do_uncompress)) {
		return (2);
	}

	if (pctx->level == -1 && pctx->do_compress) {
		if (memcmp(pctx->algo, "lz4", 3) == 0) {
			pctx->level = 1;
		} else {
			pctx->level = 6;
		}
	}

	/*
	 * Sorting of members when archiving is enabled for compression levels >6 (>2 for lz4),
	 * unless it is explicitly disabled via '-n'.
	 */
	if (pctx->enable_archive_sort != -1 && pctx->do_compress) {
		if ((memcmp(pctx->algo, "lz4", 3) == 0 && pctx->level > 2) || pctx->level > 6)
			pctx->enable_archive_sort = 1;
	} else {
		pctx->enable_archive_sort = 0;
	}

	if (pctx->rab_blk_size == -1) {
		if (!pctx->enable_rabin_global)
			pctx->rab_blk_size = 0;
		else
			pctx->rab_blk_size = RAB_BLK_DEFAULT;
	}

	pctx->min_chunk = MIN_CHUNK;
	if (pctx->enable_rabin_scan)
		pctx->min_chunk = RAB_MIN_CHUNK_SIZE;
	if (pctx->enable_rabin_global)
		pctx->min_chunk = RAB_MIN_CHUNK_SIZE_GLOBAL;

	/*
	 * Remaining mandatory arguments are the filenames.
	 */
	num_rem = argc - my_optind;
	if (pctx->pipe_mode && num_rem > 0 ) {
		log_msg(LOG_ERR, 0, "Filename(s) unexpected for pipe mode");
		return (1);
	}

	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && !pctx->do_compress) {
		log_msg(LOG_ERR, 0, "Deduplication is only used during compression.");
		return (1);
	}
	if (!pctx->enable_rabin_scan)
		pctx->enable_rabin_split = 0;

	if (pctx->enable_fixed_scan && (pctx->enable_rabin_scan ||
	    pctx->enable_delta_encode || pctx->enable_rabin_split)) {
		log_msg(LOG_ERR, 0, "Rabin Deduplication and Fixed block Deduplication"
		    "are mutually exclusive");
		return (1);
	}

	if (!pctx->do_compress && pctx->encrypt_type) {
		log_msg(LOG_ERR, 0, "Encryption only makes sense when compressing!");
		return (1);

	} else if (pctx->pipe_mode && pctx->encrypt_type && !pctx->pwd_file) {
		log_msg(LOG_ERR, 0, "Pipe mode requires password to be provided in a file.");
		return (1);
	}

	/*
	 * Global Deduplication can use Rabin or Fixed chunking. Default, if not specified,
	 * is to use Rabin.
	 */
	if (pctx->enable_rabin_global && !pctx->enable_rabin_scan && !pctx->enable_fixed_scan) {
		pctx->enable_rabin_scan = 1;
		pctx->enable_rabin_split = 1;
	}

	if (pctx->enable_rabin_global && pctx->enable_delta_encode) {
		log_msg(LOG_ERR, 0, "Global Deduplication does not support Delta Compression.");
		return (1);
	}

	if (num_rem == 0 && !pctx->pipe_mode) {
		log_msg(LOG_ERR, 0, "Expected at least one filename.");
		return (1);

	} else if (num_rem == 1 || num_rem == 2 || (num_rem > 0 && pctx->archive_mode)) {
		if (pctx->do_compress) {
			char apath[MAXPATHLEN];

			/*
			 * If archiving, resolve the list of pathnames on the cmdline.
			 */
			if (pctx->archive_mode) {
				struct fn_list **fn;
				int valid_paths;

				slab_cache_add(sizeof (struct fn_list));
				pctx->filename = NULL;
				fn = &(pctx->fn);
				valid_paths = 0;
				while (num_rem > 0) {
					char *filename;

					if ((filename = realpath(argv[my_optind], NULL)) != NULL) {
						free(filename);
						*fn = slab_alloc(NULL, sizeof (struct fn_list));
						(*fn)->filename = strdup(argv[my_optind]);
						(*fn)->next = NULL;
						fn = &((*fn)->next);
						valid_paths++;
					} else {
						log_msg(LOG_WARN, 1, "%s", argv[my_optind]);
					}
					num_rem--;
					my_optind++;

					/*
					 * If multiple pathnames are provided, last one must be the archive name.
					 * This check here handles that case. If only one pathname is provided
					 * then archive name can be derived and num_rem here will be 0 so it
					 * exits normally in the loop check above.
					 */
					if (num_rem == 1) break;
				}
				if (valid_paths == 0) {
					log_msg(LOG_ERR, 0, "No usable paths found to archive.");
					return (1);
				}
				if (valid_paths == 1)
					pctx->filename = pctx->fn->filename;
			} else {
				if ((pctx->filename = realpath(argv[my_optind], NULL)) == NULL) {
					log_msg(LOG_ERR, 1, "%s", argv[my_optind]);
					return (1);
				}
				num_rem--;
				my_optind++;
			}

			if (num_rem > 0) {
				if (*(argv[my_optind]) == '-') {
					pctx->to_filename = "-";
					pctx->pipe_out = 1;
					pctx->to_filename = NULL;
				} else {
					strcpy(apath, argv[my_optind]);
					strcat(apath, COMP_EXTN);
					pctx->to_filename = realpath(apath, NULL);

					/* Check if compressed file exists */
					if (pctx->to_filename != NULL) {
						log_msg(LOG_ERR, 0, "Compressed file %s exists", pctx->to_filename);
						free((void *)(pctx->to_filename));
						return (1);
					}
					pctx->to_filename = argv[my_optind];
				}
			} else {
				strcpy(apath, pctx->filename);
				strcat(apath, COMP_EXTN);
				pctx->to_filename = realpath(apath, NULL);

				/* Check if compressed file exists */
				if (pctx->to_filename != NULL) {
					log_msg(LOG_ERR, 0, "Compressed file %s exists", pctx->to_filename);
					free((void *)(pctx->to_filename));
					return (1);
				}
			}
		} else if (pctx->do_uncompress) {
			/*
			 * While decompressing, input can be stdin and output a physical file.
			 */
			if (*(argv[my_optind]) == '-') {
				pctx->filename = NULL;
			} else {
				if ((pctx->filename = realpath(argv[my_optind], NULL)) == NULL) {
					log_msg(LOG_ERR, 1, "%s", argv[my_optind]);
					return (1);
				}
			}
			if (num_rem == 2) {
				my_optind++;
				pctx->to_filename = argv[my_optind];
			} else {
				pctx->to_filename = NULL;
			}
		} else {
			return (1);
		}
	} else if (num_rem > 2) {
		log_msg(LOG_ERR, 0, "Too many filenames.");
		return (1);
	}
	pctx->main_cancel = 0;

	if (pctx->cksum == 0)
		get_checksum_props(DEFAULT_CKSUM, &(pctx->cksum), &(pctx->cksum_bytes), &(pctx->mac_bytes), 0);

	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && pctx->cksum == CKSUM_CRC64) {
		log_msg(LOG_ERR, 0, "CRC64 checksum is not suitable for Deduplication.");
		return (1);
	}

	if (!pctx->encrypt_type) {
		/*
		 * If not encrypting we compute a header CRC32.
		 */
		pctx->mac_bytes = sizeof (uint32_t); // CRC32 in non-crypto mode
	} else {
		/*
		 * When encrypting we do not compute a normal digest. The HMAC
		 * is computed over header and encrypted data.
		 */
		pctx->cksum_bytes = 0;
	}

	if (pctx->do_compress) {
		struct stat sbuf;

		if (pctx->filename && stat(pctx->filename, &sbuf) == -1) {
			log_msg(LOG_ERR, 1, "Cannot stat: %s", pctx->filename);
			return (1);
		}

		/*
		 * Selectively enable filters while compressing.
		 */
		if (pctx->archive_mode) {
			struct filter_flags ff;

			ff.enable_packjpg = 0;
			if (pctx->level > 9) ff.enable_packjpg = 1;
			init_filters(&ff);
			pctx->enable_packjpg = ff.enable_packjpg;
		}

	} else if (pctx->do_uncompress) {
		struct filter_flags ff;
		/*
		 * Enable all filters while decompressing. Obviously!
		 */
		ff.enable_packjpg = 1;
		pctx->enable_packjpg = 1;
		init_filters(&ff);
	}
	pctx->inited = 1;

	return (0);
}

int DLL_EXPORT
start_pcompress(pc_ctx_t *pctx)
{
	int err;

	if (!pctx->inited)
		return (1);

	handle_signals();
	err = 0;
	if (pctx->do_compress)
		err = start_compress(pctx, pctx->filename, pctx->chunksize, pctx->level);
	else if (pctx->do_uncompress)
		err = start_decompress(pctx, pctx->filename, pctx->to_filename);
	return (err);
}

/*
 * Setter functions for various parameters in the context.
 */
void DLL_EXPORT
pc_set_userpw(pc_ctx_t *pctx, unsigned char *pwdata, int pwlen)
{
	pctx->user_pw = pwdata;
	pctx->user_pw_len = pwlen;
}
