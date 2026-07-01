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
 * x86_64 CPU feature detection for the cpu_features abstraction.
 * Bridges the existing cpuid.c data into the unified interface.
 */

#ifdef __x86_64__

#include "cpu_features.h"
#include "cpuid.h"

void
cpu_features_detect_x86_64(cpu_features_t *feat)
{
	processor_cap_t pc;

	feat->arch = ARCH_X86_64;
	feat->features = CPU_FEAT_SIMD_BASE; /* x86_64 always has SSE2 */

	cpuid_basic_identify(&pc);

	if (pc.sse_level >= 3 && pc.sse_sub_level >= 1)
		feat->features |= CPU_FEAT_SIMD_EXT1; /* SSSE3 */

	if (pc.sse_level >= 4) {
		if (pc.sse_sub_level >= 1)
			feat->features |= CPU_FEAT_SIMD_EXT2; /* SSE4.1 */
		if (pc.sse_sub_level >= 2) {
			feat->features |= CPU_FEAT_SIMD_EXT3; /* SSE4.2 */
			feat->features |= CPU_FEAT_CRC32;
		}
	}

	if (pc.avx_level >= 1)
		feat->features |= CPU_FEAT_SIMD_WIDE; /* AVX */
	if (pc.avx_level >= 2)
		feat->features |= CPU_FEAT_SIMD_WIDE2; /* AVX2 */

	if (pc.avx512_avail)
		feat->features |= CPU_FEAT_AVX512;

	if (pc.aes_avail)
		feat->features |= CPU_FEAT_AES;

	if (pc.xop_avail)
		feat->features |= CPU_FEAT_XOP;
}

#endif /* __x86_64__ */
