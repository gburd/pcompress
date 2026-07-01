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
 * @file pc_archive.h
 * @brief PAX archiving subsystem built on libarchive.
 *
 * Provides functions for creating and extracting PAX archives. In archive
 * mode (-a), files and directories are streamed through libarchive into
 * the compression pipeline. Content-aware filters can be applied per-file
 * based on detected type (JPEG, WAV, executable, etc.).
 */

#ifndef	_PC_ARCHIVE_H
#define	_PC_ARCHIVE_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pcompress.h>
#include <pc_arc_filter.h>

#ifdef	__cplusplus
extern "C" {
#endif

/** Entry in the archive member list used for sorting. */
typedef struct {
	char *fpath;    /**< File path. */
	int typeflag;   /**< File type flag. */
	size_t size;    /**< File size in bytes. */
} archive_list_entry_t;

/** @name Archive Creation */
/**@{*/

/**
 * Configure the archiver for a new archive creation session.
 * @param pctx  Pcompress context (must have archive_mode set).
 * @param sbuf  Stat buffer for initial size estimation.
 * @return 0 on success, -1 on failure.
 */
int setup_archiver(pc_ctx_t *pctx, struct stat *sbuf);

/**
 * Start the archiver thread. Runs in a separate thread, feeding the PAX
 * stream into the compression pipeline.
 * @param pctx  Configured Pcompress context.
 * @return 0 on success, -1 on failure.
 */
int start_archiver(pc_ctx_t *pctx);

/**@}*/

/** @name Archive Extraction */
/**@{*/

/**
 * Configure the extractor for archive extraction.
 * @param pctx  Pcompress context.
 * @return 0 on success, -1 on failure.
 */
int setup_extractor(pc_ctx_t *pctx);

/**
 * Start the extraction process.
 * @param pctx  Configured Pcompress context.
 * @return 0 on success, -1 on failure.
 */
int start_extractor(pc_ctx_t *pctx);

/**@}*/

/** @name Archive I/O Callbacks */
/**@{*/

/** Read callback for libarchive integration. */
int64_t archiver_read(void *ctx, void *buf, uint64_t count);

/** Write callback for libarchive integration. */
int64_t archiver_write(void *ctx, void *buf, uint64_t count);

/** Close callback for libarchive integration. */
int archiver_close(void *ctx);

/**@}*/

/** @name Filter Management */
/**@{*/

/** Initialize the archive module (register built-in filters). */
int init_archive_mod();

/**
 * Register a content filter for a specific file extension.
 * @param func            Filter function pointer.
 * @param filter_private  Filter-specific private data.
 * @param ext             File extension to match (e.g., ".jpg").
 * @return 0 on success, -1 on failure.
 */
int insert_filter_data(filter_func_ptr func, void *filter_private, const char *ext);

/** Initialize filter flags from configuration. */
void init_filters(struct filter_flags *ff);

/** Disable all content filters. */
void disable_all_filters();


#ifdef	__cplusplus
}
#endif

#endif
