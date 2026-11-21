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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <utils.h>
#include "allocator.h"
#include "pc_alloc.h"

/*
 * The process's allocator: malloc == NULL means the C library's. The
 * state word guards it: MEM_UNSET while pc_set_allocator() may still
 * change it, MEM_SETTING while one call copies a struct in, MEM_USED once
 * the library has allocated (from then on it never changes, so the
 * allocation calls read it without a lock).
 */
#define	MEM_UNSET	0
#define	MEM_SETTING	1
#define	MEM_USED	2
#define	ALLOCATOR_V1_SIZE	(offsetof(pc_allocator_t, ud) + sizeof (void *))

static pc_allocator_t mem;
static int mem_state = MEM_UNSET;

/*
 * Called before every allocation: the first one moves the state to
 * MEM_USED, which makes pc_set_allocator() refuse. One that races a
 * pc_set_allocator() in progress waits out its copy (a few stores), so it
 * never reads a half-written struct.
 */
static void
mark_used(void)
{
	int st = MEM_UNSET;

	if (__atomic_load_n(&mem_state, __ATOMIC_ACQUIRE) == MEM_USED)
		return;
	while (!__atomic_compare_exchange_n(&mem_state, &st, MEM_USED, 0,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (st == MEM_USED)
			return;
		st = MEM_UNSET;	/* MEM_SETTING: retry until the copy is done */
	}
}

/*
 * Install the process's allocator (a NULL a: the C library's). Refused,
 * with nothing changed, once the library has allocated anything, while
 * another call installs one, or for a struct that is smaller than the
 * first version's or lacks one of the three functions. A larger struct
 * (a newer caller's) is accepted and its extra fields are ignored; a
 * smaller one than this build's gets the missing fields zeroed.
 */
int DLL_EXPORT
pc_set_allocator(const pc_allocator_t *a)
{
	int st = MEM_UNSET;

	if (a != NULL && (a->size < ALLOCATOR_V1_SIZE || a->malloc == NULL ||
	    a->realloc == NULL || a->free == NULL)) {
		log_msg(LOG_ERR, 0, "pc_set_allocator: size too small or a function missing");
		return (-1);
	}
	if (!__atomic_compare_exchange_n(&mem_state, &st, MEM_SETTING, 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		log_msg(LOG_ERR, 0, "pc_set_allocator: the library has already allocated memory");
		return (-1);
	}
	memset(&mem, 0, sizeof (mem));
	if (a != NULL) {
		memcpy(&mem, a, a->size < sizeof (mem) ? a->size : sizeof (mem));
		mem.size = sizeof (mem);
	}
	__atomic_store_n(&mem_state, MEM_UNSET, __ATOMIC_RELEASE);
	return (0);
}

/* NULL with errno ENOMEM on failure: log_msg() maps that to PC_ERR_NOMEM. */
void *
pc_malloc(size_t n)
{
	void *p;

	mark_used();
	if (mem.malloc == NULL)
		p = malloc(n);
	else
		p = mem.malloc(mem.ud, n);
	if (p == NULL)
		errno = ENOMEM;
	return (p);
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

/*
 * The hook's realloc never sees NULL (that is a plain allocation) nor a
 * size of 0: that frees p and returns NULL, with errno left alone. On a
 * failure p is untouched and errno is ENOMEM.
 */
void *
pc_realloc(void *p, size_t n)
{
	void *np;

	if (p == NULL)
		return (pc_malloc(n));
	if (n == 0) {
		pc_mfree(p);
		return (NULL);
	}
	if (mem.malloc == NULL)
		np = realloc(p, n);
	else
		np = mem.realloc(mem.ud, p, n);
	if (np == NULL)
		errno = ENOMEM;
	return (np);
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
