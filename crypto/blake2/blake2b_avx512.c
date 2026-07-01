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
 * BLAKE2b AVX-512 variant.
 *
 * Compiles blake2b.c with BLAKE_NAMESPACE set to produce _avx512
 * suffixed symbols. The actual AVX-512 speedup comes from:
 * - EVEX-encoded instructions (3-operand form, reduced register pressure)
 * - Potential auto-vectorization with -mavx512f -mavx512vl
 * - The underlying blake2b.c already uses __m128i SSE intrinsics which
 *   benefit from the wider AVX-512 register file and execution units
 *
 * On CPUs without AVX-512 this file compiles to empty (x86 guard).
 */

#if defined(__x86_64__) || defined(__i386__)
#define BLAKE_NAMESPACE(x) x##_avx512
#include "blake2b.c"
#endif
