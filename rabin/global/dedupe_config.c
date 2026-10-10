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
 */

#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <rabin_dedup.h>

#include "dedupe_config.h"
#include "index.h"

static compress_algo_t
get_compress_level(compress_algo_t algo)
{
	switch (algo) {
		case COMPRESS_NONE:
		case COMPRESS_INVALID:
			return (0);
		case COMPRESS_LZFX:
			return (5);
		case COMPRESS_LZ4:
			return (1);
		case COMPRESS_ZLIB:
		case COMPRESS_BZIP2:
		case COMPRESS_LZMA:
			return (6);
	};
	return (0);
}

static int
get_compress_algo(const char *algo_name)
{
	if (strcmp(algo_name, "none") == 0) {
		return (COMPRESS_NONE);

	} else if (strcmp(algo_name, "lzfx") == 0) {
		return (COMPRESS_LZFX);

	} else if (strcmp(algo_name, "lz4") == 0) {
		return (COMPRESS_LZ4);

	} else if (strcmp(algo_name, "zlib") == 0) {
		return (COMPRESS_ZLIB);

	} else if (strcmp(algo_name, "bzip2") == 0) {
		return (COMPRESS_BZIP2);

	} else if (strcmp(algo_name, "lzma") == 0) {
		return (COMPRESS_LZMA);
	}
	return (COMPRESS_INVALID);
}

static int
get_cksum_sz(cksum_t ck)
{
	if (ck == CKSUM_CRC64) {
		return (8);

	} else if (ck == CKSUM_SHA256 || ck == CKSUM_BLAKE256 || ck == CKSUM_KECCAK256 ||
	    ck == CKSUM_SKEIN256) {
		return (32);

	} else if (ck == CKSUM_SHA512 || ck == CKSUM_BLAKE512 || ck == CKSUM_KECCAK512 ||
	    ck == CKSUM_SKEIN512) {
		return (64);
	}
	return (0);
}

int
set_config_s(archive_config_t *cfg, const char *algo, cksum_t ck, cksum_t ck_sim,
	     uint32_t chunksize, size_t file_sz, uint64_t user_chunk_sz, int pct_interval)
{
	cfg->algo = get_compress_algo(algo);
	cfg->chunk_cksum_type = ck;
	cfg->similarity_cksum = ck_sim;
	cfg->compress_level = get_compress_level(cfg->algo);
	cfg->chunk_cksum_sz = get_cksum_sz(cfg->chunk_cksum_type);
	cfg->similarity_cksum_sz = get_cksum_sz(cfg->similarity_cksum);
	cfg->chunk_sz = chunksize; // Chunk size indicator 1 - 5.
				  // Allows segment to be sized appropriately: 1 - 8M .. 5 - 40M
	cfg->chunk_sz_bytes = RAB_BLK_AVG_SZ(cfg->chunk_sz);
	cfg->pct_interval = pct_interval;
	cfg->archive_sz = file_sz;
	cfg->dedupe_mode = MODE_SIMILARITY;

	if (pct_interval == 0 || pct_interval == 100) {
		cfg->dedupe_mode = MODE_SIMPLE;
		cfg->segment_sz_bytes = user_chunk_sz;
		cfg->similarity_cksum_sz = cfg->chunk_cksum_sz;
	} else {
		if (cfg->chunk_sz == 0) {
			cfg->segment_sz_bytes = FOUR_MB;
		} else {
			cfg->segment_sz_bytes = EIGHT_MB * cfg->chunk_sz;
		}
	}

	cfg->segment_sz = cfg->segment_sz_bytes / cfg->chunk_sz_bytes;
	return (0);
}

