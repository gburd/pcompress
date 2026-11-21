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
 * Allocation wrappers. Pcompress used to ship a slab allocator here; it was
 * removed in 4.1 because it was process-global (unsafe with several contexts
 * in one process), called abort() on a bad free, and measured no faster than
 * the system allocator (lz4/zstd/lzma/libbsc/ppmd/adapt2, 1 MB-256 MB chunks).
 * The slab_* names remain so call sites did not change; they and the pc_*
 * functions below go through the allocator of pc_set_allocator().
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <utils.h>
#include "allocator.h"
#include "pc_alloc.h"

/*
 * The process's allocator: malloc == NULL means the C library's. Written
 * only by pc_set_allocator() before anything is allocated (mem_used is
 * still 0), so the allocation calls read it without a lock.
 */
static pc_allocator_t mem;
static int mem_used;

/* First allocation: from now on pc_set_allocator() refuses. */
static void
mark_used(void)
{
	if (!__atomic_load_n(&mem_used, __ATOMIC_RELAXED))
		__atomic_store_n(&mem_used, 1, __ATOMIC_RELAXED);
}

/*
 * Install the process's allocator (a NULL a: the C library's). Refused,
 * with nothing changed, once the library has allocated anything, or for
 * a struct that is too small or lacks one of the three functions.
 */
int DLL_EXPORT
pc_set_allocator(const pc_allocator_t *a)
{
	if (__atomic_load_n(&mem_used, __ATOMIC_ACQUIRE))
		return (-1);
	if (a == NULL) {
		memset(&mem, 0, sizeof (mem));
		return (0);
	}
	if (a->size < sizeof (pc_allocator_t) || a->malloc == NULL ||
	    a->realloc == NULL || a->free == NULL)
		return (-1);
	/* A newer caller's struct may be longer: take the fields we know. */
	memcpy(&mem, a, sizeof (mem));
	mem.size = sizeof (mem);
	return (0);
}

void *
pc_malloc(size_t n)
{
	mark_used();
	if (mem.malloc == NULL)
		return (malloc(n));
	return (mem.malloc(mem.ud, n));
}

void *
pc_calloc(size_t nmemb, size_t size)
{
	void *p;

	if (size != 0 && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return (NULL);
	}
	if ((p = pc_malloc(nmemb * size)) != NULL)
		memset(p, 0, nmemb * size);
	return (p);
}

/* The hook's realloc never sees NULL: that is a plain allocation. */
void *
pc_realloc(void *p, size_t n)
{
	if (p == NULL)
		return (pc_malloc(n));
	if (mem.malloc == NULL)
		return (realloc(p, n));
	return (mem.realloc(mem.ud, p, n));
}

void
pc_mfree(void *p)
{
	if (p == NULL)
		return;
	if (mem.malloc == NULL)
		free(p);
	else
		mem.free(mem.ud, p);
}

char *
pc_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = pc_malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return (d);
}

/*
 * realpath(path, NULL) copied into pc_malloc() memory; the C library's
 * copy goes back with free(). errno is realpath()'s on its failure, as
 * the callers report it.
 */
char *
pc_realpath(const char *path)
{
	char *r, *d;
	int err;

	if ((r = realpath(path, NULL)) == NULL)
		return (NULL);
	d = pc_strdup(r);
	err = errno;
	free(r);
	errno = (d == NULL) ? ENOMEM : err;
	return (d);
}

void *
pc_adopt(void *p, size_t n)
{
	void *d = NULL;

	if (p != NULL && (d = pc_malloc(n ? n : 1)) != NULL)
		memcpy(d, p, n);
	free(p);
	return (d);
}

/*
 * Aligned memory from pc_malloc(): over-allocate by align plus a pointer,
 * and keep the block's own address in the pointer just below the aligned
 * one, for pc_aligned_free(). align must be a power of two.
 */
void *
pc_aligned_alloc(size_t align, size_t n)
{
	uintptr_t a;
	void *raw;

	if (n > SIZE_MAX - align - sizeof (void *)) {
		errno = ENOMEM;
		return (NULL);
	}
	if ((raw = pc_malloc(n + align + sizeof (void *))) == NULL)
		return (NULL);
	a = ((uintptr_t)raw + sizeof (void *) + align - 1) & ~(uintptr_t)(align - 1);
	((void **)a)[-1] = raw;
	return ((void *)a);
}

void
pc_aligned_free(void *p)
{
	if (p != NULL)
		pc_mfree(((void **)p)[-1]);
}

/*
 * The bundled lz4 is built with LZ4_USER_MEMORY_FUNCTIONS (lz4/lib/lz4.c)
 * and calls these. They stay local: libpcompress.map exports neither.
 */
void *LZ4_malloc(size_t s);
void *LZ4_calloc(size_t n, size_t s);
void LZ4_free(void *p);

void *
LZ4_malloc(size_t s)
{
	return (pc_malloc(s));
}

void *
LZ4_calloc(size_t n, size_t s)
{
	return (pc_calloc(n, s));
}

void
LZ4_free(void *p)
{
	pc_mfree(p);
}

void
slab_init(void)
{
}

void
slab_cleanup(int quiet)
{
	(void) quiet;
}

void *
slab_alloc(void *p, size_t size)
{
	(void) p;
	return (pc_malloc(size));
}

void *
slab_calloc(void *p, size_t items, size_t size)
{
	(void) p;
	return (pc_calloc(items, size));
}

void
slab_free(void *p, void *address)
{
	(void) p;
	pc_mfree(address);
}

void
slab_release(void *p, void *address)
{
	(void) p;
	pc_mfree(address);
}

int
slab_cache_add(uint64_t size)
{
	(void) size;
	return (0);
}
