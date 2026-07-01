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
 * BLAKE2b round function macros using ARM NEON intrinsics.
 *
 * ARM NEON provides 128-bit SIMD registers (uint64x2_t) which map
 * naturally to the BLAKE2b state (pairs of 64-bit words).
 * This provides equivalent functionality to the SSE2/SSSE3 x86 round macros.
 */
#pragma once
#ifndef __BLAKE2B_ROUND_NEON_H__
#define __BLAKE2B_ROUND_NEON_H__

#include <arm_neon.h>

#define LOAD(p)  vld1q_u64((const uint64_t *)(p))
#define STORE(p, r) vst1q_u64((uint64_t *)(p), (r))

#define LOADU(p) vld1q_u64((const uint64_t *)(p))
#define STOREU(p, r) vst1q_u64((uint64_t *)(p), (r))

#define LIKELY(x) __builtin_expect((x), 1)

/*
 * 64-bit rotate right using NEON.
 * NEON does not have a native 64-bit rotate, so we implement it
 * with shift-right + shift-left + OR, with special cases for
 * common rotation amounts used in BLAKE2b (32, 24, 16, 63).
 */
static inline uint64x2_t neon_ror64_32(uint64x2_t x)
{
	/* 32-bit rotation: reinterpret as 32-bit and reverse pairs */
	return vreinterpretq_u64_u32(vrev64q_u32(vreinterpretq_u32_u64(x)));
}

static inline uint64x2_t neon_ror64(uint64x2_t x, int c)
{
	return vorrq_u64(vshrq_n_u64(x, c), vshlq_n_u64(x, 64 - c));
}

/*
 * BLAKE2b G function - quarter round.
 * Operates on pairs of 64-bit values packed in uint64x2_t.
 */
#define G1_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1) \
  row1l = vaddq_u64(vaddq_u64(row1l, b0), row2l); \
  row1h = vaddq_u64(vaddq_u64(row1h, b1), row2h); \
  \
  row4l = veorq_u64(row4l, row1l); \
  row4h = veorq_u64(row4h, row1h); \
  \
  row4l = neon_ror64_32(row4l); \
  row4h = neon_ror64_32(row4h); \
  \
  row3l = vaddq_u64(row3l, row4l); \
  row3h = vaddq_u64(row3h, row4h); \
  \
  row2l = veorq_u64(row2l, row3l); \
  row2h = veorq_u64(row2h, row3h); \
  \
  row2l = neon_ror64(row2l, 24); \
  row2h = neon_ror64(row2h, 24);

#define G2_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1) \
  row1l = vaddq_u64(vaddq_u64(row1l, b0), row2l); \
  row1h = vaddq_u64(vaddq_u64(row1h, b1), row2h); \
  \
  row4l = veorq_u64(row4l, row1l); \
  row4h = veorq_u64(row4h, row1h); \
  \
  row4l = neon_ror64(row4l, 16); \
  row4h = neon_ror64(row4h, 16); \
  \
  row3l = vaddq_u64(row3l, row4l); \
  row3h = vaddq_u64(row3h, row4h); \
  \
  row2l = veorq_u64(row2l, row3l); \
  row2h = veorq_u64(row2h, row3h); \
  \
  row2l = neon_ror64(row2l, 63); \
  row2h = neon_ror64(row2h, 63);

/*
 * Diagonalize/Undiagonalize using NEON vext (byte-level extract).
 * vextq_u64 shifts by 64-bit lane, equivalent to _mm_alignr_epi8(..., 8).
 */
#define DIAGONALIZE_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h) \
  do { \
    uint64x2_t t0, t1; \
    t0 = vextq_u64(row2l, row2h, 1); \
    t1 = vextq_u64(row2h, row2l, 1); \
    row2l = t0; \
    row2h = t1; \
    \
    t0 = row3l; \
    row3l = row3h; \
    row3h = t0; \
    \
    t0 = vextq_u64(row4l, row4h, 1); \
    t1 = vextq_u64(row4h, row4l, 1); \
    row4l = t1; \
    row4h = t0; \
  } while(0)

#define UNDIAGONALIZE_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h) \
  do { \
    uint64x2_t t0, t1; \
    t0 = vextq_u64(row2h, row2l, 1); \
    t1 = vextq_u64(row2l, row2h, 1); \
    row2l = t0; \
    row2h = t1; \
    \
    t0 = row3l; \
    row3l = row3h; \
    row3h = t0; \
    \
    t0 = vextq_u64(row4h, row4l, 1); \
    t1 = vextq_u64(row4l, row4h, 1); \
    row4l = t1; \
    row4h = t0; \
  } while(0)

/*
 * Message loading macros for NEON.
 * These load pairs of 64-bit message words into uint64x2_t vectors.
 * The BLAKE2b sigma schedule is the same across all implementations.
 */
static const uint8_t blake2b_sigma_neon[12][16] = {
  {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
  { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
  { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
  {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
  {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
  {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
  { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
  { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
  {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
  { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
  {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
  { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

#define LOAD_MSG_NEON(buf, r, i, j) \
  vcombine_u64( \
    vld1_u64((const uint64_t *)((buf) + blake2b_sigma_neon[r][2*(i)])), \
    vld1_u64((const uint64_t *)((buf) + blake2b_sigma_neon[r][2*(i)+1])) \
  )

#define ROUND_NEON(r, buf) \
  do { \
    uint64x2_t b0, b1; \
    b0 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][0]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][1])); \
    b1 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][2]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][3])); \
    G1_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1); \
    b0 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][4]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][5])); \
    b1 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][6]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][7])); \
    G2_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1); \
    DIAGONALIZE_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h); \
    b0 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][8]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][9])); \
    b1 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][10]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][11])); \
    G1_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1); \
    b0 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][12]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][13])); \
    b1 = vcombine_u64( \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][14]), \
      vld1_u64((const uint64_t *)(buf) + blake2b_sigma_neon[r][15])); \
    G2_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h,b0,b1); \
    UNDIAGONALIZE_NEON(row1l,row2l,row3l,row4l,row1h,row2h,row3h,row4h); \
  } while(0)

#endif /* __BLAKE2B_ROUND_NEON_H__ */
