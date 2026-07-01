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
 * @brief Slab allocator for efficient repeated buffer allocation.
 *
 * Provides a size-class cached allocator optimized for the allocation
 * pattern in Pcompress where same-sized buffers are repeatedly allocated
 * and freed. Can be bypassed by setting ALLOCATOR_BYPASS=1 in the
 * environment.
 */

#ifndef __ALLOCATOR_H__
#define __ALLOCATOR_H__

#include <sys/types.h>
#include <inttypes.h>

/** Initialize the slab allocator. Must be called before any slab_alloc. */
void slab_init();

/**
 * Clean up and release all slab allocator memory.
 * @param quiet  If non-zero, suppress statistics output.
 */
void slab_cleanup(int quiet);

/**
 * Allocate memory from the slab allocator.
 * @param p     Unused (for API compatibility with zlib-style allocators).
 * @param size  Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void *slab_alloc(void *p, size_t size);

/**
 * Allocate zeroed memory from the slab allocator.
 * @param p      Unused.
 * @param items  Number of elements.
 * @param size   Size of each element.
 * @return Pointer to zeroed allocated memory, or NULL on failure.
 */
void *slab_calloc(void *p, size_t items, size_t size);

/**
 * Return memory to the slab cache for reuse.
 * @param p        Unused.
 * @param address  Pointer previously returned by slab_alloc/slab_calloc.
 */
void slab_free(void *p, void *address);

/**
 * Release memory back to the OS (rather than caching in the slab).
 * @param p        Unused.
 * @param address  Pointer previously returned by slab_alloc/slab_calloc.
 */
void slab_release(void *p, void *address);

/**
 * Pre-create a slab cache for a given allocation size.
 * @param size  Allocation size to cache.
 * @return 0 on success, -1 on failure.
 */
int slab_cache_add(uint64_t size);

#endif

