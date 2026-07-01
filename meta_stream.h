/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2014 Moinak Ghosh. All rights reserved.
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
 * @file meta_stream.h
 * @brief Metadata stream handling for archive pathname data.
 *
 * In archive mode, pathname metadata is packed into separate chunks
 * distinct from file data. This improves compression by grouping
 * similar metadata together. Metadata chunks are identified in the
 * output stream by a special indicator value in the compressed length
 * field.
 */

#ifndef	_META_STREAM_H
#define	_META_STREAM_H

#ifdef	__cplusplus
extern "C" {
#endif

/** Compressed length value that indicates a metadata chunk. */
#define METADATA_INDICATOR	1

/**
 * Metadata chunk header format:
 * - 64-bit integer = 1: Compressed length (metadata indicator)
 * - 64-bit integer: Compressed length (data portion only)
 * - 64-bit integer: Uncompressed original length
 * - 1 Byte: Chunk flag
 * - Up to 64 bytes: Checksum (HMAC if encrypting)
 * - 32-bit integer: Header CRC32 if not encrypting, otherwise empty
 */
#define CKSUM_MAX		64
#define CRC32_SIZE		4
#define	METADATA_HDR_SZ		(8 * 3 + 1 + CKSUM_MAX + CRC32_SIZE)

/** Opaque metadata context. */
typedef struct _meta_ctx meta_ctx_t;

/** Message structure for metadata stream communication. */
typedef struct _meta_msg {
	const uchar_t *buf;  /**< Buffer pointer. */
	size_t len;          /**< Buffer length. */
} meta_msg_t;

/**
 * Create a metadata stream context.
 * @param pc            Pointer to pc_ctx_t (cast to void*).
 * @param file_version  File format version.
 * @param comp_fd       File descriptor for compressed output.
 * @return Allocated metadata context, or NULL on failure.
 */
meta_ctx_t *meta_ctx_create(void *pc, int file_version, int comp_fd);

/**
 * Send data through the metadata stream for compression.
 * @param mctx  Metadata context.
 * @param buf   In/out: pointer to buffer.
 * @param len   In/out: pointer to buffer length.
 * @return 0 on success, -1 on failure.
 */
int meta_ctx_send(meta_ctx_t *mctx, const void **buf, size_t *len);

/**
 * Signal that all metadata has been sent. Flushes remaining data.
 * @param mctx  Metadata context.
 * @return 0 on success.
 */
int meta_ctx_done(meta_ctx_t *mctx);

/** Close the sink (writer) side of the metadata channel. */
void meta_ctx_close_sink_channel(meta_ctx_t *mctx);

/** Close the source (reader) side of the metadata channel. */
void meta_ctx_close_src_channel(meta_ctx_t *mctx);

#ifdef	__cplusplus
}
#endif

#endif
