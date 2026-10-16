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
 *
 */

/**
 * @file allocator.h
 * @brief Allocation wrappers (malloc/calloc/free).
 *
 * The slab_* names are kept from the old slab allocator, removed in 4.1;
 * they now call the system allocator. The zlib-style first argument is
 * ignored.
 */

#ifndef __ALLOCATOR_H__
#define __ALLOCATOR_H__

#include <sys/types.h>
#include <inttypes.h>

/** No-op, kept for source compatibility. */
void slab_init(void);

/** No-op, kept for source compatibility. */
void slab_cleanup(int quiet);

/**
 * malloc().
 * @param p     Unused (for API compatibility with zlib-style allocators).
 * @param size  Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void *slab_alloc(void *p, size_t size);

/**
 * calloc().
 * @param p      Unused.
 * @param items  Number of elements.
 * @param size   Size of each element.
 * @return Pointer to zeroed allocated memory, or NULL on failure.
 */
void *slab_calloc(void *p, size_t items, size_t size);

/**
 * free().
 * @param p        Unused.
 * @param address  Pointer previously returned by slab_alloc/slab_calloc.
 */
void slab_free(void *p, void *address);

/**
 * free().
 * @param p        Unused.
 * @param address  Pointer previously returned by slab_alloc/slab_calloc.
 */
void slab_release(void *p, void *address);

/** No-op, kept for source compatibility. Returns 0. */
int slab_cache_add(uint64_t size);

#endif

