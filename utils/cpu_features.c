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
 * Platform-agnostic CPU feature detection dispatch.
 * Routes to architecture-specific implementations.
 */

#include "cpu_features.h"

/*
 * Architecture-specific detection functions.
 * Defined in cpu_features_x86_64.c, cpu_features_arm64.c,
 * and cpu_features_riscv.c respectively.
 */
#if defined(__x86_64__) || defined(__amd64__)
extern void cpu_features_detect_x86_64(cpu_features_t *feat);
#elif defined(__aarch64__) || defined(__arm64__)
extern void cpu_features_detect_arm64(cpu_features_t *feat);
#elif defined(__riscv) && (__riscv_xlen == 64)
extern void cpu_features_detect_riscv(cpu_features_t *feat);
#endif

void
cpu_features_detect(cpu_features_t *feat)
{
	feat->arch = ARCH_UNKNOWN;
	feat->features = CPU_FEAT_NONE;

#if defined(__x86_64__) || defined(__amd64__)
	cpu_features_detect_x86_64(feat);
#elif defined(__aarch64__) || defined(__arm64__)
	cpu_features_detect_arm64(feat);
#elif defined(__riscv) && (__riscv_xlen == 64)
	cpu_features_detect_riscv(feat);
#endif
}
