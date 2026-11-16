/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/*
 * <sys/mman.h> for Windows builds (on the include path only there):
 * read-only file mappings and anonymous private memory, the two uses
 * pcompress has. utils/win32.c implements them.
 */

#ifndef	_PC_WIN32_SYS_MMAN_H
#define	_PC_WIN32_SYS_MMAN_H

#include <sys/types.h>

#define	PROT_NONE	0
#define	PROT_READ	1
#define	PROT_WRITE	2
#define	MAP_SHARED	1
#define	MAP_PRIVATE	2
#define	MAP_ANON	0x20
#define	MAP_ANONYMOUS	MAP_ANON
#define	MAP_FAILED	((void *)-1)

#ifdef	__cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);

#ifdef	__cplusplus
}
#endif

#endif
