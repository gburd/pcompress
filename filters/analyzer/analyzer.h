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
 */

/**
 * @file analyzer.h
 * @brief Data type analyzer for adaptive compression mode.
 *
 * Analyzes input buffers to determine data type characteristics (text,
 * binary, high-entropy, etc.) at multiple significance thresholds.
 * Used by adaptive modes to select the best compression algorithm
 * per chunk.
 */

#ifndef	_ANALYZER_H
#define	_ANALYZER_H

#ifdef  __cplusplus
extern "C" {
#endif

/** Significance value holding a data type classification. */
struct significance_value {
	int btype;  /**< Data type (DATA_TEXT, DATA_BINARY, etc.) */
};

/**
 * @brief Analyzer context with multi-threshold data classification.
 *
 * Contains data type classifications at 10%, 30%, and 50% significance
 * thresholds. Higher thresholds indicate stronger confidence in the
 * classification.
 */
typedef struct _analyzer_ctx {
	struct significance_value ten_pct;    /**< Classification at 10% threshold. */
	struct significance_value thirty_pct; /**< Classification at 30% threshold. */
	struct significance_value fifty_pct;  /**< Classification at 50% threshold. */
} analyzer_ctx_t;

/**
 * Analyze a buffer and populate the multi-threshold analyzer context.
 * @param src     Input data buffer.
 * @param srclen  Length of the input buffer.
 * @param actx    Output: analyzer context with classifications.
 */
void analyze_buffer(void *src, uint64_t srclen, analyzer_ctx_t *actx);

/**
 * Perform a simple single-threshold analysis of a buffer.
 * @param src     Input data buffer.
 * @param srclen  Length of the input buffer.
 * @return Data type classification (DATA_TEXT, DATA_BINARY, etc.).
 */
int analyze_buffer_simple(void *src, uint64_t srclen);

#ifdef  __cplusplus
}
#endif

#endif
