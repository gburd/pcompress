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

/**
 * @file cpu_features.h
 * @brief Platform-agnostic CPU feature detection abstraction.
 *
 * Provides a unified interface for querying CPU SIMD capabilities
 * across x86_64 (SSE/AVX), ARM64 (NEON/crypto extensions), and
 * RISC-V (scalar, with RVV placeholder). This is the recommended
 * API for new code that needs to be portable across architectures.
 *
 * Usage:
 * @code
 *   cpu_features_t feat;
 *   cpu_features_detect(&feat);
 *   if (cpu_has_feature(&feat, CPU_FEAT_AES)) {
 *       // Use hardware AES
 *   }
 * @endcode
 */
#ifndef __CPU_FEATURES_H__
#define __CPU_FEATURES_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CPU architecture identification. */
typedef enum {
	ARCH_UNKNOWN = 0,  /**< Unknown or unsupported architecture. */
	ARCH_X86_64,       /**< x86-64 (AMD64/Intel 64). */
	ARCH_ARM64,        /**< ARM AArch64. */
	ARCH_RISCV         /**< RISC-V 64-bit. */
} cpu_arch_t;

/*
 * Unified feature flags across architectures.
 * On x86_64, these map to SSE/AVX levels.
 * On ARM64, NEON is baseline; crypto extensions are optional.
 * On RISC-V, currently scalar only.
 */
#define CPU_FEAT_NONE       0
#define CPU_FEAT_SIMD_BASE  (1 << 0)  /* SSE2 on x86, NEON on ARM64 */
#define CPU_FEAT_SIMD_EXT1  (1 << 1)  /* SSSE3 on x86, unused on ARM64 */
#define CPU_FEAT_SIMD_EXT2  (1 << 2)  /* SSE4.1 on x86, unused on ARM64 */
#define CPU_FEAT_SIMD_EXT3  (1 << 3)  /* SSE4.2 on x86, unused on ARM64 */
#define CPU_FEAT_SIMD_WIDE  (1 << 4)  /* AVX on x86, unused on ARM64 */
#define CPU_FEAT_SIMD_WIDE2 (1 << 5)  /* AVX2 on x86, unused on ARM64 */
#define CPU_FEAT_AES        (1 << 6)  /* AES-NI on x86, CE AES on ARM64 */
#define CPU_FEAT_SHA        (1 << 7)  /* SHA-NI on x86, CE SHA on ARM64 */
#define CPU_FEAT_CRC32      (1 << 8)  /* SSE4.2 CRC on x86, CE CRC on ARM64 */
#define CPU_FEAT_CRYPTO     (1 << 9)  /* General crypto extensions */
#define CPU_FEAT_NEON       (1 << 10) /* ARM NEON (always set on AArch64) */
#define CPU_FEAT_AVX512     (1 << 11) /* AVX-512F on x86 */
#define CPU_FEAT_XOP        (1 << 12) /* AMD XOP on x86 */

/** Detected CPU feature set. */
typedef struct {
	cpu_arch_t arch;    /**< Detected architecture. */
	uint32_t features;  /**< Bitmask of CPU_FEAT_* flags. */
} cpu_features_t;

/**
 * Detect CPU features for the current platform.
 * @param feat  Output: populated with architecture and feature flags.
 */
void cpu_features_detect(cpu_features_t *feat);

/**
 * Query whether a specific feature (or set of features) is available.
 * @param feat  Feature set from cpu_features_detect().
 * @param flag  One or more CPU_FEAT_* flags OR'd together.
 * @return Non-zero if all requested features are present.
 */
static inline int cpu_has_feature(const cpu_features_t *feat, uint32_t flag)
{
	return (feat->features & flag) == flag;
}

/**
 * Get the detected architecture type.
 * @param feat  Feature set from cpu_features_detect().
 * @return The cpu_arch_t value.
 */
static inline cpu_arch_t cpu_get_arch(const cpu_features_t *feat)
{
	return feat->arch;
}

#ifdef __cplusplus
}
#endif

#endif /* __CPU_FEATURES_H__ */
