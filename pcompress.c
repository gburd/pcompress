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
 * pcompress - Do a chunked parallel compression/decompression and archiving
 * of one or more files.
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
#include <libgen.h>
#include <utils.h>
#include <pcompress.h>
#include <allocator.h>
#include <rabin_dedup.h>

#ifndef _MPLV2_LICENSE_
#include <lzp.h>
#endif

#include <transpose.h>
#include <delta2/delta2.h>
#include <crypto/crypto_utils.h>
#include <crypto/ossl_compat.h>
#include <crypto_xsalsa20.h>
#include <ctype.h>
#include <errno.h>
#include <pc_archive.h>
#include <filters/dispack/dis.hpp>
#include "filters/dict/DictFilter.h"

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

/*
 * Reentrant getopt for the library. getopt() keeps its state in process
 * globals (optind, optarg), which the library cannot share with the
 * program or other contexts. This one matches the CLI's long-standing
 * glibc behaviour: short options, clustering ("-dv"), attached or separate
 * arguments ("-l6", "-l 6"), options after operands ("in -w pw" works; the
 * operands are moved to the end of argv, in order), "--" ends options, and
 * "-" is an operand. Diagnostics go through log_msg, worded like glibc's.
 */
struct pc_getopt {
	int ind;	/* next argv index to scan */
	int pos;	/* position inside a clustered argument */
	int nopt, nops;	/* option words and operands seen, in order */
	char **opts, **ops;	/* scratch, argc entries each */
	char *arg;	/* argument of the last option */
	const char *prog;
};

/*
 * Returns the next option, '?' for an error, or -1 at the end. At the end
 * argv[1..] is rewritten as all option words, then all operands, each in
 * original order, and ind is the first operand (glibc's default order).
 */
static int
pc_getopt(struct pc_getopt *g, int argc, char *argv[], const char *spec)
{
	const char *o;
	char c, *a;

	g->arg = NULL;
	if (g->pos == 0) {
		while (g->ind < argc && (argv[g->ind][0] != '-' || argv[g->ind][1] == '\0'))
			g->ops[g->nops++] = argv[g->ind++];	/* operand */
		if (g->ind < argc && strcmp(argv[g->ind], "--") == 0) {
			g->opts[g->nopt++] = argv[g->ind++];
			while (g->ind < argc)
				g->ops[g->nops++] = argv[g->ind++];
		}
		if (g->ind >= argc) {
			memcpy(&argv[1], g->opts, g->nopt * sizeof (char *));
			memcpy(&argv[1 + g->nopt], g->ops, g->nops * sizeof (char *));
			g->ind = 1 + g->nopt;
			return (-1);
		}
		g->opts[g->nopt++] = argv[g->ind];
		g->pos = 1;
	}
	a = argv[g->ind];
	c = a[g->pos++];
	if (c == ':' || (o = strchr(spec, c)) == NULL) {
		log_msg(LOG_ERR, 0, "%s: invalid option -- '%c'", g->prog, c);
		if (a[g->pos] == '\0') {
			g->ind++;
			g->pos = 0;
		}
		return ('?');
	}
	if (o[1] == ':') {
		if (a[g->pos] != '\0') {
			g->arg = a + g->pos;
		} else if (g->ind + 1 < argc) {
			g->arg = argv[++g->ind];
			g->opts[g->nopt++] = g->arg;
		} else {
			log_msg(LOG_ERR, 0, "%s: option requires an argument -- '%c'",
			    g->prog, c);
			g->ind++;
			g->pos = 0;
			return ('?');
		}
		g->ind++;
		g->pos = 0;
	} else if (a[g->pos] == '\0') {
		g->ind++;
		g->pos = 0;
	}
	return (c);
}

static void * writer_thread(void *dat);
static int init_algo(pc_ctx_t *pctx, const char *algo, int bail);
extern uint32_t lzma_crc32(const uint8_t *buf, uint64_t size, uint32_t crc);

void DLL_EXPORT
usage(pc_ctx_t *pctx)
{

	fprintf(stderr,
"\nPcompress Version %s\n"
"License: %s\n\n"
"See README.md for detailed usage.\n\n"
"Standard Usage\n"
"==============\n"
"    Standard usage consists of a few common options to control basic behavior with auto-\n"
"    setting of various other parameters.\n\n"
"    Archiving\n"
"    ---------\n"
"       %s -a [-v] [-l <compress level>] [-s <chunk size>] [-c <algorithm>]\n"
"                    [<file1> <directory1> <file2> ...] [-t <number>] [-S <chunk checksum>]\n"
"                    <archive filename or '-'>\n\n"
"       Archives a given set of files and/or directories into a compressed PAX archive which\n"
"       is then compressed.\n\n"
"       -a       Enables the archive mode.\n"
"       -l <compress level>\n"
"                Select a compression level from 1 (fast) to 14 (slow). Default: 6\n\n"
"       -s <chunk size>\n"
"                Specifies the maximum chunk size to split the data for parallelism. Values\n"
"                can be in bytes or with suffix(k - KB, m - MB, g - GB). Default: 8m\n"
"                Larger chunks can produce better compression at the cost of memory.\n\n"
"       -c <algorithm>\n"
"                The compression algorithm. Default algorithm when archiving is adapt2.\n"
"       -v       Enables verbose mode.\n\n"
"       -h       Show this help.  -V  Show the version.\n\n"
"       -t <number>\n"
"                Sets the number of compression threads. Default: core count.\n"
"       -T       Disable separate metadata stream.\n"
"       -S <chunk checksum>\n"
"                The chunk verification checksum. Default: BLAKE256. Others are: CRC64, SHA256,\n"
"                SHA512, KECCAK256, KECCAK512, BLAKE256, BLAKE512.\n"
"       <archive filename>\n"
"                Pathname of the resulting archive. A '.pz' extension is automatically added\n"
"                if not already present. This can be '-' to output to stdout.\n\n"
"    Single File Compression\n"
"    -----------------------\n"
"       %s -c <algorithm> [-l <compress level>] [-s <chunk size>] [-p] [<file>]\n"
"                 [-t <number>] [-S <chunk checksum>] [<target file or '-'>]\n\n"
"       Takes a single file as input and produces a compressed file. Archiving is not performed.\n"
"       This can also work in streaming mode.\n\n"
"       -c <algorithm>\n"
"                See above. Also see section 'Compression Algorithms' in README.md for details.\n"
"       -l <compress level>\n"
"       -s <chunk size>\n"
"       -t <number>\n"
"       -S <chunk checksum>\n"
"                See above.\n"
"                Note: In singe file compression mode with adapt2 or adapt algorithm, larger\n"
"                      chunks may not necessarily produce better compression.\n"
"       -p       Make Pcompress work in streaming mode. Input is stdin, output is stdout.\n\n"
"       <target file>\n"
"                Pathname of the compressed file to be created or '-' for stdout.\n\n"
"    Decompression, Listing and Archive extraction\n"
"    ---------------------------------------------\n"
"       %s <-d|-i>  [-m] [-K] <compressed file or '-'> [<target file or directory>]\n\n"
"       -d        Extract archive to target dir or current dir.\n"
"       -i        Only list contents of the archive, do not extract.\n\n"
"       -m        Enable restoring *all* permissions, ACLs, Extended Attributes etc.\n"
"                 Equivalent to the '-p' option in tar.\n"
"       -K        Do not overwrite newer files.\n"
"       -m and -K are only meaningful if the compressed file is an archive. For single file\n"
"       compressed mode these options are ignored.\n\n"
"       <compressed file>\n"
"                 Specifies the compressed file or archive. This can be '-' to indicate reading\n"
"                 from stdin while write goes to <target file>\n\n"
"       <target file or directory>\n"
"                 If single file compression was used then this is the output file.\n"
"                 Default output name if omitted: <input filename>.out\n\n"
"                 If Archiving was done then this should be the name of a directory into which\n"
"                 extracted files are restored. Default if omitted: Current directory.\n\n",
	    UTILITY_VERSION, LICENSE_STRING, pctx->exec_name, pctx->exec_name, pctx->exec_name);
	fprintf(stderr,
"    Encryption\n"
"    ----------\n"
"       -e <ALGO> Encrypt with an authenticated cipher: AES (AES-256-GCM) or SALSA20\n"
"                 (XChaCha20-Poly1305), keyed with Argon2id from a password, which is\n"
"                 prompted for or read from a file (-w). Unique keys are generated every\n"
"                 time pcompress is run even when giving the same password.\n"
"       -w <pathname>\n"
"                 Provide a file which contains the encryption password. This file must\n"
"                 be readable and writable since it is zeroed out after the password is\n"
"                 read.\n"
"       -k <key length>\n"
"                 Accepted (16 or 32) for compatibility; keys are always 256 bits.\n\n");
}

static void
show_compression_stats(pc_ctx_t *pctx)
{
	log_msg(LOG_INFO, 0, "\nCompression Statistics");
	log_msg(LOG_INFO, 0, "======================");
	log_msg(LOG_INFO, 0, "Total chunks           : %" PRIu64, pctx->chunk_num);
	if (pctx->chunk_num == 0) {
		log_msg(LOG_INFO, 0, "No statistics to display.");
	} else {
		log_msg(LOG_INFO, 0, "Best compressed chunk  : %s(%.2f%%)",
		    bytes_to_size(pctx->smallest_chunk),
		    (double)pctx->smallest_chunk/(double)pctx->chunksize*100);
		log_msg(LOG_INFO, 0, "Worst compressed chunk : %s(%.2f%%)",
		    bytes_to_size(pctx->largest_chunk),
		    (double)pctx->largest_chunk/(double)pctx->chunksize*100);
		pctx->avg_chunk /= pctx->chunk_num;
		log_msg(LOG_INFO, 0, "Avg compressed chunk   : %s(%.2f%%)\n",
		    bytes_to_size(pctx->avg_chunk),
		    (double)pctx->avg_chunk/(double)pctx->chunksize*100);
	}
}

/*
 * Wrapper functions to pre-process the buffer and then call the main compression routine.
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
    void *dst, uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data,
    algo_props_t *props, int interesting)
{
	uchar_t *dest = (uchar_t *)dst, type = 0;
	int result;
	uint64_t _dstlen, fromlen;
	uchar_t *from, *to;
	int stype, analyzed;
	analyzer_ctx_t actx;
	DEBUG_STAT_EN(double strt, en);

	_dstlen = *dstlen;
	from = src;
	to = dst;
	fromlen = srclen;
	result = 0;
	stype = PC_SUBTYPE(btype);
	analyzed = 0;

	if (btype == TYPE_UNKNOWN || stype == TYPE_ARCHIVE_TAR || stype == TYPE_PDF ||
	    PC_TYPE(btype) & TYPE_TEXT || interesting) {
		analyze_buffer(src, srclen, &actx);
		analyzed = 1;
		if (pctx->adapt_mode)
			adapt_set_analyzer_ctx(data, &actx);
	}

	/*
	 * Dispack is used for 32-bit EXE files via a libarchive filter routine.
	 * For 64-bit exes or AR archives we apply an E8E9 CALL/JMP transform filter.
	 */
	if (pctx->exe_preprocess) {
		int processed = 0;

		if (stype == TYPE_EXE32 ||  stype == TYPE_EXE32_PE ||
		    stype == TYPE_EXE64 || stype == TYPE_ARCHIVE_AR) {
			/*
			 * If file-level Dispack did not happen for 32-bit EXEs it was
			 * most likely that the file was large. So, as a workaround,
			 * we do raw-block Dispack here. However if even this fails to
			 * get any worthwhile reduction we do E8E9 as the final
			 * fallback.
			 */
			_dstlen = fromlen;
			result = dispack_encode((uchar_t *)from, fromlen, to, &_dstlen);
			if (result != -1) {
				uchar_t *tmp;
				tmp = from;
				from = to;
				to = tmp;
				fromlen = _dstlen;
				type |= PREPROC_TYPE_DISPACK;
				processed = 1;
			}
		}

		if (!processed) {
			_dstlen = fromlen;
			memcpy(to, from, fromlen);
			if (Forward_E89(to, fromlen) == 0) {
				uchar_t *tmp;
				tmp = from;
				from = to;
				to = tmp;
				fromlen = _dstlen;
				type |= PREPROC_TYPE_E8E9;
			}
		}
	}

	/*
	 * Enabling LZP also enables the DICT filter since we are dealing with text
	 * in any case.
	 */
	if (pctx->lzp_preprocess) {
		int b_type;

		b_type = btype;
		if (analyzed) {
			b_type = actx.ten_pct.btype;
		} else {
			b_type = analyze_buffer_simple(from, fromlen);
		}

		if (PC_TYPE(b_type) & TYPE_TEXT) {
			_dstlen = fromlen;
			result = dict_encode(from, fromlen, to, &_dstlen, (stype == TYPE_DNA_SEQ));
			if (result != -1) {
				uchar_t *tmp;
				tmp = from;
				from = to;
				to = tmp;
				fromlen = _dstlen;
				type |= PREPROC_TYPE_DICT;
			}
		}
	}

#ifndef _MPLV2_LICENSE_
	if (pctx->lzp_preprocess && stype != TYPE_BMP && stype != TYPE_TIFF) {
		int hashsize, b_type;
		int64_t result;

		b_type = btype;
		if (analyzed)
			b_type = actx.thirty_pct.btype;

		if (!(PC_TYPE(b_type) & TYPE_BINARY)) {
			hashsize = lzp_hash_size(level);
			result = lzp_compress((const uchar_t *)from, to, fromlen,
					      hashsize, LZP_DEFAULT_LZPMINLEN, 0);
			if (result >= 0 && result < srclen) {
				uchar_t *tmp;
				tmp = from;
				from = to;
				to = tmp;
				fromlen = result;
				type |= PREPROC_TYPE_LZP;
			}
		}
	}
#endif

	if (pctx->enable_delta2_encode && props->delta2_span > 0 &&
	    stype != TYPE_DNA_SEQ && stype != TYPE_BMP &&
	    stype != TYPE_TIFF && stype != TYPE_MP4) {
		int b_type;

		b_type = btype;
		if (analyzed)
			b_type = actx.ten_pct.btype;

		if (!(PC_TYPE(b_type) & TYPE_TEXT)) {
			_dstlen = fromlen;
			result = delta2_encode((uchar_t *)from, fromlen, to,
					       &_dstlen, props->delta2_span,
					       pctx->delta2_nstrides);
			if (result != -1) {
				uchar_t *tmp;
				tmp = from;
				from = to;
				to = tmp;
				fromlen = _dstlen;
				type |= PREPROC_TYPE_DELTA2;
			}
		}
	}

	/*
	 * Check which is the resulting buffer. If Encoded data is already sitting
	 * in src buffer then a memcpy() is not needed.
	 * Note that from,to ptrs are swapped after every encoding stage. So if
	 * from == dst, it means that encoded data is in dst.
	 */
	if (from == dst) {
		memcpy(src, dst, fromlen);
	}
	srclen = fromlen;

	*dest = type;
	U64_P(dest + 1) = htonll(srclen);
	_dstlen = srclen;
	DEBUG_STAT_EN(strt = get_wtime_millis());
	result = cmp_func(src, srclen, dest+9, &_dstlen, level, chdr,
	    btype, data);
	DEBUG_STAT_EN(en = get_wtime_millis());
	/* actx is on this stack frame: never let adapt_compress() keep it. */
	if (pctx->adapt_mode)
		adapt_set_analyzer_ctx(data, NULL);

	if (result > -1 && _dstlen < srclen) {
		*dest |= PREPROC_COMPRESSED;
		*dstlen = _dstlen + 9;
		DEBUG_STAT_EN(fprintf(stderr, "Chunk compression speed %.3f MB/s\n",
		    get_mb_s(srclen, strt, en)));
	} else {
		DEBUG_STAT_EN(fprintf(stderr, "Chunk did not compress.\n"));
		/*
		 * If compression failed but one of the pre-processing succeeded then
		 * type flags will be non-zero. In that case we still indicate a success
		 * result so that decompression will reverse the pre-processing. The
		 * type flags will indicate that compression was not done and the
		 * decompress routine will not be called.
		 */
		if (type > 0) {
			memcpy(dest+1, src, srclen);
			*dstlen = srclen + 1;
			result = 0;
		} else {
			/*
			 * Not Compressed and not preprocessed. Ensure that we signal
			 * error upstream, so this is handled correctly.
			 */
			result = -1;
		}
	}
	return (result);
}

static int
preproc_decompress(pc_ctx_t *pctx, compress_func_ptr dec_func, void *src, uint64_t srclen,
    uint64_t src_cap, void *dst, uint64_t *dstlen, int level, uchar_t chdr, int btype,
    void *data, algo_props_t *props)
{
	uchar_t *sorc = (uchar_t *)src, type;
	int result;
	uint64_t _dstlen = *dstlen, _dstlen1 = *dstlen;
	DEBUG_STAT_EN(double strt, en);

	/*
	 * Each stage writes into dst and copies the result back into src,
	 * which (a chunk segment past its header) holds less than dst: bound
	 * every stage's output by the smaller of the two.
	 */
	if (src_cap < _dstlen1)
		_dstlen1 = _dstlen = src_cap;
	if (srclen < 1)
		return (-1);
	type = *sorc;
	++sorc;
	--srclen;
	if (type & PREPROC_COMPRESSED) {
		/* Untrusted header: need the 8-byte length, and it must fit dst. */
		if (srclen < 8)
			return (-1);
		*dstlen = ntohll(U64_P(sorc));
		if (*dstlen > _dstlen1) {
			log_msg(LOG_ERR, 0, "Chunk length %" PRIu64 " exceeds buffer, "
			    "file corrupt ?", *dstlen);
			return (-1);
		}
		sorc += 8;
		srclen -= 8;
		DEBUG_STAT_EN(strt = get_wtime_millis());
		result = dec_func(sorc, srclen, dst, dstlen, level, chdr, btype, data);
		DEBUG_STAT_EN(en = get_wtime_millis());

		if (result < 0) return (result);
		DEBUG_STAT_EN(fprintf(stderr, "Chunk decompression speed %.3f MB/s\n",
		    get_mb_s(srclen, strt, en)));
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
			_dstlen = _dstlen1;
		} else {
			log_msg(LOG_ERR, 0, "Delta2 decoding failed.");
			return (result);
		}
	}

	if (type & PREPROC_TYPE_LZP) {
#ifndef _MPLV2_LICENSE_
		int hashsize;
		int64_t result;
		hashsize = lzp_hash_size(level);
		result = lzp_decompress((const uchar_t *)src, (uchar_t *)dst, srclen,
					_dstlen1, hashsize, LZP_DEFAULT_LZPMINLEN, 0);
		if (result > 0) {
			memcpy(src, dst, result);
			srclen = result;
			*dstlen = result;
		} else {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "LZP decompression failed.");
			return ((int)result);
		}
#else
		log_msg(LOG_ERR, 0, "LZP feature not available in this build"
		    " (MPLv2). Aborting.");
		return (-1);
#endif
	}

	if (type & PREPROC_TYPE_DICT) {
		result = dict_decode(src, srclen, dst, &_dstlen);
		if (result != -1) {
			memcpy(src, dst, _dstlen);
			srclen = _dstlen;
			*dstlen = _dstlen;
			_dstlen = _dstlen1;
		} else {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "DICT decoding failed.");
			return (result);
		}
	}

	if (type & PREPROC_TYPE_E8E9) {
		_dstlen1 = srclen;
		memcpy(dst, src, srclen);
		result = Inverse_E89(dst, srclen);
		if (result != -1) {
			*dstlen = _dstlen1;
		} else {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "E8E9 decoding failed.");
			return (result);
		}

	} else if (type & PREPROC_TYPE_DISPACK) { // Backward compatibility
		result = dispack_decode((uchar_t *)src, srclen, (uchar_t *)dst, &_dstlen1);
		if (result != -1) {
			*dstlen = _dstlen1;
		} else {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Dispack decoding failed.");
			return (result);
		}
	}

	if (!(type & (PREPROC_COMPRESSED|PREPROC_TYPE_DELTA2|PREPROC_TYPE_LZP|
		      PREPROC_TYPE_DISPACK|PREPROC_TYPE_DICT|PREPROC_TYPE_E8E9))
	    && type > 0) {
		log_msg(LOG_ERR, 0, "Invalid preprocessing flags: %d", type);
		return (-1);
	}
	return (0);
}

/*
 * The associated data of a version 12 data chunk, from its stored bytes
 * at seg (length field, tag, flags, payload of plen bytes, then the
 * original size when the flags say so): the length field, the flags and
 * the original size. With pc_pz_seal()'s stream and ordinal, every byte of
 * the chunk outside the payload is bound to it. Returns the length.
 */
static size_t
pz_chunk_ad(const uchar_t *len_field, uchar_t flags, const uchar_t *orig, uchar_t ad[17])
{
	memcpy(ad, len_field, 8);
	ad[8] = flags;
	if (!(flags & CHSIZE_MASK))
		return (9);
	memcpy(ad + 9, orig, ORIGINAL_CHUNKSZ);
	return (17);
}

/*
 * Seal a finished data chunk in place (version 12): seg holds the length
 * field, the tag's place, the flags, plen payload bytes and maybe the
 * original size. The payload is encrypted and the tag written.
 */
static int
pz_seal_chunk(pc_ctx_t *pctx, uchar_t *seg, uint64_t plen, uchar_t flags, uint64_t id)
{
	uchar_t ad[17], *tag = seg + COMPRESSED_CHUNKSZ, *pay = tag + AEAD_TAGLEN + CHUNK_FLAG_SZ;
	size_t adl = pz_chunk_ad(seg, flags, pay + plen, ad);

	return (pc_pz_seal(&pctx->pz_aead, PZ_STREAM_DATA, id, ad, adl, pay, plen, tag));
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
	int rv = 0, defer_index_wait;

	log_bind(&tdat->pctx->log);
	unsigned int blknum;
	uchar_t checksum[CKSUM_MAX_BYTES];
	uchar_t HDR;
	uchar_t *cseg;
	pc_ctx_t *pctx;

	pctx = tdat->pctx;
redo:
	defer_index_wait = 0;
	Sem_Wait(&tdat->start_sem);
	/*
	 * Cancelled (by another chunk's failure or by shutdown): still hand
	 * an empty result to the writer, which may be waiting on this chunk.
	 * Returning silently left it blocked on cmp_done_sem, and the main
	 * thread on write_done_sem (~2% of decompressions of a corrupt file).
	 */
	if (unlikely(__atomic_load_n(&tdat->cancel, __ATOMIC_RELAXED) || __atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED))) {
		tdat->len_cmp = 0;
		Sem_Post(&tdat->cmp_done_sem);
		return (NULL);
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

		/* Corrupt length: the trailing original-size field must fit. */
		if (tdat->len_cmp < ORIGINAL_CHUNKSZ ||
		    tdat->rbytes < (int64_t)ORIGINAL_CHUNKSZ) {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", invalid length. File corrupt ?",
			    tdat->id);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			goto cont;
		}
		tdat->rbytes -= ORIGINAL_CHUNKSZ;
		tdat->len_cmp -= ORIGINAL_CHUNKSZ;
		rseg = tdat->compressed_chunk + tdat->rbytes;
		_chunksize = ntohll(I64_P(rseg));
		if (_chunksize > tdat->chunksize) {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", original size too large. "
			    "File corrupt ?", tdat->id);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			goto cont;
		}
	}

	/*
	 * Version 12: open the payload as chunk tdat->id of the data stream;
	 * the tag covers its length field, flags and original size too, so a
	 * moved, repeated or altered chunk fails here.
	 */
	if (pctx->pz_v12) {
		uchar_t ad[17];
		size_t adl = pz_chunk_ad((uchar_t *)&tdat->len_cmp_be, HDR,
		    tdat->compressed_chunk + tdat->rbytes, ad);

		if (pc_pz_open(&pctx->pz_aead, PZ_STREAM_DATA, tdat->id, ad, adl, cseg,
		    tdat->len_cmp, tdat->compressed_chunk) != 0) {
			log_error_code(PC_ERR_VERIFY);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", authentication failed", tdat->id);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->cmp_done_sem);
			return (NULL);
		}
	} else if (pctx->encrypt_type) {
		/*
		 * Version 11 and earlier: verify the HMAC before anything else,
		 * then decrypt.
		 */
		unsigned int len;
		DEBUG_STAT_EN(double strt, en);

		DEBUG_STAT_EN(strt = get_wtime_millis());
		len = pctx->mac_bytes;
		deserialize_checksum(checksum, tdat->compressed_chunk + pctx->cksum_bytes,
		    pctx->mac_bytes);
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
			log_error_code(PC_ERR_VERIFY);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", HMAC verification failed",
			    tdat->id);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->cmp_done_sem);
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
			log_error_code(PC_ERR_CRYPTO);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", Decryption failed", tdat->id);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->cmp_done_sem);
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
			log_error_code(PC_ERR_VERIFY);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", Header CRC verification failed",
			    tdat->id);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->cmp_done_sem);
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
		/*
		 * Untrusted sizes: the compressed parts must lie inside this
		 * chunk, and the restored parts inside the chunk buffer.
		 */
		if (tdat->len_cmp < RABIN_HDR_SIZE ||
		    dedupe_index_sz_cmp > tdat->len_cmp - RABIN_HDR_SIZE ||
		    dedupe_data_sz_cmp > tdat->len_cmp - RABIN_HDR_SIZE - dedupe_index_sz_cmp ||
		    dedupe_index_sz > tdat->chunksize || dedupe_data_sz > tdat->chunksize ||
		    _chunksize > tdat->chunksize) {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64
			    ", invalid dedupe header. File corrupt ?",
			    tdat->id);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			goto cont;
		}
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
				rv = preproc_decompress(pctx, tdat->decompress, cmpbuf,
				    dedupe_data_sz_cmp,
				    tdat->compressed_chunk + tdat->buf_size - cmpbuf,
				    ubuf, &_chunksize, tdat->level,
				    HDR, pctx->btype, tdat->data, tdat->props);
			} else {
				DEBUG_STAT_EN(double strt, en);

				DEBUG_STAT_EN(strt = get_wtime_millis());
				rv = tdat->decompress(cmpbuf, dedupe_data_sz_cmp, ubuf, &_chunksize,
				    tdat->level, HDR, pctx->btype, tdat->data);
				DEBUG_STAT_EN(en = get_wtime_millis());
				DEBUG_STAT_EN(fprintf(stderr, "Chunk %" PRIu64
				    " decompression speed %.3f MB/s\n",
						      tdat->id, get_mb_s(_chunksize, strt, en)));
			}
			if (rv == -1) {
				tdat->len_cmp = 0;
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "ERROR: Chunk %" PRIu64
				    ", decompression failed.", tdat->id);
				__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
				goto cont;
			}
		} else {
			if (_chunksize > dedupe_data_sz_cmp) {	/* stored: sizes must agree */
				tdat->len_cmp = 0;
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "ERROR: Chunk %" PRIu64
				    ", stored data size %" PRIu64
				    " exceeds its length %" PRIu64 ".", tdat->id, (uint64_t)_chunksize,
				    (uint64_t)dedupe_data_sz_cmp);
				__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
				goto cont;
			}
			memcpy(ubuf, cmpbuf, _chunksize);
		}

		rv = 0;
		cmpbuf = cseg + RABIN_HDR_SIZE;
		ubuf = tdat->uncompressed_chunk + RABIN_HDR_SIZE;

		if (dedupe_index_sz < 90 && dedupe_index_sz > dedupe_index_sz_cmp) {
			/* Stored (uncompressed) index larger than its bytes in the file. */
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", invalid dedupe index.", tdat->id);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			goto cont;
		}
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
				    tdat->compressed_chunk + tdat->buf_size - cseg,
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
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "ERROR: Chunk %" PRIu64 ", decompression failed.", tdat->id);
		__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
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
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "ERROR: Chunk %" PRIu64 ", dedup recovery failed.",
			    tdat->id);
			rv = -1;
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
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
		/* Defer the global-index token wait until after checksum verification. */
		defer_index_wait = pctx->enable_rabin_global;
	}

	if (!pctx->encrypt_type) {
		/*
		 * Re-compute checksum of original uncompressed chunk.
		 * If it does not match we set length of chunk to 0 to indicate
		 * exit to the writer thread.
		 */
		compute_checksum(checksum, pctx->cksum, tdat->uncompressed_chunk,
		    _chunksize, tdat->cksum_mt, 1);
		if (memcmp(checksum, tdat->checksum, pctx->cksum_bytes) != 0) {
			tdat->len_cmp = 0;
			log_error_code(PC_ERR_VERIFY);
			log_msg(LOG_ERR, 0, "ERROR: Chunk %" PRIu64 ", checksums do not match.",
			    tdat->id);
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
		}
	}
	if (defer_index_wait && !__atomic_load_n(&pctx->t_errored, __ATOMIC_RELAXED))
		Sem_Wait(tdat->rctx->index_sem);

cont:
	Sem_Post(&tdat->cmp_done_sem);
	/*
	 * Do not leave on another chunk's error (t_errored). Between the post
	 * above and this point the writer may write this chunk and the main
	 * thread hand this worker the next one, C; if a later chunk fails
	 * meanwhile, leaving strands C, and the writer (in order) waits for it
	 * forever. That hung about 1 in 40 test_api runs on 4-CPU runners.
	 * The cancel check after start_sem ends this loop instead.
	 */
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

/* The job's stream ends: pc_*_fd() descriptors, else stdin/stdout. */
#ifdef _WIN32
/* Binary: Windows opens stdin and stdout in text (CRLF) mode. */
#define	PC_STDIO_FD(f)	pc_win32_binfd(fileno(f))
#else
#define	PC_STDIO_FD(f)	fileno(f)
#endif
#define	PC_IN_FD(p)	((p)->in_fd >= 0 ? (p)->in_fd : PC_STDIO_FD(stdin))
#define	PC_OUT_FD(p)	((p)->out_fd >= 0 ? (p)->out_fd : PC_STDIO_FD(stdout))

/*
 * The fixed part of a .pz file header, as read by start_decompress_impl()
 * and checked by the helpers below. Each helper logs its own error and
 * returns -1; the caller then bails out to its single cleanup path.
 */
struct dec_hdr {
	unsigned short version, flags;
	int64_t chunksize;
	int level;
};

/*
 * Open the compressed input (a file, or the job's input stream in pipe
 * mode) and stat it. Returns 1, with nothing left open, if the file cannot
 * be opened or is empty (the caller returns at once, before any setup), 0
 * on success, -1 if the input stream is missing.
 */
static int
dec_open_input(pc_ctx_t *pctx, const char *filename, int *compfd, struct stat *sbuf)
{
	if (!pctx->pipe_mode && filename != NULL) {
		if ((*compfd = open(filename, O_RDONLY, 0)) == -1) {
			log_msg(LOG_ERR, 1, "Cannot open: %s", filename);
			return (1);
		}
		if (fstat(*compfd, sbuf) == -1) {
			log_msg(LOG_ERR, 1, "Cannot stat: %s", filename);
			close(*compfd);
			*compfd = -1;
			return (1);
		}
		if (sbuf->st_size == 0) {
			close(*compfd);
			*compfd = -1;
			return (1);
		}
		return (0);
	}
	if (!pctx->pipe_mode) {
		pctx->pipe_mode = 1;
		sbuf->st_size = 0;
	}
	*compfd = PC_IN_FD(pctx);
	if (*compfd == -1) {
		log_msg(LOG_ERR, 1, "fileno ");
		return (-1);
	}
	return (0);
}

/*
 * Read the algorithm name, version, flags, chunk size and level, and
 * refuse values no writer produces (tampering or corruption). algorithm
 * must outlive the job: pctx->algo points into it.
 */
static int
dec_read_header(pc_ctx_t *pctx, int compfd, const char *filename, char *algorithm,
    struct dec_hdr *h)
{
	if (Read(compfd, algorithm, ALGO_SZ) < ALGO_SZ) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	if (init_algo(pctx, algorithm, 0) != 0) {
		log_error_code(PC_ERR_FORMAT);
		if (pctx->pipe_mode || filename == NULL)
			log_msg(LOG_ERR, 0, "Input stream is not pcompressed.");
		else
			log_msg(LOG_ERR, 0, "%s is not a pcompressed file.", filename);
		return (-1);
	}
	pctx->algo = algorithm;

	if (Read(compfd, &h->version, sizeof (h->version)) < sizeof (h->version) ||
	    Read(compfd, &h->flags, sizeof (h->flags)) < sizeof (h->flags) ||
	    Read(compfd, &h->chunksize, sizeof (h->chunksize)) < sizeof (h->chunksize) ||
	    Read(compfd, &h->level, sizeof (h->level)) < sizeof (h->level)) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	h->version = ntohs(h->version);
	h->flags = ntohs(h->flags);
	h->chunksize = ntohll(h->chunksize);
	h->level = ntohl(h->level);

	/*
	 * Check for ridiculous values (malicious tampering or otherwise).
	 */
	if (h->version > VERSION_MAX) {
		log_error_code(PC_ERR_FORMAT);
		log_msg(LOG_ERR, 0, "Cannot handle newer archive version %d, capability %d",
			h->version, VERSION_MAX);
		return (-1);
	}
	if (h->chunksize > EIGHTY_PCT(get_total_ram())) {
		log_msg(LOG_ERR, 0, "Chunk size must not exceed 80%% of total RAM.");
		return (-1);
	}
	if (h->level > MAX_LEVEL || h->level < 0) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Invalid compression level in header: %d", h->level);
		return (-1);
	}
	if (h->version < VERSION-4) {
		log_error_code(PC_ERR_FORMAT);
		log_msg(LOG_ERR, 0, "Unsupported version: %d", h->version);
		return (-1);
	}
	return (0);
}

/*
 * Archive mode: the target must be a directory, created if missing
 * (default "."). With a metadata stream, open the second descriptor the
 * metadata thread reads.
 */
static int
dec_archive_target(pc_ctx_t *pctx, const char *filename, char **to_filename,
    const struct dec_hdr *h, int *compfd2)
{
	struct stat sbuf;

	if (h->flags & FLAG_META_STREAM && h->version > 9)
		pctx->meta_stream = 1;

	/*
	 * Archives with metadata streams cannot be decoded in pipe mode.
	 */
	if (pctx->pipe_mode && pctx->meta_stream) {
		log_msg(LOG_ERR, 0,
		    "Cannot extract archive with metadata stream in pipe mode.");
		return (-1);
	}

	/*
	 * If to_filename is not set, we just use the current directory.
	 */
	if (*to_filename == NULL) {
		*to_filename = ".";
		pctx->to_filename = ".";
	}
	pctx->archive_mode = 1;
	if (stat(*to_filename, &sbuf) == -1) {
		if (errno != ENOENT) {
			log_msg(LOG_ERR, 1, "Target path is not a directory.");
			return (-1);
		}
		if (mkdir(*to_filename,
		    S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) == -1) {
			log_msg(LOG_ERR, 1, "Unable to create target directory %s.",
			    *to_filename);
			return (-1);
		}
		if (stat(*to_filename, &sbuf) == -1) {
			log_msg(LOG_ERR, 1, "Unable to correctly create target directory %s.",
			    *to_filename);
			return (-1);
		}
	}
	if (!S_ISDIR(sbuf.st_mode)) {
		log_msg(LOG_ERR, 0, "Target path is not a directory.", *to_filename);
		return (-1);
	}

	/*
	 * Open another fd to the compressed archive. This is used by the metadata
	 * thread.
	 */
	if (pctx->meta_stream) {
		if ((*compfd2 = open(filename, O_RDONLY, 0)) == -1) {
			log_msg(LOG_ERR, 1, "Cannot open: %s", filename);
			return (-1);
		}
	}
	return (0);
}

/*
 * Single-file mode: with no target name, derive one from the input
 * (strip .pz, else append .out). An existing target is never overwritten.
 */
static int
dec_output_name(pc_ctx_t *pctx, const char *filename, char **to_filename)
{
	const char *origf;

	if (pctx->list_mode) {
		log_msg(LOG_ERR, 0, "Nothing to list. The compressed file "
		    "is not an archive.");
		return (-1);
	}
	if (*to_filename == NULL && !pctx->pipe_mode) {
		char *pos;

		/*
		 * Use unused space in archive_members_file buffer to hold generated
		 * filename so that it need not be explicitly freed at the end.
		 */
		*to_filename = pctx->archive_members_file;
		pctx->to_filename = pctx->archive_members_file;
		pos = strrchr(filename, '.');
		if (pos != NULL) {
			/* pos points at the '.': was pos[0]/pos[1], never matched. */
			if ((pos[1] == 'p' || pos[1] == 'P') &&
			    (pos[2] == 'z' || pos[2] == 'Z') && pos[3] == '\0') {
				memcpy(*to_filename, filename, pos - filename);
				(*to_filename)[pos - filename] = '\0';
			} else {
				pos = NULL;
			}
		}

		/*
		 * If no .pz extension is found then use <filename>.out as the
		 * decompressed file name.
		 */
		if (pos == NULL) {
			strcpy(*to_filename, filename);
			strcat(*to_filename, ".out");
			log_msg(LOG_WARN, 0, "Using %s for output file name.", *to_filename);
		}
	}
	if (*to_filename != NULL) {
		char *real;

		origf = *to_filename;
		if ((real = pc_realpath(origf)) != NULL) {
			pc_mfree(real);
			log_msg(LOG_ERR, 0, "File %s exists", origf);
			return (-1);
		}
	}
	return (0);
}

/*
 * Dedupe and checksum settings from the header flags. Global dedupe
 * restore re-reads already written output, so it needs a real output
 * file (archives use a temp file).
 */
static int
dec_flags(pc_ctx_t *pctx, const struct dec_hdr *h, const char *to_filename,
    int *dedupe_flag, algo_props_t *props)
{
	*dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler
	if (h->flags & FLAG_DEDUP) {
		pctx->enable_rabin_scan = 1;
		*dedupe_flag = RABIN_DEDUPE_SEGMENTED;

		if (h->flags & FLAG_DEDUP_FIXED) {
			if (h->version > 7) {
				if (to_filename == NULL && !(h->flags & FLAG_ARCHIVE)) {
					log_msg(LOG_ERR, 0, "Global Deduplication requires "
					    "an output file; cannot decompress to stdout.");
					return (-1);
				}
				pctx->enable_rabin_global = 1;
				*dedupe_flag = RABIN_DEDUPE_FILE_GLOBAL;
			} else {
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "Invalid file deduplication flags.");
				return (-1);
			}
		}
	} else if (h->flags & FLAG_DEDUP_FIXED) {
		pctx->enable_fixed_scan = 1;
		*dedupe_flag = RABIN_DEDUPE_FIXED;
	}

	if (h->flags & FLAG_SINGLE_CHUNK) {
		props->is_single_chunk = 1;
	}

	pctx->cksum = h->flags & CKSUM_MASK;

	/*
	 * Backward compatibility check for SKEIN in archives version 5 or below.
	 * In newer versions BLAKE uses same IDs as SKEIN.
	 */
	if (h->version <= 5) {
		if (pctx->cksum == CKSUM_BLAKE256) pctx->cksum = CKSUM_SKEIN256;
		if (pctx->cksum == CKSUM_BLAKE512) pctx->cksum = CKSUM_SKEIN512;
	}
	if (get_checksum_props(NULL, &(pctx->cksum), &(pctx->cksum_bytes),
	    &(pctx->mac_bytes), 1) == -1) {
		log_msg(LOG_ERR, 0, "Invalid checksum algorithm code: %d. "
		    "File corrupt ?", pctx->cksum);
		return (-1);
	}

	/*
	 * Archives older than version 5 did not support MACs.
	 */
	if (h->version < 5)
		pctx->mac_bytes = 0;
	return (0);
}

/*
 * The encryption fields of a header (version 7 and later: salt, nonce and
 * key length; version 11 and later: the KDF cost) and its stored HMAC.
 * salt1/n1 are the bytes as stored (what the HMAC covers), salt2/nonce
 * as the cipher wants them.
 */
struct dec_crypto {
	int saltlen, noncelen;
	uchar_t *salt1, *salt2;
	uchar_t nonce[MAX_NONCE], n1[MAX_NONCE];
	uint32_t kdf_raw[4];	/* version 11: KDF id, log2 N, r, p */
	scrypt_params_t kdf;
	uchar_t stored_mac[CKSUM_MAX_BYTES];
};

/* Wipe and free what dec_read_crypto() filled in. Safe after any failure. */
static void
dec_crypto_wipe(struct dec_crypto *c)
{
	if (c->salt1 != NULL) {
		memset(c->salt1, 0, c->saltlen);
		pc_mfree(c->salt1);
	}
	if (c->salt2 != NULL) {
		memset(c->salt2, 0, c->saltlen);
		pc_mfree(c->salt2);
	}
	memset(c, 0, sizeof (*c));
}

/*
 * Read the encryption fields and the header HMAC. Untrusted: the salt
 * length is bounded before allocating, the key length and the KDF cost
 * are checked before any key derivation.
 */
static int
dec_read_crypto(pc_ctx_t *pctx, int compfd, const struct dec_hdr *h,
    struct dec_crypto *c)
{
	uchar_t hdr_hash1[CKSUM_MAX_BYTES];

	if (pctx->encrypt_type == CRYPTO_ALG_AES) {
		c->noncelen = 8;
	} else if (pctx->encrypt_type == CRYPTO_ALG_SALSA20) {
		c->noncelen = XSALSA20_CRYPTO_NONCEBYTES;
	} else {
		log_msg(LOG_ERR, 0, "Invalid Encryption algorithm code: %d. File corrupt ?",
			pctx->encrypt_type);
		return (-1);
	}
	if (Read(compfd, &c->saltlen, sizeof (c->saltlen)) < sizeof (c->saltlen)) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	c->saltlen = ntohl(c->saltlen);
	/* Untrusted header field: bound before allocating. */
	if (c->saltlen <= 0 || c->saltlen > MAX_SALTLEN) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Invalid salt length %d. File corrupt ?", c->saltlen);
		return (-1);
	}
	c->salt1 = (uchar_t *)pc_malloc(c->saltlen);
	c->salt2 = (uchar_t *)pc_malloc(c->saltlen);
	if (c->salt1 == NULL || c->salt2 == NULL ||
	    Read(compfd, c->salt1, c->saltlen) < c->saltlen) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	deserialize_checksum(c->salt2, c->salt1, c->saltlen);

	if (Read(compfd, c->n1, c->noncelen) < c->noncelen) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	if (pctx->encrypt_type == CRYPTO_ALG_AES) {
		U64_P(c->nonce) = ntohll(U64_P(c->n1));
	} else if (pctx->encrypt_type == CRYPTO_ALG_SALSA20) {
		deserialize_checksum(c->nonce, c->n1, c->noncelen);
	}

	if (h->version > 6) {
		if (Read(compfd, &(pctx->keylen), sizeof (pctx->keylen)) < sizeof (pctx->keylen)) {
			log_msg(LOG_ERR, 1, "Read: ");
			return (-1);
		}
		pctx->keylen = ntohl(pctx->keylen);
		if (pctx->keylen != 16 && pctx->keylen != 32) {
			log_msg(LOG_ERR, 0, "Invalid key length %d. File corrupt ?",
			    pctx->keylen);
			return (-1);
		}
	}

	/* Version 11: the scrypt cost, checked before any work is done. */
	if (h->version >= VERSION_KDF) {
		if (Read(compfd, c->kdf_raw, sizeof (c->kdf_raw)) < (int64_t)sizeof (c->kdf_raw)) {
			log_msg(LOG_ERR, 1, "Read: ");
			return (-1);
		}
		c->kdf.logn = (int)ntohl(c->kdf_raw[1]);
		c->kdf.r = ntohl(c->kdf_raw[2]);
		c->kdf.p = ntohl(c->kdf_raw[3]);
		if (ntohl(c->kdf_raw[0]) != KDF_SCRYPT || !scrypt_params_ok(&c->kdf)) {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Invalid key derivation parameters. "
			    "File corrupt ?");
			return (-1);
		}
	}

	if (Read(compfd, hdr_hash1, pctx->mac_bytes) < pctx->mac_bytes) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	deserialize_checksum(c->stored_mac, hdr_hash1, pctx->mac_bytes);
	return (0);
}

/*
 * Derive the key from one password candidate and check the header HMAC
 * with it. 1 if it matches (the key is right), 0 if not, -1 on error.
 */
static int
dec_try_key(pc_ctx_t *pctx, const struct dec_hdr *h, struct dec_crypto *c,
    uchar_t *pw, int plen)
{
	uchar_t ntry[MAX_NONCE], mac[CKSUM_MAX_BYTES];
	mac_ctx_t hdr_mac;
	unsigned int hlen;
	unsigned short d1;
	unsigned int d2;
	uint64_t d3;

	/* init_crypto() wipes the nonce it is given: one copy per try. */
	memcpy(ntry, c->nonce, sizeof (ntry));
	cleanup_crypto(&(pctx->crypto_ctx));
	pctx->crypto_ctx.kdf = c->kdf;
	if (init_crypto(&(pctx->crypto_ctx), pw, plen, pctx->encrypt_type,
	    c->salt2, c->saltlen, pctx->keylen, ntry, DECRYPT_FLAG) == -1) {
		log_msg(LOG_ERR, 0, "Failed to initialize crypto");
		return (-1);
	}

	/*
	 * Verify file header HMAC: it also tells whether
	 * this key is the right one.
	 */
	if (hmac_init(&hdr_mac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
		log_msg(LOG_ERR, 0, "Cannot initialize header hmac.");
		return (-1);
	}
	hmac_update(&hdr_mac, (uchar_t *)pctx->algo, ALGO_SZ);
	d1 = htons(h->version);
	hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (h->version));
	d1 = htons(h->flags);
	hmac_update(&hdr_mac, (uchar_t *)&d1, sizeof (h->flags));
	d3 = htonll(h->chunksize);
	hmac_update(&hdr_mac, (uchar_t *)&d3, sizeof (h->chunksize));
	d2 = htonl(h->level);
	hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (h->level));
	if (h->version > 6) {
		d2 = htonl(c->saltlen);
		hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (c->saltlen));
		hmac_update(&hdr_mac, c->salt1, c->saltlen);
		hmac_update(&hdr_mac, c->n1, c->noncelen);
		d2 = htonl(pctx->keylen);
		hmac_update(&hdr_mac, (uchar_t *)&d2, sizeof (pctx->keylen));
	}
	if (h->version >= VERSION_KDF)
		hmac_update(&hdr_mac, (uchar_t *)c->kdf_raw, sizeof (c->kdf_raw));
	hmac_final(&hdr_mac, mac, &hlen);
	hmac_cleanup(&hdr_mac);
	return (memcmp(c->stored_mac, mac, pctx->mac_bytes) == 0);
}

/* The password from pc_set_password(), the -w file or the terminal. */
static int
dec_get_pw(pc_ctx_t *pctx, uchar_t pw[PC_PW_MAX + 1], uchar_t **upw, int *legacy)
{
	int len;

	if (pctx->user_pw != NULL) {
		*upw = pctx->user_pw;
		return (pctx->user_pw_len);
	}
	if (!pctx->pwd_file) {
		len = get_pw_string(pw, "Please enter decryption password", 0);
	} else {
		len = read_pw_file(pctx->pwd_file, pw, legacy);
		if (len == -1)
			log_msg(LOG_ERR, 1, "%s", pctx->pwd_file);
	}
	if (len == -1) {
		log_error_code(PC_ERR_CRYPTO);
		log_msg(LOG_ERR, 0, "Failed to get password.");
	}
	*upw = pw;
	return (len);
}

/*
 * Version 12: read the salt, KDF cost and header tag, refuse a cost out
 * of bounds before any work, derive the key and check the tag over the
 * whole header (which also tells a wrong password). The password is
 * wiped.
 */
static int
dec_crypto_v12(pc_ctx_t *pctx, int compfd, const struct dec_hdr *h)
{
	uchar_t hdr[ALGO_SZ + 2 + 2 + 8 + 4 + PZ_HDR_CRYPTO], tag[AEAD_TAGLEN];
	uchar_t pw[PC_PW_MAX + 1], *upw, *pos = hdr;
	unsigned short d1;
	uint64_t d3;
	int d2, len, rv;

	if (Read(compfd, pctx->pz_hdr, PZ_HDR_CRYPTO) < PZ_HDR_CRYPTO ||
	    Read(compfd, tag, AEAD_TAGLEN) < AEAD_TAGLEN) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Incomplete header. File corrupt ?");
		return (-1);
	}
	memcpy(pos, pctx->algo, ALGO_SZ);
	pos += ALGO_SZ;
	d1 = htons(h->version);
	memcpy(pos, &d1, 2);
	d1 = htons(h->flags);
	memcpy(pos + 2, &d1, 2);
	d3 = htonll(h->chunksize);
	memcpy(pos + 4, &d3, 8);
	d2 = htonl(h->level);
	memcpy(pos + 12, &d2, 4);
	memcpy(pos + 16, pctx->pz_hdr, PZ_HDR_CRYPTO);
	if ((len = dec_get_pw(pctx, pw, &upw, NULL)) == -1)
		return (-1);
	rv = pc_pz_key_open(&pctx->pz_aead, h->flags & MASK_CRYPTO_ALG, upw, len, pctx->pz_hdr);
	memset(upw, 0, len);
	if (upw == pctx->user_pw) {
		pctx->user_pw = NULL;
		pctx->user_pw_len = 0;
	}
	if (rv == -2) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Invalid key derivation parameters. File corrupt ?");
		return (-1);
	}
	if (rv != 0) {
		log_error_code(PC_ERR_CRYPTO);
		log_msg(LOG_ERR, 0, "Failed to initialize crypto");
		return (-1);
	}
	if (pc_pz_open(&pctx->pz_aead, PZ_STREAM_HEADER, 0, hdr, sizeof (hdr), NULL, 0, tag) != 0) {
		pc_pz_key_wipe(&pctx->pz_aead);
		log_error_code(PC_ERR_VERIFY);
		log_msg(LOG_ERR, 0, "Header verification failed! File "
		    "tampered or wrong password.");
		return (-1);
	}
	pctx->pz_v12 = 1;
	return (0);
}

/*
 * Version 12, after the end-of-chunks marker: the end record must be
 * authentic and its counts those read, or chunks were cut off the end
 * (or added). nmeta is checked only when the metadata chunks were
 * counted (they are skipped, and counted, by the data reader).
 */
static int
dec_check_end(pc_ctx_t *pctx, int compfd, uint64_t ndata, uint64_t nmeta, int check_meta)
{
	uchar_t end[PZ_END_LEN];
	uint64_t wd, wm;

	if (Read(compfd, end, sizeof (end)) != sizeof (end) ||
	    pc_pz_end_check(&pctx->pz_aead, end, &wd, &wm) != 0) {
		log_error_code(PC_ERR_VERIFY);
		log_msg(LOG_ERR, 0, "End record missing or not authentic: file truncated "
		    "or tampered.");
		return (-1);
	}
	if (wd != ndata || (check_meta && wm != nmeta)) {
		log_error_code(PC_ERR_VERIFY);
		log_msg(LOG_ERR, 0, "File has %" PRIu64 " data and %" PRIu64 " metadata "
		    "chunks, the end record says %" PRIu64 " and %" PRIu64 ": truncated or "
		    "tampered.", ndata, nmeta, wd, wm);
		return (-1);
	}
	return (0);
}

/*
 * Encrypted file: read the encryption fields, get the password, and set
 * up decryption with the key whose header HMAC matches. The whole
 * password is tried first; for files written before 4.1.1 with a -w file
 * over 16 bytes, also the 15 bytes those used. The password is wiped.
 */
static int
dec_crypto(pc_ctx_t *pctx, int compfd, const struct dec_hdr *h)
{
	struct dec_crypto c;
	uchar_t pw[PC_PW_MAX + 1];
	uchar_t *upw;
	int upw_len, legacy = -1, tries, t, ok = 0;

	if (h->version >= VERSION_AEAD)
		return (dec_crypto_v12(pctx, compfd, h));
	if (pctx->mac_bytes > CKSUM_MAX_BYTES)
		return (-1);	/* not possible: from the checksum table */
	memset(&c, 0, sizeof (c));
	if (dec_read_crypto(pctx, compfd, h, &c) != 0) {
		dec_crypto_wipe(&c);
		return (-1);
	}
	if ((upw_len = dec_get_pw(pctx, pw, &upw, &legacy)) == -1) {
		dec_crypto_wipe(&c);
		return (-1);
	}
	/*
	 * A prompt never took more than 14 bytes: no legacy form.
	 * Version 11 files were written with the whole password.
	 */
	tries = (legacy > 0 && legacy != upw_len &&
	    h->version < VERSION_KDF) ? 2 : 1;
	for (t = 0; t < tries && ok == 0; t++)
		ok = dec_try_key(pctx, h, &c, upw, t == 0 ? upw_len : legacy);
	memset(upw, 0, upw_len);
	if (ok >= 0 && upw == pctx->user_pw) {
		pctx->user_pw = NULL;
		pctx->user_pw_len = 0;
	}
	dec_crypto_wipe(&c);
	if (ok < 0)
		return (-1);
	if (!ok) {
		log_error_code(PC_ERR_VERIFY);
		log_msg(LOG_ERR, 0, "Header verification failed! File "
		    "tampered or wrong password.");
		return (-1);
	}
	return (0);
}

/* Unencrypted file, version 5 and later: verify the header CRC32. */
static int
dec_header_crc(pc_ctx_t *pctx, int compfd, const struct dec_hdr *h)
{
	uint32_t crc1, crc2;
	unsigned short d1;
	unsigned int d2;
	uint64_t ch;

	if (Read(compfd, &crc1, sizeof (crc1)) < sizeof (crc1)) {
		log_msg(LOG_ERR, 1, "Read: ");
		return (-1);
	}
	crc1 = htonl(crc1);
	pctx->mac_bytes = sizeof (uint32_t);

	crc2 = lzma_crc32((uchar_t *)pctx->algo, ALGO_SZ, 0);
	d1 = htons(h->version);
	crc2 = lzma_crc32((uchar_t *)&d1, sizeof (h->version), crc2);
	d1 = htons(h->flags);
	crc2 = lzma_crc32((uchar_t *)&d1, sizeof (h->version), crc2);
	ch = htonll(h->chunksize);
	crc2 = lzma_crc32((uchar_t *)&ch, sizeof (ch), crc2);
	d2 = htonl(h->level);
	crc2 = lzma_crc32((uchar_t *)&d2, sizeof (h->level), crc2);
	if (crc1 != crc2) {
		log_error_code(PC_ERR_VERIFY);
		log_msg(LOG_ERR, 0, "Header verification failed! File tampered "
		    "or wrong password.");
		return (-1);
	}
	return (0);
}

/*
 * Archive mode, after the header: the global-dedupe temp file in the
 * target, the metadata context (its descriptor positioned just past the
 * header), the listing buffer, and the extractor thread.
 */
static int
dec_archive_setup(pc_ctx_t *pctx, int compfd, int compfd2, const char *to_filename,
    int64_t chunksize)
{
	if (pctx->enable_rabin_global) {
		char cwd[MAXPATHLEN];
		int absolute = to_filename[0] == PATHSEP_CHAR;

#ifdef _WIN32
		/* "C:/x" and "C:\\x" are absolute too. */
		absolute = absolute || (to_filename[0] != '\0' && to_filename[1] == ':' &&
		    (to_filename[2] == '/' || to_filename[2] == '\\'));
#endif
		if (!absolute) {
			if (getcwd(cwd, MAXPATHLEN) == NULL) {
				log_msg(LOG_ERR, 1, "Cannot get current dir");
				return (-1);
			}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
			snprintf(pctx->archive_temp_file, sizeof (pctx->archive_temp_file),
			    "%s" PATHSEP_STR "%s" PATHSEP_STR ".data", cwd, to_filename);
#pragma GCC diagnostic pop
		} else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
			snprintf(pctx->archive_temp_file, sizeof (pctx->archive_temp_file),
				 "%s" PATHSEP_STR ".data", to_filename);
#pragma GCC diagnostic pop
		}
		if ((pctx->archive_temp_fd = open(pctx->archive_temp_file,
		    O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) == -1) {
			log_msg(LOG_ERR, 1, "Cannot open temporary data file in "
			    "target directory.");
			return (-1);
		}
		add_fname(pctx->archive_temp_file);
	}

	/*
	 * If we are having a metadata stream, get the current position of the main
	 * fd. The secondary fd must be set to the same position so that metadata
	 * thread can start scanning for chunks after the header and any info chunks.
	 *
	 * NOTE: This is done here to allow setup_extractor() call later to work.
	 */
	if (pctx->meta_stream) {
		off_t cpos = lseek(compfd, 0, SEEK_CUR);
		cpos = lseek(compfd2, cpos, SEEK_SET);
		if (cpos == -1) {
			log_msg(LOG_ERR, 1, "Can't seek in metadata fd: ");
			return (-1);
		}

		/*
		 * Finally create the metadata context.
		 */
		pctx->meta_ctx = meta_ctx_create(pctx, VERSION, compfd2);
		if (pctx->meta_ctx == NULL)
			return (-1);	/* compfd2 is closed by the caller */
	}

	/*
	 * Listing an archive with a metadata stream decompresses metadata
	 * only; data reads are served from a dummy buffer. It must exist
	 * before the extractor thread starts reading.
	 */
	if (pctx->list_mode && pctx->meta_stream) {
		pctx->temp_mmap_buf = (uchar_t *)slab_alloc(NULL, chunksize);
		pctx->temp_mmap_len = chunksize;
		if (pctx->temp_mmap_buf == NULL) {
			log_error_code(PC_ERR_NOMEM);
			log_msg(LOG_ERR, 0, "Out of memory");
			return (-1);
		}
	}

	if (setup_extractor(pctx) == -1) {
		log_msg(LOG_ERR, 0, "Setup of extraction context failed.");
		return (-1);
	}
	if (start_extractor(pctx) == -1) {
		log_msg(LOG_ERR, 0, "Unable to start extraction thread.");
		return (-1);
	}
	return (0);
}

/*
 * How many decompression threads: one per core (one fewer in archive mode,
 * for the extractor), capped by -t; one for a single-chunk file; none when
 * listing an archive with a metadata stream (only metadata is decoded).
 */
static uint32_t
dec_thread_count(pc_ctx_t *pctx, algo_props_t *props)
{
	uint32_t nprocs = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);

	if (pctx->archive_mode) {
		nprocs = nprocs > 1 ? nprocs-1:nprocs;
	}

	if (pctx->nthreads > 0 && pctx->nthreads < nprocs)
		nprocs = pctx->nthreads;
	else
		pctx->nthreads = nprocs;

	set_threadcounts(props, &(pctx->nthreads), nprocs, DECOMPRESS_THREADS);
	if (props->is_single_chunk)
		pctx->nthreads = 1;
	if (pctx->list_mode && pctx->meta_stream)
		pctx->nthreads = 0;

	if (pctx->nthreads * props->nthreads > 1)
		log_msg(LOG_INFO, 0, "Scaling to %d threads", pctx->nthreads * props->nthreads);
	else
		log_msg(LOG_INFO, 0, "Scaling to 1 thread");
	return (pctx->nthreads);
}

/* What every decompression worker is set up with. */
struct dec_job {
	const struct dec_hdr *h;
	int64_t compressed_chunksize;
	int dedupe_flag;
	uint32_t nprocs;
	algo_props_t *props;
	const char *to_filename;
};

/*
 * Set up one worker's slot (codec state, dedupe context and the read
 * handle global dedupe restores from, chunk HMAC) and start its thread.
 * tdat must be zeroed; on failure the caller frees it like any other.
 */
static int
dec_start_worker(pc_ctx_t *pctx, struct cmp_data *tdat, const struct dec_job *j)
{
	const struct dec_hdr *h = j->h;

	tdat->pctx = pctx;
	tdat->compressed_chunk = NULL;
	tdat->uncompressed_chunk = NULL;
	tdat->chunksize = h->chunksize;
	tdat->compress = pctx->_compress_func;
	tdat->decompress = pctx->_decompress_func;
	tdat->cancel = 0;
	tdat->decompressing = 1;
	if (j->props->is_single_chunk) {
		tdat->cksum_mt = 1;
		if (h->version == 6) {
			tdat->cksum_mt = 2; // Indicate old format parallel hash
		}
	} else {
		tdat->cksum_mt = 0;
	}
	tdat->level = h->level;
	tdat->data = NULL;
	tdat->props = j->props;
	Sem_Init(&(tdat->start_sem), 0, 0);
	Sem_Init(&(tdat->cmp_done_sem), 0, 0);
	Sem_Init(&(tdat->write_done_sem), 0, 1);
	Sem_Init(&(tdat->index_sem), 0, 0);

	if (pctx->_init_func) {
		if (pctx->_init_func(&(tdat->data), &(tdat->level), j->props->nthreads,
		    h->chunksize, h->version, DECOMPRESS) != 0)
			return (-1);
	}

	/*
	 * The last parameter is freeram. It is not needed during decompression.
	 */
	if (pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) {
		tdat->rctx = create_dedupe_context(h->chunksize, j->compressed_chunksize,
		    pctx->rab_blk_size, pctx->algo, j->props, pctx->enable_delta_encode,
		    j->dedupe_flag, h->version, DECOMPRESS, 0, NULL, pctx->pipe_mode,
		    j->nprocs, 0, &pctx->dedupe_index);
		if (tdat->rctx == NULL)
			return (-1);
		if (pctx->enable_rabin_global) {
			const char *of = pctx->archive_mode ? pctx->archive_temp_file :
			    j->to_filename;

			if ((tdat->rctx->out_fd = open(of, O_RDONLY, 0)) == -1) {
				log_msg(LOG_ERR, 1, "Unable to get new read handle"
				    " to output file");
				return (-1);
			}
		}
		tdat->rctx->index_sem = &(tdat->index_sem);
	} else {
		tdat->rctx = NULL;
	}

	if (pctx->encrypt_type && !pctx->pz_v12) {
		if (hmac_init(&tdat->chunk_hmac, pctx->cksum, &(pctx->crypto_ctx)) == -1) {
			log_msg(LOG_ERR, 0, "Cannot initialize chunk hmac.");
			return (-1);
		}
	}
	if (pc_thread_create(&(tdat->thr), "pc-decompress", perform_decompress, (void *)tdat) != 0) {
		log_msg(LOG_ERR, 1, "Error in thread creation: ");
		return (-1);
	}
	return (0);
}

/* Read a raw 8-byte length field (the caller converts it); -1 on a short read. */
static int
dec_read_len(pc_ctx_t *pctx, int compfd, uint64_t *v)
{
	int64_t rb = Read(compfd, v, sizeof (*v));

	if (rb != sizeof (*v)) {
		if (rb < 0) {
			log_msg(LOG_ERR, 1, "Read: ");
		} else {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Incomplete chunk %" PRIu64 " header,"
			    "file corrupt", pctx->chunk_num);
		}
		return (-1);
	}
	return (0);
}

/*
 * Read the next data chunk into tdat, skipping metadata chunks (read by
 * the metadata thread from its own descriptor). Chunk buffers are
 * allocated on first use: the format does not say how many chunks there
 * are (pipe mode), and fewer chunks than threads is common.
 * Returns 0 with a chunk ready, 1 at the end of the stream, 2 if the job
 * was cancelled meanwhile, -1 on error.
 */
static int
dec_read_chunk(pc_ctx_t *pctx, int compfd, struct cmp_data *tdat, int64_t chunksize,
    int64_t compressed_chunksize)
{
	int64_t rb;
	int is_meta;

	do {
		/*
		 * First read length of compressed chunk.
		 */
		if (dec_read_len(pctx, compfd, &tdat->len_cmp) != 0)
			return (-1);
		tdat->len_cmp_be = tdat->len_cmp; // Needed for HMAC
		tdat->len_cmp = htonll(tdat->len_cmp);

		/*
		 * Check for ridiculous length.
		 */
		if (tdat->len_cmp > chunksize + 256) {
			log_error_code(PC_ERR_CORRUPT);
			log_msg(LOG_ERR, 0, "Compressed length too big for chunk: %" PRIu64,
			    pctx->chunk_num);
			return (-1);
		}

		/*
		 * Zero compressed len means end of file. A length equal to
		 * METADATA_INDICATOR denotes a metadata chunk only when the
		 * archive has metadata streams; otherwise it is a 1-byte chunk.
		 */
		is_meta = (pctx->meta_stream && tdat->len_cmp == METADATA_INDICATOR);
		if (tdat->len_cmp == 0)
			return (1);
		if (is_meta) {
			pctx->pz_nmeta++;
			/* Its real length follows; it is skipped below. */
			if (dec_read_len(pctx, compfd, &tdat->len_cmp_be) != 0)
				return (-1);
			tdat->len_cmp_be = LE64(tdat->len_cmp_be);
		}

		if (!tdat->compressed_chunk && !is_meta) {
			tdat->compressed_chunk = (uchar_t *)slab_alloc(NULL,
			    compressed_chunksize);
			tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
			    compressed_chunksize);
			tdat->buf_size = compressed_chunksize;
			if (!tdat->compressed_chunk || !tdat->uncompressed_chunk) {
				log_error_code(PC_ERR_NOMEM);
				log_msg(LOG_ERR, 0, "2: Out of memory");
				return (-1);
			}
			tdat->cmp_seg = tdat->uncompressed_chunk;
		}

		if (!is_meta) {
			if (tdat->len_cmp > pctx->largest_chunk)
				pctx->largest_chunk = tdat->len_cmp;
			if (tdat->len_cmp < pctx->smallest_chunk)
				pctx->smallest_chunk = tdat->len_cmp;
			pctx->avg_chunk += tdat->len_cmp;

			/*
			 * Now read compressed chunk including the checksum. This is
			 * a seek if it is just a listing operation. No data is
			 * processed in that case.
			 */
			rb = tdat->len_cmp + pctx->cksum_bytes + pctx->mac_bytes +
			    CHUNK_FLAG_SZ;
			/* The check above leaves 256 bytes; the codec's slack can be less. */
			if ((uint64_t)rb > tdat->buf_size) {
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "Compressed length too big for chunk: %"
				    PRIu64, pctx->chunk_num);
				return (-1);
			}
			tdat->rbytes = Read(compfd, tdat->compressed_chunk, rb);
		} else {
			off_t cpos = lseek(compfd, 0, SEEK_CUR);

			 /* Two values already read */
			rb = tdat->len_cmp_be + METADATA_HDR_SZ - 16;
			tdat->rbytes = lseek(compfd, rb, SEEK_CUR);
			if (tdat->rbytes > 0)
				tdat->rbytes = tdat->rbytes - cpos;
		}
		if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED))
			return (2);
		if (tdat->rbytes < rb) {
			if (tdat->rbytes < 0) {
				log_msg(LOG_ERR, 1, "Read: ");
			} else {
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "Incomplete chunk %" PRIu64 ", file corrupt.",
				    pctx->chunk_num);
			}
			return (-1);
		}
	} while (is_meta);
	return (0);
}

/*
 * Report progress after the calling thread hands out a chunk: in bytes of
 * input so far, and what the writer has written. If the pc_set_progress()
 * callback asks to stop, record PC_ERR_CANCELLED and cancel the job the
 * way a failing thread does (main_cancel); the caller then leaves its
 * feed loop, and the teardown releases and joins every thread. Returns 1
 * if the job is cancelled.
 */
static int
report_progress(pc_ctx_t *pctx, uint64_t in)
{
	uint64_t out = __atomic_load_n(&pctx->prog_out, __ATOMIC_RELAXED);

	if (pctx->progress_fn == NULL || pctx->cancelled)
		return (pctx->cancelled);
	if (pctx->progress_fn(pctx->progress_arg, in, out) == 0)
		return (0);
	pctx->cancelled = 1;
	log_error_code(PC_ERR_CANCELLED);
	log_msg(LOG_ERR, 0, "Cancelled by the progress callback.");
	__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
	return (1);
}

/*
 * Hand chunks to the workers in turn until the end of the stream, an
 * error, or a cancel. Chunk sequencing is ensured: a slot is reused only
 * after the writer has written its previous chunk. *np is the slot that
 * saw the end (its write_done_sem is already taken).
 */
static int
dec_feed(pc_ctx_t *pctx, int compfd, struct cmp_data **dary, const struct dec_job *j,
    uint32_t *np)
{
	struct cmp_data *tdat;
	uint64_t in = 0;
	uint32_t p;
	int rv;

	pctx->chunk_num = 0;
	*np = 0;
	if (j->nprocs == 0)
		return (0);
	for (;;) {
		if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED))
			return (0);
		for (p = 0; p < j->nprocs; p++) {
			*np = p;
			tdat = dary[p];
			Sem_Wait(&tdat->write_done_sem);
			if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED))
				break;
			/*
			 * Version 11 and earlier keyed CTR with a 32-bit chunk
			 * number, so a writer wrapped there and reused nonces.
			 */
			if (pctx->encrypt_type && !pctx->pz_v12 && pctx->chunk_num > UINT32_MAX) {
				log_error_code(PC_ERR_CORRUPT);
				log_msg(LOG_ERR, 0, "More than 2^32 chunks in an encrypted "
				    "version %d file: its nonces repeat.", j->h->version);
				return (-1);
			}
			tdat->id = pctx->chunk_num;
			if (tdat->rctx) tdat->rctx->id = tdat->id;
			rv = dec_read_chunk(pctx, compfd, tdat, j->h->chunksize,
			    j->compressed_chunksize);
			if (rv == -1)
				return (-1);
			if (rv == 1)
				return (0);
			if (rv == 2)
				break;
			/* The length field and the chunk; the worker changes rbytes. */
			in += sizeof (uint64_t) + (uint64_t)tdat->rbytes;
			Sem_Post(&tdat->start_sem);
			++(pctx->chunk_num);
			if (report_progress(pctx, in))
				return (0);
		}
	}
}

/* Free every worker slot (after the threads are joined). */
static void
dec_free_workers(pc_ctx_t *pctx, struct cmp_data **dary, uint32_t nprocs)
{
	uint32_t i;

	for (i = 0; i < nprocs; i++) {
		if (!dary[i]) continue;
		if (dary[i]->uncompressed_chunk)
			slab_release(NULL, dary[i]->uncompressed_chunk);
		if (dary[i]->compressed_chunk)
			slab_release(NULL, dary[i]->compressed_chunk);
		if (pctx->_deinit_func)
			pctx->_deinit_func(&(dary[i]->data));
		if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
			/* The read handle global dedupe restores from (dec_start_worker()). */
			if (dary[i]->rctx && dary[i]->rctx->out_fd != -1)
				close(dary[i]->rctx->out_fd);
			destroy_dedupe_context(dary[i]->rctx);
		}
		if (pctx->encrypt_type && dary[i]->chunk_hmac.mac_ctx)
			hmac_cleanup(&dary[i]->chunk_hmac);
		Sem_Destroy(&(dary[i]->start_sem));
		Sem_Destroy(&(dary[i]->cmp_done_sem));
		Sem_Destroy(&(dary[i]->write_done_sem));
		Sem_Destroy(&(dary[i]->index_sem));

		slab_release(NULL, dary[i]);
	}
	slab_release(NULL, dary);
}

/*
 * Archive mode teardown: end and join the extractor, free the metadata
 * context and the listing buffer, remove the global-dedupe temp file.
 */
static void
dec_archive_done(pc_ctx_t *pctx, int err)
{
	if (pctx->archive_thread_started) {
		/*
		 * On error the extractor may be blocked waiting for data
		 * that will never come: signal end of stream first
		 * (a no-op if the writer already did).
		 */
		if (err) {
			/* The extractor's "End of file" that follows is this, not its own fault. */
			log_msg(LOG_ERR, 0, "Decompression failed (see above): "
			    "ending archive extraction.");
			archiver_close(pctx);
		}
		pthread_join(pctx->archive_thread, NULL);
		pctx->archive_thread_started = 0;
	} else if (pctx->archive_ctx) {
		/* Set up but never handed to the extractor thread, which frees it. */
		archive_read_free((struct archive *)pctx->archive_ctx);
	}
	pctx->archive_ctx = NULL;
	if (pctx->meta_stream) {
		meta_ctx_done(pctx->meta_ctx);
		pctx->meta_ctx = NULL;
		if (pctx->list_mode) {
			slab_release(NULL, pctx->temp_mmap_buf);
		}
	}
	if (pctx->enable_rabin_global) {
		close(pctx->archive_temp_fd);
		unlink(pctx->archive_temp_file);
		rm_fname(pctx->archive_temp_file);
	}
	Sem_Destroy(&(pctx->read_sem));
	Sem_Destroy(&(pctx->write_sem));
}

/*
 * Decompress filename (or the job's input stream) to to_filename (or the
 * output stream), or extract an archive into the to_filename directory.
 * Reads and verifies the header, starts one worker per thread and a
 * writer, feeds them chunks in order, and tears everything down on one
 * path. Returns 0 or 1.
 */
static int
start_decompress_impl(pc_ctx_t *pctx, const char *filename, char *to_filename)
{
	char algorithm[ALGO_SZ];
	struct stat sbuf;
	struct wdata w;
	struct dec_hdr h;
	struct dec_job job;
	int compfd = -1, compfd2 = -1, uncompfd = -1, own_uncompfd = 0, err = 0;
	int thread = 0, dedupe_flag, rv;
	uint32_t nprocs = 1, i, np = 0, started = 0;
	int64_t compressed_chunksize;
	struct cmp_data **dary = NULL;
	pthread_t writer_thr;
	algo_props_t props;

	init_algo_props(&props);
	memset(&h, 0, sizeof (h));
	pctx->cancelled = 0;
	pctx->prog_out = 0;
	if ((rv = dec_open_input(pctx, filename, &compfd, &sbuf)) == 1)
		return (1);
	if (rv != 0 || dec_read_header(pctx, compfd, filename, algorithm, &h) != 0) {
		UNCOMP_BAIL;
	}
	/*
	 * A password says the caller expects an encrypted file: anyone can
	 * substitute a plain one, which nothing would then authenticate.
	 */
	if (!(h.flags & MASK_CRYPTO_ALG) && (pctx->pwd_file != NULL || pctx->user_pw != NULL)) {
		log_error_code(PC_ERR_CRYPTO);
		log_msg(LOG_ERR, 0, "A password was given, but the file is not encrypted.");
		UNCOMP_BAIL;
	}
	if (h.flags & FLAG_ARCHIVE) {
		if (dec_archive_target(pctx, filename, &to_filename, &h, &compfd2) != 0) {
			UNCOMP_BAIL;
		}
	} else if (dec_output_name(pctx, filename, &to_filename) != 0) {
		UNCOMP_BAIL;
	}

	compressed_chunksize = h.chunksize + CHUNK_HDR_SZ + zlib_buf_extra(h.chunksize);
	if (pctx->_props_func) {
		pctx->_props_func(&props, h.level, h.chunksize);
		if (h.chunksize + props.buf_extra > compressed_chunksize) {
			compressed_chunksize += (h.chunksize + props.buf_extra -
			    compressed_chunksize);
		}
	}
	if (dec_flags(pctx, &h, to_filename, &dedupe_flag, &props) != 0) {
		UNCOMP_BAIL;
	}

	/* Version 12 is only ever written encrypted. */
	pctx->pz_v12 = 0;
	if (h.version >= VERSION_AEAD && !(h.flags & MASK_CRYPTO_ALG)) {
		log_error_code(PC_ERR_CORRUPT);
		log_msg(LOG_ERR, 0, "Version %d file without encryption. File corrupt ?",
		    h.version);
		UNCOMP_BAIL;
	}
	if (h.flags & MASK_CRYPTO_ALG) {
		/*
		 * In encrypted files we do not have a normal digest. The HMAC
		 * is computed over header and encrypted data.
		 */
		pctx->cksum_bytes = 0;
		compressed_chunksize += pctx->mac_bytes;
		pctx->encrypt_type = h.flags & MASK_CRYPTO_ALG;
		if (h.version >= VERSION_AEAD)
			pctx->mac_bytes = AEAD_TAGLEN;
		if (h.version < 7)
			pctx->keylen = OLD_KEYLEN;
		if (dec_crypto(pctx, compfd, &h) != 0) {
			UNCOMP_BAIL;
		}
	} else if (h.version >= 5 && dec_header_crc(pctx, compfd, &h) != 0) {
		UNCOMP_BAIL;
	}

	if (h.flags & FLAG_ARCHIVE) {
		uncompfd = -1;
		if (dec_archive_setup(pctx, compfd, compfd2, to_filename, h.chunksize) != 0) {
			UNCOMP_BAIL;
		}
	} else if (to_filename != NULL) {
		/* Write to the named target if given, else stdout (pipe mode). */
		if ((uncompfd = open(to_filename, O_WRONLY|O_CREAT|O_TRUNC,
		    S_IRUSR|S_IWUSR)) == -1) {
			log_msg(LOG_ERR, 1, "Cannot open: %s", to_filename);
			UNCOMP_BAIL;
		}
		own_uncompfd = 1;
	} else if ((uncompfd = PC_OUT_FD(pctx)) == -1) {
		log_msg(LOG_ERR, 1, "fileno ");
		UNCOMP_BAIL;
	}

	/*
	 * WARNING: NO Further file header/info chunk processing beyond this point.
	 *          Doing so will BREAK Separate Metadata stream processing.
	 */
	nprocs = dec_thread_count(pctx, &props);
	slab_cache_add(compressed_chunksize);
	slab_cache_add(h.chunksize);
	slab_cache_add(sizeof (struct cmp_data));

	job.h = &h;
	job.compressed_chunksize = compressed_chunksize;
	job.dedupe_flag = dedupe_flag;
	job.nprocs = nprocs;
	job.props = &props;
	job.to_filename = to_filename;
	dary = (struct cmp_data **)slab_calloc(NULL, nprocs, sizeof (struct cmp_data *));
	/* From here the teardown stops and joins the first `started` workers. */
	thread = 1;
	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			log_error_code(PC_ERR_NOMEM);
			log_msg(LOG_ERR, 0, "1: Out of memory");
			UNCOMP_BAIL;
		}
		/* Error paths free per-member; unset members must be NULL. */
		memset(dary[i], 0, sizeof (struct cmp_data));
		if (dec_start_worker(pctx, dary[i], &job) != 0) {
			UNCOMP_BAIL;
		}
		started++;
	}

	if (pctx->enable_rabin_global) {
		for (i = 0; i < nprocs; i++)
			dary[i]->rctx->index_sem_next = &(dary[(i + 1) % nprocs]->index_sem);
	}
	// When doing global dedupe first thread does not wait to start dedupe recovery.
	if (nprocs > 0)
		Sem_Post(&(dary[0]->index_sem));

	if (pctx->encrypt_type && !pctx->pz_v12) {
		/* Erase encryption key bytes stored as a plain array. No longer reqd. */
		crypto_clean_pkey(&(pctx->crypto_ctx));
	}

	if (!(pctx->list_mode && pctx->meta_stream)) {
		w.dary = dary;
		w.wfd = uncompfd;
		w.nprocs = nprocs;
		w.chunksize = h.chunksize;
		w.pctx = pctx;
		if (pc_thread_create(&writer_thr, "pc-writer", writer_thread, (void *)(&w)) != 0) {
			log_msg(LOG_ERR, 1, "Error in thread creation: ");
			UNCOMP_BAIL;
		}
		thread = 2;
	}

	pctx->pz_nmeta = 0;
	if (dec_feed(pctx, compfd, dary, &job, &np) != 0) {
		UNCOMP_BAIL;
	}
	if (pctx->pz_v12 && nprocs > 0 && !__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED) &&
	    dec_check_end(pctx, compfd, pctx->chunk_num, pctx->pz_nmeta, pctx->meta_stream) != 0) {
		__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
		UNCOMP_BAIL;
	}
	/* Wait for the writer to finish every other slot's last chunk. */
	if (!__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) {
		for (i = 0; i < nprocs; i++) {
			if (i == np) continue;
			Sem_Wait(&dary[i]->write_done_sem);
			if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) break;
		}
	}
uncomp_done:
	/*
	 * Stop and join the workers that started. A later worker's setup
	 * failing used to skip this, leaving the earlier ones running while
	 * their slots were freed below.
	 */
	if (thread) {
		for (i = 0; i < started; i++) {
			struct cmp_data *tdat = dary[i];

			__atomic_store_n(&tdat->cancel, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->start_sem);
			Sem_Post(&tdat->cmp_done_sem);
			/* Wake workers parked on the global dedupe index token. */
			if (pctx->enable_rabin_global)
				Sem_Post(&tdat->index_sem);
			pthread_join(tdat->thr, NULL);
		}
		if (thread == 2)
			pthread_join(writer_thr, NULL);
	}
	/*
	 * Only after the joins: a worker that fails verification sets
	 * t_errored, and reading it earlier let a corrupt file exit 0 with
	 * truncated output (~0.5% of runs).
	 */
	if (pctx->t_errored) err = pctx->t_errored;	/* threads joined: plain read is fine */
	if (pctx->cancelled) err = 1;

	/*
	 * Ownership and mode of target should be same as original.
	 */
	if (filename != NULL && uncompfd != -1) {
		fchmod(uncompfd, sbuf.st_mode);
		if (fchown(uncompfd, sbuf.st_uid, sbuf.st_gid) == -1)
			log_msg(LOG_ERR, 1, "Chown ");
	}
	if (dary != NULL)
		dec_free_workers(pctx, dary, nprocs);
	if (!pctx->pipe_mode) {
		if (filename && compfd != -1) close(compfd);
		/* stdout or the caller's out_fd (decompress to a stream): not ours. */
		if (uncompfd != -1 && own_uncompfd) close(uncompfd);
	}
	/* A cancelled job leaves no partial file it created itself. */
	if (pctx->cancelled && own_uncompfd)
		unlink(to_filename);
	if (pctx->archive_mode)
		dec_archive_done(pctx, err);
	/* The metadata thread reads compfd2 until meta_ctx_done() above. */
	if (compfd2 != -1) close(compfd2);

	if (!pctx->hide_cmp_stats) show_compression_stats(pctx);

	return (err);
}

static void *
perform_compress(void *dat) {
	struct cmp_data *tdat = (struct cmp_data *)dat;
	typeof (tdat->chunksize) _chunksize, len_cmp, dedupe_index_sz, index_size_cmp, plen;
	int type, rv;

	log_bind(&tdat->pctx->log);
	uchar_t *compressed_chunk;
	int64_t rbytes;
	pc_ctx_t *pctx;

	pctx = tdat->pctx;
redo:
	Sem_Wait(&tdat->start_sem);
	if (unlikely(__atomic_load_n(&tdat->cancel, __ATOMIC_RELAXED))) {	/* may race the writer's cancel */
		tdat->len_cmp = 0;
		Sem_Post(&tdat->cmp_done_sem);
		return (0);
	}

	compressed_chunk = tdat->compressed_chunk + CHUNK_FLAG_SZ;
	rbytes = tdat->rbytes;
	dedupe_index_sz = 0;
	type = COMPRESSED;

	/* Perform Dedup if enabled. */
	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
		dedupe_context_t *rctx;
		uint64_t rb = tdat->rbytes;

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
		dedupe_index_sz = dedupe_compress(tdat->rctx, tdat->cmp_seg, &rb, 0,
						  NULL, tdat->cksum_mt);
		tdat->rbytes = rb;
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
		} else if (pctx->preprocess_mode) {
			rv = preproc_compress(pctx, tdat->compress,
			    tdat->uncompressed_chunk + dedupe_index_sz, _chunksize,
			    compressed_chunk + index_size_cmp, &_chunksize, tdat->level, 0,
			    tdat->btype, tdat->data, tdat->props, tdat->interesting);
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
		if (pctx->preprocess_mode) {
			rv = preproc_compress(pctx, tdat->compress, tdat->uncompressed_chunk,
			    tdat->rbytes, compressed_chunk, &_chunksize, tdat->level, 0,
			    tdat->btype, tdat->data, tdat->props, tdat->interesting);
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
	 *
	 * HOWEVER, increased chunk size is allowed in preprocessing mode since
	 * there are unavoidable cases where an E8E9 filter is applied and then
	 * later on compression does not happen. So we have to retain information
	 * that E8E9 hapened, to recover the data correctly. In this corner case
	 * the chunk size is increased by 1 byte for the preproc header.
	 */
	tdat->len_cmp = _chunksize;
	if ((_chunksize >= tdat->rbytes && !pctx->preprocess_mode) || rv < 0) {
		if (!(pctx->enable_rabin_scan || pctx->enable_fixed_scan) || !tdat->rctx->valid)
			memcpy(compressed_chunk, tdat->uncompressed_chunk, tdat->rbytes);
		type = UNCOMPRESSED;
		tdat->len_cmp = tdat->rbytes;
		if (rv < 0) rv = COMPRESS_NONE;
	}

	/* The payload, encrypted below (version 12) once the header is known. */
	plen = tdat->len_cmp;

	if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan) && tdat->rctx->valid) {
		type |= CHUNK_FLAG_DEDUP;
	}
	if (pctx->preprocess_mode) {
		type |= CHUNK_FLAG_PREPROC;
	}

	/*
	 * Insert compressed chunk length and checksum into chunk header.
	 */
	len_cmp = tdat->len_cmp;
	U64_P(tdat->cmp_seg) = htonll(tdat->len_cmp);
	if (!pctx->encrypt_type)
		serialize_checksum(tdat->checksum, tdat->cmp_seg + sizeof (tdat->len_cmp),
		    pctx->cksum_bytes);
	tdat->len_cmp += CHUNK_FLAG_SZ;
	tdat->len_cmp += sizeof (len_cmp);
	tdat->len_cmp += (pctx->cksum_bytes + pctx->mac_bytes);
	rbytes = tdat->len_cmp - len_cmp; // HDR size for HMAC

	/*
	 * In adaptive mode return value from compression function function indicates
	 * which algorithm was used on the chunk. We have to store that.
	 */
	if (pctx->adapt_mode)
		type |= (rv << 4);

	/*
	 * If chunk is less than max chunksize, store this length as well.
	 */
	if (tdat->rbytes < tdat->chunksize) {
		type |= CHSIZE_MASK;
		I64_P(tdat->cmp_seg + tdat->len_cmp) = htonll(tdat->rbytes);
		tdat->len_cmp += ORIGINAL_CHUNKSZ;
		len_cmp += ORIGINAL_CHUNKSZ;
		U64_P(tdat->cmp_seg) = htonll(len_cmp);
	}
	/*
	 * Set the chunk header flags.
	 */
	*(tdat->compressed_chunk) = type;

	/*
	 * Version 12: seal the payload as chunk tdat->id of the data stream,
	 * with its length, flags and original size as associated data.
	 */
	if (pctx->encrypt_type) {
		if (pz_seal_chunk(pctx, tdat->cmp_seg, plen, type, tdat->id) != 0) {
			log_error_code(PC_ERR_CRYPTO);
			log_msg(LOG_ERR, 0, "Chunk %" PRIu64 ", encryption failed", tdat->id);
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			tdat->len_cmp = 0;
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->cmp_done_sem);
			return (0);
		}
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

	Sem_Post(&tdat->cmp_done_sem);
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
	log_bind(&pctx->log);
repeat:
	for (p = 0; p < w->nprocs; p++) {
		tdat = w->dary[p];
		Sem_Wait(&tdat->cmp_done_sem);
		/*
		 * cancel first: at shutdown the main thread posts cmp_done_sem
		 * itself, and len_cmp may then still be in a worker's hands.
		 * Otherwise the post came from the worker and len_cmp is ready.
		 */
		if (__atomic_load_n(&tdat->cancel, __ATOMIC_RELAXED) || tdat->len_cmp == 0) {
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
			pthread_mutex_lock(&pctx->write_mutex);
			wbytes = Write(w->wfd, tdat->cmp_seg, tdat->len_cmp);
			pthread_mutex_unlock(&pctx->write_mutex);
		}
		if (pctx->archive_temp_fd != -1 && wbytes == tdat->len_cmp) {
			wbytes = Write(pctx->archive_temp_fd, tdat->cmp_seg, tdat->len_cmp);
		}
		if (unlikely(wbytes != tdat->len_cmp)) {
			log_msg(LOG_ERR, 1, "Chunk Write (expected: %" PRIu64
			    ", written: %" PRId64 ") : ", tdat->len_cmp, wbytes);
			__atomic_store_n(&pctx->t_errored, 1, __ATOMIC_RELAXED);
do_cancel:
			__atomic_store_n(&pctx->main_cancel, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&tdat->cancel, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->start_sem);
			if (tdat->rctx && pctx->enable_rabin_global)
				Sem_Post(tdat->rctx->index_sem_next);
			/*
			 * The writer is exiting: chunks queued behind this one will
			 * never be written, so release every slot or the main
			 * thread blocks forever waiting for a later one.
			 */
			for (p = 0; p < w->nprocs; p++)
				Sem_Post(&w->dary[p]->write_done_sem);
			return (0);
		}
		__atomic_add_fetch(&pctx->prog_out, (uint64_t)wbytes, __ATOMIC_RELAXED);
		if (tdat->decompressing && tdat->rctx && pctx->enable_rabin_global) {
			Sem_Post(tdat->rctx->index_sem_next);
		}
		Sem_Post(&tdat->write_done_sem);
	}
	goto repeat;
}

/*
 * Write the file header: algorithm, version, flags, chunk size and level,
 * then for an encrypted file (version 12) the salt and KDF cost and an
 * AEAD tag over all of it (stream PZ_STREAM_HEADER, nothing encrypted),
 * else (version 10) a CRC32. Built in a buffer of its own: the chunk
 * buffer is sized from the chunk size, which for a tiny input is smaller
 * than an encrypted header (4.4 wrote past it for 1- and 2-byte files).
 */
static int
write_file_header(pc_ctx_t *pctx, int compfd, unsigned short flags, uint64_t chunksize,
    int level)
{
	uchar_t hdr[ALGO_SZ + 2 + 2 + 8 + 4 + PZ_HDR_CRYPTO + AEAD_TAGLEN], *pos = hdr;
	unsigned short v = htons(pctx->encrypt_type ? VERSION_AEAD : VERSION);
	unsigned short f = htons(flags);
	uint64_t cs = htonll(chunksize);
	int lv = htonl(level);
	uint32_t crc;

	memset(hdr, 0, ALGO_SZ);
	strncpy((char *)hdr, pctx->algo, ALGO_SZ);
	pos += ALGO_SZ;
	memcpy(pos, &v, sizeof (v));
	pos += sizeof (v);
	memcpy(pos, &f, sizeof (f));
	pos += sizeof (f);
	memcpy(pos, &cs, sizeof (cs));
	pos += sizeof (cs);
	memcpy(pos, &lv, sizeof (lv));
	pos += sizeof (lv);
	if (pctx->encrypt_type) {
		memcpy(pos, pctx->pz_hdr, PZ_HDR_CRYPTO);
		pos += PZ_HDR_CRYPTO;
		if (pc_pz_seal(&pctx->pz_aead, PZ_STREAM_HEADER, 0, hdr, pos - hdr, NULL, 0,
		    pos) != 0) {
			log_error_code(PC_ERR_CRYPTO);
			log_msg(LOG_ERR, 0, "Cannot authenticate the header.");
			return (-1);
		}
		pos += AEAD_TAGLEN;
	} else {
		/* Archive version 5 and later: a header CRC32. */
		crc = htonl(lzma_crc32(hdr, pos - hdr, 0));
		memcpy(pos, &crc, sizeof (crc));
		pos += sizeof (crc);
	}
	if (Write(compfd, hdr, pos - hdr) != pos - hdr) {
		log_msg(LOG_ERR, 1, "Write ");
		return (-1);
	}
	return (0);
}

/*
 * File compression routine. Can use as many threads as there are
 * logical cores unless user specified something different. There is
 * not much to gain from nthreads > n logical cores however.
 */
#define COMP_BAIL err = 1; goto comp_done

static int
start_compress_impl(pc_ctx_t *pctx, const char *filename, uint64_t chunksize, int level)
{
	struct wdata w;
	char tmpfile1[MAXPATHLEN], tmpdir[MAXPATHLEN];
	char to_filename[MAXPATHLEN];
	uint64_t compressed_chunksize, file_offset;
	int64_t rbytes, rabin_count;
	unsigned short flags;
	struct stat sbuf;
	int compfd = -1, uncompfd = -1, err;
	int thread, bail, single_chunk;
	uint32_t i, nprocs, np, p, dedupe_flag, started = 0;
	struct cmp_data **dary = NULL, *tdat;
	pthread_t writer_thr;
	uchar_t *cread_buf;
	dedupe_context_t *rctx;
	algo_props_t props;
	my_sysinfo msys_info;

	init_algo_props(&props);
	props.cksum = pctx->cksum;
	props.buf_extra = 0;
	cread_buf = NULL;
	pctx->btype = TYPE_UNKNOWN;
	flags = 0;
	sbuf.st_size = 0;
	err = 0;
	thread = 0;
	dedupe_flag = RABIN_DEDUPE_SEGMENTED; // Silence the compiler
	compressed_chunksize = 0;
	pctx->cancelled = 0;
	pctx->prog_out = 0;

	pctx->pz_nmeta = 0;
	pctx->pz_v12 = 0;
	if (pctx->encrypt_type) {
		uchar_t pw[PC_PW_MAX + 1];
		int pw_len = -1, rv;

		compressed_chunksize += AEAD_TAGLEN;
		if (!pctx->pwd_file && !pctx->user_pw) {
			pw_len = get_pw_string(pw,
				"Please enter encryption password", 1);
			if (pw_len == -1) {
				log_error_code(PC_ERR_CRYPTO);
				log_msg(LOG_ERR, 0, "Failed to get password.");
				return (1);
			}
		} else if (!pctx->user_pw) {
			if ((pw_len = read_pw_file(pctx->pwd_file, pw, NULL)) == -1) {
				log_error_code(PC_ERR_CRYPTO);
				log_msg(LOG_ERR, 1, "Failed to get password.");
				return (1);
			}
		}
		/* Format version 12: one Argon2id key per file, for the AEAD. */
		if (pctx->user_pw) {
			rv = pc_pz_key_new(&pctx->pz_aead, pctx->encrypt_type, pctx->user_pw,
			    pctx->user_pw_len, pctx->pz_hdr);
			memset(pctx->user_pw, 0, pctx->user_pw_len);
			pctx->user_pw = NULL;
			pctx->user_pw_len = 0;
		} else {
			rv = pc_pz_key_new(&pctx->pz_aead, pctx->encrypt_type, pw, pw_len,
			    pctx->pz_hdr);
			memset(pw, 0, sizeof (pw));
		}
		if (rv != 0) {
			log_error_code(PC_ERR_CRYPTO);
			log_msg(LOG_ERR, 0, "Failed to initialize crypto.");
			return (1);
		}
		pctx->pz_v12 = 1;
		pctx->cksum_bytes = 0;
		pctx->mac_bytes = AEAD_TAGLEN;
	}

	single_chunk = 0;
	rctx = NULL;

	/*
	 * Get number of lCPUs. When archiving with advanced filters, we use one less
	 * lCPU to reduce threads due to increased memory requirements.
	 */
	nprocs = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
	if (pctx->archive_mode && (pctx->enable_packjpg || pctx->enable_wavpack)) {
		nprocs = nprocs > 1 ? nprocs-1:nprocs;
	}

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
				log_msg(LOG_ERR, 0, "File %s is empty; nothing to compress.", filename);
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
			 * Disable deduplication if file is too small.
			 */
			if (chunksize < RAB_MIN_CHUNK_SIZE) {
				pctx->enable_rabin_scan = 0;
				pctx->enable_rabin_global = 0;
				pctx->enable_fixed_scan = 0;
				pctx->enable_delta_encode = 0;
			}

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
				pctx->nthreads = (int)(sbuf.st_size / chunksize);
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
			char *d;

			strcpy(tmpfile1, filename);
			/* dirname() may return a pointer into its argument: memmove. */
			d = dirname(tmpfile1);
			memmove(tmpfile1, d, strlen(d) + 1);
		} else {
			char *tmp1;
			if (!(pctx->archive_mode)) {
				log_msg(LOG_ERR, 0, "Inconsistent NULL Filename when Not archiving.");
				COMP_BAIL;
			}
			tmp1 = get_temp_dir();
			strcpy(tmpfile1, tmp1);
			pc_mfree(tmp1);
		}

		tmp = getenv("PCOMPRESS_CACHE_DIR");
		if (tmp == NULL || !chk_dir(tmp)) {
			strcpy(tmpdir, tmpfile1);
		} else {
			strcpy(tmpdir, tmp);
		}

		if (pctx->pipe_out) {
			compfd = PC_OUT_FD(pctx);
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
				if (!endswith(pctx->to_filename, COMP_EXTN))
					snprintf(to_filename, sizeof (to_filename),
					    "%s" COMP_EXTN, pctx->to_filename);
				else
					snprintf(to_filename, sizeof (to_filename),
					    "%s", pctx->to_filename);
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
		compfd = PC_OUT_FD(pctx);
		if (compfd == -1) {
			log_msg(LOG_ERR, 1, "fileno ");
			COMP_BAIL;
		}
		uncompfd = PC_IN_FD(pctx);
		if (uncompfd == -1) {
			log_msg(LOG_ERR, 1, "fileno ");
			COMP_BAIL;
		}

		/*
		 * Get a workable temporary dir. Required if global dedupe is enabled.
		 */
		tmp = get_temp_dir();
		strcpy(tmpdir, tmp);
		pc_mfree(tmp);
	}

	if (pctx->enable_rabin_global) {
		my_sysinfo msys_info;

		get_sys_limits(&msys_info);
		global_dedupe_bufadjust(pctx->rab_blk_size, &chunksize, 0, pctx->algo,
		    pctx->cksum, CKSUM_BLAKE256, sbuf.st_size, msys_info.freeram,
		    pctx->nthreads, pctx->pipe_mode);
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
	 * compressed_chunksize might also already be set to accomodate a HMAC
	 * when encrypting, so we have to add to than here. Otherwise it is set
	 * to 0.
	 */
	compressed_chunksize += chunksize + CHUNK_HDR_SZ + zlib_buf_extra(chunksize);
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
	cread_buf = (uchar_t *)slab_alloc(NULL, compressed_chunksize);
	if (!cread_buf) {
		log_error_code(PC_ERR_NOMEM);
		log_msg(LOG_ERR, 0, "3: Out of memory");
		COMP_BAIL;
	}

	for (i = 0; i < nprocs; i++) {
		dary[i] = (struct cmp_data *)slab_alloc(NULL, sizeof (struct cmp_data));
		if (!dary[i]) {
			log_error_code(PC_ERR_NOMEM);
			log_msg(LOG_ERR, 0, "4: Out of memory");
			COMP_BAIL;
		}
		/* Error paths free per-member; unset members must be NULL. */
		memset(dary[i], 0, sizeof (struct cmp_data));
		tdat = dary[i];
		tdat->pctx = pctx;
		tdat->cmp_seg = NULL;
		tdat->chunksize = chunksize;
		tdat->compress = pctx->_compress_func;
		tdat->decompress = pctx->_decompress_func;
		tdat->uncompressed_chunk = (uchar_t *)1;
		if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
			if (single_chunk)
				tdat->cmp_seg = (uchar_t *)1;
			else
				tdat->cmp_seg = (uchar_t *)slab_alloc(NULL, compressed_chunksize);
			tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
				compressed_chunksize);
		} else {
			if (single_chunk)
				tdat->uncompressed_chunk = (uchar_t *)1;
			else
				tdat->uncompressed_chunk = (uchar_t *)slab_alloc(NULL,
					compressed_chunksize);
			tdat->cmp_seg = (uchar_t *)slab_alloc(NULL, compressed_chunksize);
		}
		tdat->compressed_chunk = tdat->cmp_seg + COMPRESSED_CHUNKSZ +
		    pctx->cksum_bytes + pctx->mac_bytes;
		if (!tdat->cmp_seg || !tdat->uncompressed_chunk) {
			log_error_code(PC_ERR_NOMEM);
			log_msg(LOG_ERR, 0, "5: Out of memory");
			COMP_BAIL;
		}
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
		Sem_Init(&(tdat->start_sem), 0, 0);
		Sem_Init(&(tdat->cmp_done_sem), 0, 0);
		Sem_Init(&(tdat->write_done_sem), 0, 1);
		Sem_Init(&(tdat->index_sem), 0, 0);

		if (pctx->_init_func) {
			if (pctx->_init_func(&(tdat->data), &(tdat->level), props.nthreads,
			    chunksize, VERSION, COMPRESS) != 0) {
				COMP_BAIL;
			}
		}

		/* Running workers must be joined even if a later setup fails. */
		thread = 1;
		if (pc_thread_create(&(tdat->thr), "pc-compress", perform_compress,
		    (void *)tdat) != 0) {
			log_msg(LOG_ERR, 1, "Error in thread creation: ");
			COMP_BAIL;
		}
		started++;
	}

	/*
	 * Now create the metadata handler context. This is relevant in archive mode where
	 * the underlying libarchive metadata is compressed into a separate stream of
	 * metadata chunks.
	 */
	if (pctx->meta_stream) {
		pctx->meta_ctx = meta_ctx_create(pctx, VERSION, compfd);
		if (pctx->meta_ctx == NULL) {
			COMP_BAIL;
		}
	}
	thread = 1;

	/*
	 * initialize Dedupe Context here after all other allocations so that index size can be
	 * correctly computed based on free memory. The freeram got here is adjusted amount.
	 * When archiving, filter scratch buffer is taken into account.
	 */
	get_sys_limits(&msys_info);

	if (pctx->enable_packjpg || pctx->enable_wavpack) {
		if (FILTER_SCRATCH_SIZE_MAX >= msys_info.freeram ||
		    msys_info.freeram - FILTER_SCRATCH_SIZE_MAX < FILTER_SCRATCH_SIZE_MAX) {
			log_msg(LOG_WARN, 0, "Not enough memory. Disabling advanced filters.");
			disable_all_filters(pctx);
		} else {
			msys_info.freeram -= FILTER_SCRATCH_SIZE_MAX;
		}
	}

	if (pctx->enable_rabin_scan || pctx->enable_fixed_scan || pctx->enable_rabin_global) {
		for (i = 0; i < nprocs; i++) {
			tdat = dary[i];
			tdat->rctx = create_dedupe_context(chunksize, compressed_chunksize,
			    pctx->rab_blk_size, pctx->algo, &props, pctx->enable_delta_encode,
			    dedupe_flag, VERSION, COMPRESS, sbuf.st_size, tmpdir,
			    pctx->pipe_mode, nprocs, msys_info.freeram, &pctx->dedupe_index);
			if (tdat->rctx == NULL) {
				COMP_BAIL;
			}

			tdat->rctx->show_chunks = pctx->show_chunks;
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
		Sem_Post(&(dary[0]->index_sem));
	}

	w.dary = dary;
	w.wfd = compfd;
	w.nprocs = nprocs;
	w.pctx = pctx;
	if (pc_thread_create(&writer_thr, "pc-writer", writer_thread, (void *)(&w)) != 0) {
		log_msg(LOG_ERR, 1, "Error in thread creation: ");
		COMP_BAIL;
	}
	thread = 2;

	/*
	 * Start the archiver thread if needed.
	 */
	if (pctx->archive_mode) {
		if (start_archiver(pctx) != 0) {
			COMP_BAIL;
		}
		flags |= FLAG_ARCHIVE;
		if (pctx->meta_stream)
			flags |= FLAG_META_STREAM;
	}

	flags |= pctx->cksum;
	if (write_file_header(pctx, compfd, flags, chunksize, level) != 0) {
		COMP_BAIL;
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
	pctx->interesting = 0;
	if (pctx->enable_rabin_split) {
		rctx = create_dedupe_context(chunksize, 0, pctx->rab_blk_size, pctx->algo, &props,
		    pctx->enable_delta_encode, pctx->enable_fixed_scan, VERSION, COMPRESS, 0, NULL,
		    pctx->pipe_mode, nprocs, msys_info.freeram, &pctx->dedupe_index);
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

		if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) break;
		for (p = 0; p < nprocs; p++) {
			np = p;
			tdat = dary[p];
			if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) break;
			/* Wait for previous chunk compression to complete. */
			Sem_Wait(&tdat->write_done_sem);
			if (__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) break;

			if (rbytes == 0) { /* EOF */
				bail = 1;
				break;
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
			tdat->interesting = pctx->interesting;
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
			Sem_Post(&tdat->start_sem);
			++(pctx->chunk_num);
			if (report_progress(pctx, file_offset))
				break;

			if (single_chunk) {
				rbytes = 0;
				continue;
			}

			/*
			 * Read the next buffer we want to process while previous
			 * buffer is in progress.
			 */
			pctx->interesting = 0;
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

	if (!__atomic_load_n(&pctx->main_cancel, __ATOMIC_RELAXED)) {
		/* Wait for all remaining chunks to finish. */
		for (p = 0; p < nprocs; p++) {
			if (p == np) continue;
			tdat = dary[p];
			Sem_Wait(&tdat->write_done_sem);
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
	}
	if (pctx->meta_stream) {
		meta_ctx_done(pctx->meta_ctx);
		pctx->meta_ctx = NULL;
	}
	/* Without it an archiver still in creat_write_callback() never exits. */
	if (pctx->archive_mode)
		archiver_close(pctx);

	if (thread) {
		for (i = 0; i < started; i++) {
			tdat = dary[i];
			__atomic_store_n(&tdat->cancel, 1, __ATOMIC_RELAXED);
			Sem_Post(&tdat->start_sem);
			Sem_Post(&tdat->cmp_done_sem);
			/* Wake workers parked on the global dedupe index token. */
			if (pctx->enable_rabin_global)
				Sem_Post(&tdat->index_sem);
			pthread_join(tdat->thr, NULL);
		}
		if (thread == 2)
			pthread_join(writer_thr, NULL);
	}
	/* After the joins, as in start_decompress(): workers set t_errored. */
	if (pctx->t_errored) err = pctx->t_errored;	/* threads joined: plain read is fine */
	if (pctx->cancelled) err = 1;

	if (err) {
		if (compfd != -1 && !pctx->pipe_mode && !pctx->pipe_out) {
			/* The file it opened: the temporary one, or the named target. */
			const char *made = pctx->to_filename == NULL ? tmpfile1 : to_filename;

			unlink(made);
			rm_fname((char *)made);
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
		/* Version 12: the authenticated chunk counts, so truncation shows. */
		if (!err && pctx->pz_v12) {
			uchar_t end[PZ_END_LEN];

			if (pc_pz_end_make(&pctx->pz_aead, pctx->chunk_num, pctx->pz_nmeta,
			    end) != 0 ||
			    Write(compfd, end, sizeof (end)) != sizeof (end)) {
				log_msg(LOG_ERR, 1, "Write ");
				err = 1;
			}
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
			compfd = -1;	/* closed again below otherwise: may be another thread's fd by then */

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
				slab_release(NULL, dary[i]->uncompressed_chunk);
			if (dary[i]->cmp_seg != (uchar_t *)1)
				slab_release(NULL, dary[i]->cmp_seg);
			if ((pctx->enable_rabin_scan || pctx->enable_fixed_scan)) {
				destroy_dedupe_context(dary[i]->rctx);
			}
			if (pctx->_deinit_func)
				pctx->_deinit_func(&(dary[i]->data));
			Sem_Destroy(&(dary[i]->start_sem));
			Sem_Destroy(&(dary[i]->cmp_done_sem));
			Sem_Destroy(&(dary[i]->write_done_sem));
			Sem_Destroy(&(dary[i]->index_sem));

			slab_release(NULL, dary[i]);
		}
		slab_release(NULL, dary);
	}
	if (pctx->enable_rabin_split) destroy_dedupe_context(rctx);
	destroy_dedupe_index(&pctx->dedupe_index);	/* after all worker contexts */
	if (cread_buf != (uchar_t *)1)
		slab_release(NULL, cread_buf);
	if (!pctx->pipe_mode) {
		if (compfd != -1) close(compfd);
	}

	if (pctx->archive_mode) {
		struct fn_list *fn, *fn1;

		if (pctx->archive_thread_started) {
			pthread_join(pctx->archive_thread, NULL);
			pctx->archive_thread_started = 0;
		} else if (pctx->archive_ctx) {
			/* Set up but never handed to the archiver thread, which frees it. */
			archive_write_free((struct archive *)pctx->archive_ctx);
		}
		pctx->archive_ctx = NULL;
		fn = pctx->fn;
		while (fn) {
			fn1 = fn;
			fn = fn->next;
			pc_mfree(fn1->filename);
			slab_release(NULL, fn1);
		}
		Sem_Destroy(&(pctx->read_sem));
		Sem_Destroy(&(pctx->write_sem));
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
	char algorithm[9];

	/*
	 * Names are matched by prefix because the file header stores them in
	 * a fixed 8-byte field. A user-supplied name (bail != 0) must match
	 * exactly, else "nonexistent" would silently select "none".
	 */
	if (bail) {
		static const char *names[] = { "zlib", "lzmaMt", "lzma", "bzip2",
		    "ppmd", "lzfx", "lz4", "zstd", "none", "adapt2", "adapt",
		    "libbsc", NULL };
		int i;

		for (i = 0; names[i] != NULL; i++)
			if (strcmp(algo, names[i]) == 0)
				break;
		if (names[i] == NULL)
			return (1);
	}

	/* Copy given string into known length buffer to avoid memcmp() overruns. */
	strncpy(algorithm, algo, 8);
	algorithm[8] = '\0'; /* Ensure null termination */
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


#ifdef ENABLE_PC_ZSTD
	} else if (memcmp(algorithm, "zstd", 4) == 0) {
		pctx->_compress_func = zstd_compress;
		pctx->_decompress_func = zstd_decompress;
		pctx->_init_func = zstd_init;
		pctx->_deinit_func = zstd_deinit;
		pctx->_stats_func = zstd_stats;
		pctx->_props_func = zstd_props;
		rv = 0;
#endif

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
		pctx->adapt_mode = 2;
		pctx->enable_analyzer = 1;
		rv = 0;

	} else if (memcmp(algorithm, "adapt", 5) == 0) {
		pctx->_compress_func = adapt_compress;
		pctx->_decompress_func = adapt_decompress;
		pctx->_init_func = adapt_init;
		pctx->_deinit_func = adapt_deinit;
		pctx->_stats_func = adapt_stats;
		pctx->_props_func = adapt_props;
		pctx->adapt_mode = 1;
		pctx->enable_analyzer = 1;
		rv = 0;
#ifdef ENABLE_PC_LIBBSC
	} else if (memcmp(algorithm, "libbsc", 6) == 0) {
		pctx->_compress_func = libbsc_compress;
		pctx->_decompress_func = libbsc_decompress;
		pctx->_init_func = libbsc_init;
		pctx->_deinit_func = libbsc_deinit;
		pctx->_stats_func = libbsc_stats;
		pctx->_props_func = libbsc_props;
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
	pc_ctx_t *ctx = (pc_ctx_t *)pc_malloc(sizeof (pc_ctx_t));

	if (ctx == NULL)
		return (NULL);
	if (ossl_check_version() != 0) {
		log_msg(LOG_ERR, 0, "FATAL: Incompatible OpenSSL version detected.\n"
		    "       Compiled with: %s\n"
		    "       Runtime:       %s",
		    ossl_compiled_version(), ossl_runtime_version());
		pc_mfree(ctx);
		return (NULL);
	}
	slab_init();
	init_pcompress();
	init_archive_mod();

	memset(ctx, 0, sizeof (pc_ctx_t));
	ctx->exec_name = (char *)pc_malloc(NAME_MAX);
	ctx->hide_cmp_stats = 1;
	ctx->enable_rabin_split = 1;
	ctx->rab_blk_size = -1;
	ctx->archive_temp_fd = -1;
	ctx->pagesize = sysconf(_SC_PAGE_SIZE);
	ctx->btype = TYPE_UNKNOWN;
	ctx->delta2_nstrides = NSTRIDES_STANDARD;
	ctx->log.level = -1;	/* process default until pc_set_log() */
	ctx->in_fd = ctx->out_fd = -1;
	pthread_mutex_init(&ctx->write_mutex, NULL);

	return (ctx);
}

void DLL_EXPORT
destroy_pc_context(pc_ctx_t *pctx)
{
	cleanup_crypto(&(pctx->crypto_ctx));
	/* Always heap-owned (pc_realpath() or pc_strdup()), NULL for stdin. */
	pc_mfree((void *)(pctx->filename));
	if (pctx->pwd_file)
		pc_mfree(pctx->pwd_file);
	pc_mfree((void *)(pctx->exec_name));
	free_filters(pctx);
	if (pctx->pw_copy != NULL) {
		memset(pctx->pw_copy, 0, pctx->pw_copy_len);
		pc_mfree(pctx->pw_copy);
	}
	memset(pctx->chunk_key, 0, sizeof (pctx->chunk_key));
	pc_pz_key_wipe(&pctx->pz_aead);
	chunk_codecs_free(pctx);
	pc_mfree(pctx);
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

/*
 * Settings that follow from the options: defaults by level, filter and
 * preprocessor choice, and the checks that need the whole set. Used by
 * init_pc_context() after parsing and by pc_compress_fd()/pc_decompress_fd().
 */
static int
finish_settings(pc_ctx_t *pctx, struct filter_flags *ff_in)
{
	struct filter_flags ff = *ff_in;

	if (pctx->cksum == 0)
		get_checksum_props(DEFAULT_CKSUM, &(pctx->cksum), &(pctx->cksum_bytes),
				   &(pctx->mac_bytes), 0);

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

		/*
		 * Global deduplication produces a stream that cannot be decoded
		 * from a pipe, so reject an explicit request for it when the
		 * compressed output goes to stdout (pipe_out) or the input is a
		 * pipe (pipe_mode), rather than emitting an undecodable archive.
		 */
		if ((pctx->pipe_mode || pctx->pipe_out) && pctx->enable_rabin_global) {
			log_msg(LOG_ERR, 0,
			    "Global Deduplication is not supported with pipe mode.");
			return (1);
		}

		if (pctx->filename && stat(pctx->filename, &sbuf) == -1) {
			log_msg(LOG_ERR, 1, "Cannot stat: %s", pctx->filename);
			return (1);
		}

		if (pctx->chunksize == 0) {
			if (pctx->level < 9) {
				pctx->chunksize = DEFAULT_CHUNKSIZE;
			} else {
				pctx->chunksize = DEFAULT_CHUNKSIZE + (pctx->level - 8) *
				    DEFAULT_CHUNKSIZE/4;
			}
		}

		if (pctx->archive_mode) {
			if (pctx->meta_stream != -1)
				pctx->meta_stream = 1;
			else
				pctx->meta_stream = 0;
		}

		if (pctx->pipe_mode)
			pctx->meta_stream = 0;

		/*
		 * Auto-select filters and preprocessing modes based on compresion level.
		 * This is not done if user explicitly specified advanced options.
		 */
		if (!pctx->advanced_opts) {
			/*
			 * Selectively enable filters while archiving, depending on compression level.
			 */
			if (pctx->archive_mode) {
				if (pctx->level > 10) {
					ff.enable_packjpg = 1;
					ff.enable_wavpack = 1;
				}
				if (pctx->level > 8) {
					pctx->exe_preprocess = 1;
					ff.exe_preprocess = 1;
				}
			}

			/*
			 * Enable other preprocessors based on compresion level.
			 */
			if (pctx->level > 4) pctx->enable_delta2_encode = 1;
			if (pctx->level > 9) pctx->lzp_preprocess = 1;
			if (pctx->level > 3) {
				/*
				 * Global deduplication cannot be decoded from a pipe,
				 * so never auto-enable it when the compressed output
				 * goes to stdout (pipe_out) or the input is a pipe
				 * (pipe_mode); otherwise a plain
				 * "pcompress -c <algo> -l<n> file -" or
				 * "pcompress -p -c <algo> -l<n>" stream would be
				 * undecodable via "pcompress -d -".
				 */
				if (!pctx->pipe_mode && !pctx->pipe_out &&
				    pctx->chunksize >= RAB_MIN_CHUNK_SIZE_GLOBAL)
					pctx->enable_rabin_global = 1;
				if (pctx->chunksize >= RAB_MIN_CHUNK_SIZE) {
					pctx->enable_rabin_scan = 1;
					pctx->enable_rabin_split = 1;
				}
				pctx->rab_blk_size = 2;
			}
			if (pctx->level > 9) pctx->delta2_nstrides = NSTRIDES_EXTRA;
		}
		/*
		 * Archive filters come from -j/-x or the level. This used to run
		 * only for the level-based choice, so -j and -x were ignored.
		 */
		if (pctx->archive_mode) {
			init_filters(pctx, &ff);
			pctx->enable_packjpg = ff.enable_packjpg;
			pctx->enable_wavpack = ff.enable_wavpack;
		}
		if (pctx->lzp_preprocess || pctx->enable_delta2_encode || pctx->exe_preprocess) {
			pctx->preprocess_mode = 1;
			pctx->enable_analyzer = 1;
		}
	} else if (pctx->do_uncompress) {
		struct filter_flags ff;
		/*
		 * Enable all filters while decompressing. Obviously!
		 */
		ff.enable_packjpg = 1;
		ff.enable_wavpack = 1;
		ff.exe_preprocess = 1;
		pctx->enable_packjpg = 1;
		pctx->enable_wavpack = 1;
		pctx->exe_preprocess = 1;
		init_filters(pctx, &ff);
	}
	pctx->inited = 1;

	return (0);
}

static int
init_pc_context_impl(pc_ctx_t *pctx, int argc, char *argv[])
{
	int opt, num_rem, err, my_optind;
	struct pc_getopt g;
	char *pos;
	struct filter_flags ff;

	pctx->level = -1;
	err = 0;
	pctx->keylen = DEFAULT_KEYLEN;
	pctx->chunksize = 0;
	pos = argv[0] + strlen(argv[0]);
	while (*pos != '/' && pos > argv[0]) pos--;
	if (*pos == '/') pos++;
	strcpy(pctx->exec_name, pos);
	pctx->advanced_opts = 0;
	ff.enable_packjpg = 0;
	ff.enable_wavpack = 0;
	ff.exe_preprocess = 0;

	memset(&g, 0, sizeof (g));
	g.ind = 1;
	g.prog = argv[0];
	g.opts = pc_malloc(argc * sizeof (char *));
	g.ops = pc_malloc(argc * sizeof (char *));
	if (g.opts == NULL || g.ops == NULL) {
		pc_mfree(g.opts);
		pc_mfree(g.ops);
		log_msg(LOG_ERR, 0, "Out of memory");
		return (1);
	}
	while ((opt = pc_getopt(&g, argc, argv, "dc:s:l:pt:MCDGEe:w:LPS:B:Fk:avmKjxiTnhV")) != -1) {
		char *oarg = g.arg;
		int ovr;
		int64_t chunksize;

		switch (opt) {
		    case 'i':
			pctx->list_mode = 1; // List mode also sets decompress flag
		    case 'd':
			pctx->do_uncompress = 1;
			break;

		    case 'c':
			pctx->do_compress = 1;
			pctx->algo = oarg;
			if (init_algo(pctx, pctx->algo, 1) != 0) {
				log_msg(LOG_ERR, 0, "Invalid algorithm %s", oarg);
				{ err = 1; goto opt_done; }
			}
			break;

		    case 's':
			ovr = parse_numeric(&chunksize, oarg);
			if (ovr == 1) {
				log_msg(LOG_ERR, 0, "Chunk size too large %s", oarg);
				{ err = 1; goto opt_done; }

			} else if (ovr == 2) {
				log_msg(LOG_ERR, 0, "Invalid number %s", oarg);
				{ err = 1; goto opt_done; }
			}
			pctx->chunksize = chunksize;

			if (pctx->chunksize < MIN_CHUNK) {
				log_msg(LOG_ERR, 0, "Minimum chunk size is %ld", MIN_CHUNK);
				{ err = 1; goto opt_done; }
			}
			if (pctx->chunksize > EIGHTY_PCT(get_total_ram())) {
				log_msg(LOG_ERR, 0, "Chunk size must not exceed 80%% of total RAM.");
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'l':
			pctx->level = atoi(oarg);
			if (pctx->level < 0 || pctx->level > MAX_LEVEL) {
				log_msg(LOG_ERR, 0, "Compression level should be in range 0 - 14");
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'B':
			pctx->rab_blk_size = atoi(oarg);
			if (pctx->rab_blk_size < 0 || pctx->rab_blk_size > 5) {
				log_msg(LOG_ERR, 0, "Average Dedupe block size must be in range 0 (2k), 1 (4k) .. 5 (64k)");
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'p':
			pctx->pipe_mode = 1;
			break;

		    case 't':
			pctx->nthreads = atoi(oarg);
			if (pctx->nthreads < 1 || pctx->nthreads > 256) {
				log_msg(LOG_ERR, 0, "Thread count should be in range 1 - 256");
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'M':	/* allocator stats: removed in 4.1, still accepted */
			break;

		    case 'C':
			if (pctx->hide_cmp_stats)
				pctx->hide_cmp_stats = 0;
			else
				pctx->show_chunks = 1;
			break;

		    case 'D':
			pctx->advanced_opts = 1;
			pctx->enable_rabin_scan = 1;
			break;

		    case 'G':
			pctx->advanced_opts = 1;
			pctx->enable_rabin_global = 1;
			break;

		    case 'E':
			pctx->advanced_opts = 1;
			pctx->enable_rabin_scan = 1;
			if (!pctx->enable_delta_encode)
				pctx->enable_delta_encode = DELTA_NORMAL;
			else
				pctx->enable_delta_encode = DELTA_EXTRA;
			break;

		    case 'e':
			pctx->encrypt_type = get_crypto_alg(oarg);
			if (pctx->encrypt_type == 0) {
				log_msg(LOG_ERR, 0, "Invalid encryption algorithm. "
				    "Should be AES or SALSA20.", oarg);
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'w':
			pctx->pwd_file = pc_strdup(oarg);
			break;

		    case 'F':
			pctx->advanced_opts = 1;
			pctx->enable_fixed_scan = 1;
			pctx->enable_rabin_split = 0;
			break;

#ifndef _MPLV2_LICENSE_
		    case 'L':
			pctx->advanced_opts = 1;
			pctx->lzp_preprocess = 1;
			break;
#endif

		    case 'P':
			pctx->advanced_opts = 1;
			pctx->enable_delta2_encode = 1;
			break;

		    case 'k':
			pctx->keylen = atoi(oarg);
			if ((pctx->keylen != 16 && pctx->keylen != 32) || pctx->keylen > MAX_KEYLEN) {
				log_msg(LOG_ERR, 0, "Encryption KEY length should be 16 or 32.", oarg);
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'S':
			if (get_checksum_props(oarg, &(pctx->cksum), &(pctx->cksum_bytes),
			    &(pctx->mac_bytes), 0) == -1) {
				log_msg(LOG_ERR, 0, "Invalid checksum type %s", oarg);
				{ err = 1; goto opt_done; }
			}
			break;

		    case 'a':
			pctx->archive_mode = 1;
			pctx->do_compress = 1;
			break;

		    case 'v':
			pctx->log.level = LOG_VERBOSE;	/* this context only */
			break;

		    case 'm':
			pctx->force_archive_perms = 1;
			break;

		    case 'K':
			pctx->no_overwrite_newer = 1;
			break;

		    case 'j':
			pctx->advanced_opts = 1;
			ff.enable_packjpg = 1;
			ff.enable_wavpack = 1;
			break;

		    case 'x':
			pctx->advanced_opts = 1;
			pctx->exe_preprocess = 1;
			ff.exe_preprocess = 1;
			break;

		    case 'T':
			pctx->meta_stream = -1;
			break;

		   case 'n':
			pctx->enable_archive_sort = -1;
			break;

		    case 'h':
			{ err = 3; goto opt_done; }

		    case 'V':
			printf("pcompress %s\n", UTILITY_VERSION);
			{ err = 4; goto opt_done; }

		    case '?':
		    default:
			{ err = 2; goto opt_done; }
			break;
		}
	}
opt_done:
	/* On success the loop ended at -1: argv is permuted, g.ind = first operand. */
	my_optind = g.ind;
	pc_mfree(g.opts);
	pc_mfree(g.ops);
	if (err)
		return (err);

	if ((pctx->do_compress && pctx->do_uncompress) || (!pctx->do_compress && !pctx->do_uncompress)) {
		return (2);
	}

	if (pctx->archive_mode && pctx->do_uncompress) {
		log_msg(LOG_ERR, 0, "'-a' flag is only for archive creation.");
		return (1);
	}

	if (pctx->archive_mode && pctx->pipe_mode) {
		log_msg(LOG_ERR, 0, "Full pipeline mode is meaningless with archiver.");
		return (1);
	}

	/*
	 * Default compression algorithm during archiving is Adaptive2.
	 */
	if (pctx->archive_mode && pctx->algo == NULL) {
		pctx->algo = "adapt2";
		init_algo(pctx, pctx->algo, 1);
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
		if ((memcmp(pctx->algo, "lz4", 3) == 0 && pctx->level > 1) || pctx->level > 4)
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

	/*
	 * EXE, PackJPG and WavPack are only valid when archiving files.
	 */
	if ((pctx->exe_preprocess || ff.enable_packjpg || ff.enable_wavpack)
	    && !pctx->archive_mode) {
		log_msg(LOG_ERR, 0, "Executable File Preprocessor and PackJPG are "
		    "only valid when archiving.");
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

					if ((filename = pc_realpath(argv[my_optind])) != NULL) {
						pc_mfree(filename);
						*fn = slab_alloc(NULL, sizeof (struct fn_list));
						(*fn)->filename = pc_strdup(argv[my_optind]);
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
					pctx->filename = pc_strdup(pctx->fn->filename);
			} else {
				if ((pctx->filename = pc_realpath(argv[my_optind])) == NULL) {
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
					if (!endswith(apath, COMP_EXTN))
						strcat(apath, COMP_EXTN);
					pctx->to_filename = pc_realpath(apath);

					/* Check if compressed file exists */
					if (pctx->to_filename != NULL) {
						log_msg(LOG_ERR, 0, "Compressed file %s exists",
						    pctx->to_filename);
						pc_mfree((void *)(pctx->to_filename));
						return (1);
					}
					pctx->to_filename = argv[my_optind];
				}
			} else {
				strcpy(apath, pctx->filename);
				strcat(apath, COMP_EXTN);
				pctx->to_filename = pc_realpath(apath);

				/* Check if compressed file exists */
				if (pctx->to_filename != NULL) {
					log_msg(LOG_ERR, 0, "Compressed file %s exists",
					    pctx->to_filename);
					pc_mfree((void *)(pctx->to_filename));
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
				if ((pctx->filename = pc_realpath(argv[my_optind])) == NULL) {
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
	__atomic_store_n(&pctx->main_cancel, 0, __ATOMIC_RELAXED);

	return (finish_settings(pctx, &ff));
}

int DLL_EXPORT
start_pcompress(pc_ctx_t *pctx)
{
	int err;

	if (!pctx->inited)
		return (1);

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

/*
 * Send this context's messages at or below level to fn(arg, level, msg)
 * instead of stderr. fn == NULL restores the process default sink.
 * level < 0 uses the process default level.
 */
void DLL_EXPORT
pc_set_log(pc_ctx_t *pctx, pc_log_fn fn, void *arg, int level)
{
	pctx->log.fn = fn;
	pctx->log.arg = arg;
	pctx->log.level = level;
}

/* See include/pcompress/pcompress.h; the job reads it from the calling thread. */
void DLL_EXPORT
pc_set_progress(pc_ctx_t *pctx, pc_progress_fn fn, void *arg)
{
	pctx->progress_fn = fn;
	pctx->progress_arg = arg;
}

/* The first line of the first error in the last call on this context, or "". */
const char DLL_EXPORT *
pc_last_error(pc_ctx_t *pctx)
{
	return (pctx->log.last_error);
}

/* The class of the first error in the last call on this context. */
pc_error_t DLL_EXPORT
pc_last_error_code(pc_ctx_t *pctx)
{
	return (pctx->log.code);
}
void DLL_EXPORT
pc_set_userpw(pc_ctx_t *pctx, unsigned char *pwdata, int pwlen)
{
	pctx->user_pw = pwdata;
	pctx->user_pw_len = pwlen;
}

/*
 * Public entry points bind the context's log state to the calling thread
 * for the duration of the call, so messages and the last error go to the
 * context (see log_bind()), and restore what was bound before.
 */
/* A failure no site classified more precisely. */
static void
finish_error(pc_ctx_t *pctx, int rv, pc_error_t fallback)
{
	if (rv != 0 && pctx->log.code == PC_OK)
		pctx->log.code = fallback;
}

int DLL_EXPORT
start_decompress(pc_ctx_t *pctx, const char *filename, char *to_filename)
{
	log_state_t *prev = log_bound();
	int rv;

	log_reset(&pctx->log);
	log_bind(&pctx->log);
	rv = start_decompress_impl(pctx, filename, to_filename);
	log_bind(prev);
	finish_error(pctx, rv, PC_ERR_INTERNAL);
	return (rv);
}

int DLL_EXPORT
start_compress(pc_ctx_t *pctx, const char *filename, uint64_t chunksize, int level)
{
	log_state_t *prev = log_bound();
	int rv;

	log_reset(&pctx->log);
	log_bind(&pctx->log);
	rv = start_compress_impl(pctx, filename, chunksize, level);
	log_bind(prev);
	finish_error(pctx, rv, PC_ERR_INTERNAL);
	return (rv);
}

int DLL_EXPORT
init_pc_context(pc_ctx_t *pctx, int argc, char *argv[])
{
	log_state_t *prev = log_bound();
	int rv;

	log_reset(&pctx->log);
	log_bind(&pctx->log);
	rv = init_pc_context_impl(pctx, argc, argv);
	log_bind(prev);
	/* 3 (-h) and 4 (-V) are not errors. */
	if (rv == 1 || rv == 2)
		finish_error(pctx, rv, PC_ERR_ARGS);
	return (rv);
}

/* --- Typed settings and descriptor I/O (include/pcompress/pcompress.h) --- */

const char DLL_EXPORT *
pc_version(void)
{
	return (UTILITY_VERSION);
}

int DLL_EXPORT
pc_api_version(void)
{
	return (PC_API_VERSION);
}

pc_ctx_t DLL_EXPORT *
pc_new(void)
{
	pc_ctx_t *c = create_pc_context();

	if (c != NULL)
		c->level = -1;	/* pick from the algorithm, as the CLI does */
	return (c);
}

void DLL_EXPORT
pc_free(pc_ctx_t *ctx)
{
	if (ctx != NULL)
		destroy_pc_context(ctx);
}

/* Setter failures are argument errors: record one for pc_last_error*(). */
static int
bad_arg(pc_ctx_t *ctx, const char *what, const char *val)
{
	log_state_t *prev = log_bound();

	log_reset(&ctx->log);
	log_bind(&ctx->log);
	log_error_code(PC_ERR_ARGS);
	log_msg(LOG_ERR, 0, "Invalid %s: %s", what, val ? val : "(null)");
	log_bind(prev);
	return (-1);
}

int DLL_EXPORT
pc_set_algorithm(pc_ctx_t *ctx, const char *name)
{
	if (name == NULL || init_algo(ctx, name, 1) != 0)
		return (bad_arg(ctx, "algorithm", name));
	/* Copied: the caller may free or reuse its string. Known names fit. */
	snprintf(ctx->algo_name, sizeof (ctx->algo_name), "%s", name);
	ctx->algo = ctx->algo_name;
	return (0);
}

int DLL_EXPORT
pc_set_level(pc_ctx_t *ctx, int level)
{
	char v[16];

	if (level < 1 || level > MAX_LEVEL) {
		snprintf(v, sizeof (v), "%d", level);
		return (bad_arg(ctx, "level (1-14)", v));
	}
	ctx->level = level;
	return (0);
}

int DLL_EXPORT
pc_set_chunk_size(pc_ctx_t *ctx, uint64_t bytes)
{
	char v[32];

	/* Same limits as -s. */
	if (bytes != 0 && (bytes < MIN_CHUNK || bytes > EIGHTY_PCT(get_total_ram()))) {
		snprintf(v, sizeof (v), "%" PRIu64, bytes);
		return (bad_arg(ctx, "chunk size", v));
	}
	ctx->chunksize = bytes;
	return (0);
}

int DLL_EXPORT
pc_set_threads(pc_ctx_t *ctx, int n)
{
	char v[16];

	if (n < 0 || n > 256) {
		snprintf(v, sizeof (v), "%d", n);
		return (bad_arg(ctx, "thread count (0-256)", v));
	}
	ctx->nthreads = n;
	return (0);
}

int DLL_EXPORT
pc_set_checksum(pc_ctx_t *ctx, const char *name)
{
	if (name == NULL || get_checksum_props(name, &ctx->cksum, &ctx->cksum_bytes,
	    &ctx->mac_bytes, 0) == -1)
		return (bad_arg(ctx, "checksum", name));
	return (0);
}

int DLL_EXPORT
pc_set_dedupe(pc_ctx_t *ctx, int mode)
{
	char v[16];

	ctx->advanced_opts = 1;	/* explicit choice: no level-based defaults */
	ctx->enable_rabin_scan = ctx->enable_fixed_scan = ctx->enable_rabin_global = 0;
	ctx->enable_rabin_split = 1;
	switch (mode) {
	case PC_DEDUPE_NONE:
		break;
	case PC_DEDUPE_RABIN:
		ctx->enable_rabin_scan = 1;
		break;
	case PC_DEDUPE_FIXED:
		ctx->enable_fixed_scan = 1;
		ctx->enable_rabin_split = 0;
		break;
	case PC_DEDUPE_GLOBAL:
		ctx->enable_rabin_global = 1;
		break;
	default:
		snprintf(v, sizeof (v), "%d", mode);
		return (bad_arg(ctx, "dedupe mode", v));
	}
	return (0);
}

int DLL_EXPORT
pc_set_encryption(pc_ctx_t *ctx, const char *alg, int keylen)
{
	char v[16];

	if (alg == NULL) {
		ctx->encrypt_type = 0;
		return (0);
	}
	if ((ctx->encrypt_type = get_crypto_alg((char *)alg)) == 0)
		return (bad_arg(ctx, "encryption algorithm", alg));
	if (keylen == 0)
		keylen = DEFAULT_KEYLEN;
	if (keylen != 16 && keylen != 32) {
		ctx->encrypt_type = 0;
		snprintf(v, sizeof (v), "%d", keylen);
		return (bad_arg(ctx, "key length (16 or 32)", v));
	}
	ctx->keylen = keylen;
	return (0);
}

int DLL_EXPORT
pc_set_password(pc_ctx_t *ctx, const void *pw, size_t len)
{
	if (ctx->pw_copy != NULL) {
		memset(ctx->pw_copy, 0, ctx->pw_copy_len);
		pc_mfree(ctx->pw_copy);
		ctx->pw_copy = NULL;
	}
	if (pw == NULL || len == 0 || len > INT32_MAX)
		return (bad_arg(ctx, "password", "empty"));
	if ((ctx->pw_copy = pc_malloc(len)) == NULL)
		return (bad_arg(ctx, "password", "out of memory"));
	memcpy(ctx->pw_copy, pw, len);
	ctx->pw_copy_len = len;
	return (0);
}

/*
 * The descriptor jobs reuse the CLI's stream ("pipe") mode: the same code
 * path, the same format, with in_fd/out_fd in place of stdin/stdout.
 * Stream mode cannot do global dedupe or archives, as with -p.
 *
 * A job leaves state behind in its context (algorithm handlers, chunk
 * counters, crypto keys, filter tables), so each job runs in a fresh
 * internal context that copies only the caller's settings; results
 * (the log, last error) are copied back.
 */
static int
run_fd_job(pc_ctx_t *ctx, int in_fd, int out_fd, int compress)
{
	struct filter_flags ff = { 0, 0, 0 };
	log_state_t *prev;
	pc_ctx_t *j;
	int rv;

	if (in_fd < 0 || out_fd < 0)
		return (bad_arg(ctx, "descriptor", "negative"));
	if (compress && ctx->enable_rabin_global)
		return (bad_arg(ctx, "dedupe mode", "global dedupe needs a file, not a stream"));
	if ((j = create_pc_context()) == NULL)
		return (bad_arg(ctx, "context", "out of memory"));

	/* The settings pc_set_*() can change; everything else is fresh. */
	j->log.fn = ctx->log.fn;
	j->log.arg = ctx->log.arg;
	j->log.level = ctx->log.level;
	j->progress_fn = ctx->progress_fn;
	j->progress_arg = ctx->progress_arg;
	j->nthreads = ctx->nthreads;
	j->do_compress = compress;
	j->do_uncompress = !compress;
	j->pipe_mode = 1;
	if (compress) {
		const char *algo = ctx->algo ? ctx->algo : "zstd";

		if (init_algo(j, algo, 1) != 0) {
			destroy_pc_context(j);
			return (bad_arg(ctx, "algorithm", algo));
		}
		j->algo = algo;
		j->level = ctx->level >= 1 ? ctx->level :
		    (memcmp(algo, "lz4", 3) == 0 ? 1 : 6);
		j->chunksize = ctx->chunksize;
		j->cksum = ctx->cksum;
		j->cksum_bytes = ctx->cksum_bytes;
		j->mac_bytes = ctx->mac_bytes;
		j->advanced_opts = ctx->advanced_opts;
		j->enable_rabin_scan = ctx->enable_rabin_scan;
		j->enable_fixed_scan = ctx->enable_fixed_scan;
		j->enable_rabin_split = ctx->enable_rabin_split;
		j->encrypt_type = ctx->encrypt_type;
		j->keylen = ctx->keylen ? ctx->keylen : DEFAULT_KEYLEN;
	}
	if (j->rab_blk_size == -1)
		j->rab_blk_size = 0;
	if (!j->enable_rabin_scan && !j->enable_fixed_scan)
		j->enable_rabin_split = 0;
	j->min_chunk = j->enable_rabin_scan ? RAB_MIN_CHUNK_SIZE : MIN_CHUNK;

	prev = log_bound();
	log_reset(&j->log);
	log_bind(&j->log);
	rv = finish_settings(j, &ff);
	if (rv == 0) {
		unsigned char *pw = NULL;

		j->in_fd = in_fd;
		j->out_fd = out_fd;
		if (ctx->pw_copy != NULL) {
			/* The job wipes what it is given: give it its own copy. */
			if ((pw = pc_malloc(ctx->pw_copy_len)) == NULL) {
				log_error_code(PC_ERR_NOMEM);
				log_msg(LOG_ERR, 0, "Out of memory");
				rv = 1;
			} else {
				memcpy(pw, ctx->pw_copy, ctx->pw_copy_len);
				pc_set_userpw(j, pw, (int)ctx->pw_copy_len);
			}
		}
		if (rv == 0)
			rv = compress ? start_compress_impl(j, NULL, j->chunksize, j->level) :
			    start_decompress_impl(j, NULL, NULL);
		if (pw != NULL) {
			memset(pw, 0, ctx->pw_copy_len);
			pc_mfree(pw);
		}
	}
	log_bind(prev);
	finish_error(j, rv, PC_ERR_INTERNAL);
	ctx->log.code = j->log.code;
	memcpy(ctx->log.last_error, j->log.last_error, sizeof (ctx->log.last_error));
	destroy_pc_context(j);
	return (rv == 0 ? 0 : -1);
}

int DLL_EXPORT
pc_compress_fd(pc_ctx_t *ctx, int in_fd, int out_fd)
{
	return (run_fd_job(ctx, in_fd, out_fd, 1));
}

int DLL_EXPORT
pc_decompress_fd(pc_ctx_t *ctx, int in_fd, int out_fd)
{
	return (run_fd_job(ctx, in_fd, out_fd, 0));
}

void DLL_EXPORT
pc_usage(pc_ctx_t *ctx)
{
	usage(ctx);
}

/* For chunk.c: set ctx's compress/decompress handlers for one algorithm. */
int
pc_init_algo_handlers(pc_ctx_t *pctx, const char *algo)
{
	return (init_algo(pctx, algo, 1));
}
