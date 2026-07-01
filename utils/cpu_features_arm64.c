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
 * ARM64 (AArch64) CPU feature detection.
 *
 * On Linux, uses getauxval(AT_HWCAP) to detect NEON and crypto extensions.
 * On macOS (Apple Silicon), uses sysctlbyname.
 * NEON is mandatory on AArch64, so it is always reported as available.
 */

#if defined(__aarch64__) || defined(__arm64__)

#include "cpu_features.h"

#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>

void
cpu_features_detect_arm64(cpu_features_t *feat)
{
	unsigned long hwcap;

	feat->arch = ARCH_ARM64;
	/* NEON is mandatory on AArch64 */
	feat->features = CPU_FEAT_SIMD_BASE | CPU_FEAT_NEON;

	hwcap = getauxval(AT_HWCAP);

	if (hwcap & HWCAP_AES)
		feat->features |= CPU_FEAT_AES | CPU_FEAT_CRYPTO;

	if (hwcap & HWCAP_SHA2)
		feat->features |= CPU_FEAT_SHA | CPU_FEAT_CRYPTO;

	if (hwcap & HWCAP_CRC32)
		feat->features |= CPU_FEAT_CRC32;
}

#elif defined(__APPLE__)
#include <sys/sysctl.h>

static int
sysctl_has_feature(const char *name)
{
	int val = 0;
	size_t len = sizeof(val);
	if (sysctlbyname(name, &val, &len, NULL, 0) == 0)
		return val;
	return 0;
}

void
cpu_features_detect_arm64(cpu_features_t *feat)
{
	feat->arch = ARCH_ARM64;
	/* NEON is mandatory on AArch64 / Apple Silicon */
	feat->features = CPU_FEAT_SIMD_BASE | CPU_FEAT_NEON;

	/* Apple Silicon always has crypto extensions, but verify */
	if (sysctl_has_feature("hw.optional.arm.FEAT_AES"))
		feat->features |= CPU_FEAT_AES | CPU_FEAT_CRYPTO;

	if (sysctl_has_feature("hw.optional.arm.FEAT_SHA256"))
		feat->features |= CPU_FEAT_SHA | CPU_FEAT_CRYPTO;

	if (sysctl_has_feature("hw.optional.armv8_crc32"))
		feat->features |= CPU_FEAT_CRC32;
}

#else
/* Fallback: NEON only, no crypto extension detection */
void
cpu_features_detect_arm64(cpu_features_t *feat)
{
	feat->arch = ARCH_ARM64;
	feat->features = CPU_FEAT_SIMD_BASE | CPU_FEAT_NEON;
}

#endif /* platform */

#else
/* Avoid empty translation unit warning on non-ARM platforms */
typedef int cpu_features_arm64_unused;
#endif /* __aarch64__ || __arm64__ */
