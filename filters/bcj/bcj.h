/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Use is subject to license terms: bcj.c is 0BSD (from XZ Utils).
 */

/*
 * ARM64 and RISC-V branch filters for archive members (bcj.c). Each
 * function converts a buffer in place; decode undoes encode exactly.
 * Any input is safe to decode: the coders touch only bytes inside it.
 */

#ifndef	_BCJ_H
#define	_BCJ_H

#include <stddef.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C" {
#endif

void bcj_arm64_encode(uint8_t *buf, size_t size);
void bcj_arm64_decode(uint8_t *buf, size_t size);
void bcj_riscv_encode(uint8_t *buf, size_t size);
void bcj_riscv_decode(uint8_t *buf, size_t size);

#ifdef	__cplusplus
}
#endif

#endif
