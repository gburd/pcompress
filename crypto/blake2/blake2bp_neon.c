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

/*
 * BLAKE2bp (parallel BLAKE2b) NEON variant.
 * blake2bp uses the single-block blake2b functions from the NEON
 * implementation via the BLAKE_NAMESPACE macro.
 */

#if defined(__aarch64__) || defined(__arm64__)
#define BLAKE_NAMESPACE(x) x##_neon
#include "blake2bp.c"
#else
/* Avoid empty translation unit warning on non-ARM platforms */
typedef int blake2bp_neon_unused;
#endif
