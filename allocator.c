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
 * The slab_* names remain so call sites did not change.
 */

#include <stdlib.h>
#include <inttypes.h>
#include "allocator.h"

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
	return (malloc(size));
}

void *
slab_calloc(void *p, size_t items, size_t size)
{
	(void) p;
	return (calloc(items, size));
}

void
slab_free(void *p, void *address)
{
	(void) p;
	free(address);
}

void
slab_release(void *p, void *address)
{
	(void) p;
	free(address);
}

int
slab_cache_add(uint64_t size)
{
	(void) size;
	return (0);
}
