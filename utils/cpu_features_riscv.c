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
 * RISC-V CPU feature detection placeholder.
 *
 * Currently provides scalar-only support. RISC-V Vector extension (RVV)
 * detection can be added here once the ecosystem matures.
 */

#if defined(__riscv) && (__riscv_xlen == 64)

#include "cpu_features.h"

void
cpu_features_detect_riscv(cpu_features_t *feat)
{
	feat->arch = ARCH_RISCV;
	feat->features = CPU_FEAT_NONE;

	/*
	 * TODO: Detect RISC-V Vector extension (RVV) when available.
	 * On Linux this would use getauxval(AT_HWCAP) with COMPAT_HWCAP_ISA_V.
	 */
}

#else
/* Avoid empty translation unit warning on non-RISC-V platforms */
typedef int cpu_features_riscv_unused;
#endif /* __riscv && 64-bit */
