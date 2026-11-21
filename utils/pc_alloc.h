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
 * The library's own allocation calls. They go through the allocator a
 * host program installed with pc_set_allocator(), or the C library's.
 *
 * NB: memory from these is released only with pc_mfree() (or
 * pc_aligned_free()), and memory from the C library or another library
 * (realpath(), strdup(), packJPG output, OpenSSL objects) only with that
 * library's own release call. A host allocator need not be malloc(), so
 * mixing the two is a crash or a heap corruption.
 */

#ifndef	_PC_ALLOC_H
#define	_PC_ALLOC_H

#include <stddef.h>

#ifdef	__cplusplus
extern "C" {
#endif

void *pc_malloc(size_t n);
/* Zeroed; NULL (and no call to the hook) if nmemb * size overflows. */
void *pc_calloc(size_t nmemb, size_t size);
void *pc_realloc(void *p, size_t n);
/* NULL is a no-op; the hook never sees NULL. */
void pc_mfree(void *p);
char *pc_strdup(const char *s);
/* realpath(path, NULL) in pc_malloc() memory. */
char *pc_realpath(const char *path);
/* n bytes aligned to align (a power of two); release with pc_aligned_free(). */
void *pc_aligned_alloc(size_t align, size_t n);
void pc_aligned_free(void *p);
/*
 * Move n bytes of C library memory (malloc()ed by a bundled codec) into
 * pc_malloc() memory: the copy, or NULL; p is free()d either way.
 */
void *pc_adopt(void *p, size_t n);

#ifdef	__cplusplus
}
#endif

#endif	/* _PC_ALLOC_H */
