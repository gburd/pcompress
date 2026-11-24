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
 * A static membership filter over 32-byte chunk ids: a Binary Fuse
 * filter with 8-bit fingerprints (Graf and Lemire, "Binary Fuse Filters:
 * Fast and Smaller Than Xor Filters", 2022), about 9 bits per id and a
 * false-positive rate near 0.4%. It never says "absent" for an id it was
 * built from. A backup tool builds one from the ids it already stored,
 * so a lookup of a new chunk's id (the common case) needs no table
 * probe and no read; only a "maybe" goes on to the exact table.
 *
 * The filter code is FastFilter/xor_singleheader's binaryfusefilter.h,
 * unmodified (VENDOR_PATCHES.md). Its allocations are routed to the
 * library allocator by the macros below, which take effect because
 * <stdlib.h> is already included and its include guard skips it there.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <pc_alloc.h>
#include <pcompress/pcompress.h>

#define	malloc(n)	pc_malloc(n)
#define	calloc(n, s)	pc_calloc(n, s)
#define	free(p)		pc_mfree(p)
#include "utils/binaryfusefilter.h"
#undef	malloc
#undef	calloc
#undef	free

struct pc_idfilter {
	size_t n;		/* distinct keys */
	uint64_t key;		/* the key when n == 1 */
	binary_fuse8_t f;	/* when n >= 2 */
};

/* The first 8 bytes of an id: ids are hash output, uniform already. */
static uint64_t
idf_key(const uint8_t id[32])
{
	uint64_t k;

	memcpy(&k, id, sizeof (k));
	return (k);
}

static int
idf_cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

/*
 * Build a filter from n ids (32 bytes each, back to back, in any order,
 * repeats allowed). NULL on bad arguments or out of memory. The keys are
 * sorted and made distinct here: the vendored populate needs that to
 * converge, and its own duplicate removal sorts with a comparator that
 * truncates a 64-bit difference to int.
 */
pc_idfilter_t DLL_EXPORT *
pc_idfilter_new(const void *ids, size_t n)
{
	const uint8_t *p = ids;
	pc_idfilter_t *x;
	uint64_t *k = NULL;
	size_t i, d = 0;

	if ((ids == NULL && n > 0) || n > UINT32_MAX)
		return (NULL);
	if ((x = pc_calloc(1, sizeof (*x))) == NULL)
		return (NULL);
	if (n == 0)
		return (x);
	if ((k = pc_malloc(n * sizeof (*k))) == NULL)
		goto fail;
	for (i = 0; i < n; i++)
		k[i] = idf_key(p + i * 32);
	qsort(k, n, sizeof (*k), idf_cmp);
	for (i = 0; i < n; i++)
		if (d == 0 || k[i] != k[d - 1])
			k[d++] = k[i];
	x->n = d;
	if (d == 1) {
		x->key = k[0];
	} else if (!binary_fuse8_allocate((uint32_t)d, &x->f) ||
	    !binary_fuse8_populate(k, (uint32_t)d, &x->f)) {
		goto fail;
	}
	pc_mfree(k);
	return (x);
fail:
	pc_mfree(k);
	pc_idfilter_free(x);
	return (NULL);
}

/* 1 if id may be in the set (always, for an id it was built from), 0 if not. */
int DLL_EXPORT
pc_idfilter_maybe(const pc_idfilter_t *x, const uint8_t id[32])
{
	if (x == NULL || id == NULL)
		return (-1);
	if (x->n < 2)
		return (x->n == 1 && x->key == idf_key(id));
	return (binary_fuse8_contain(idf_key(id), &x->f) ? 1 : 0);
}

size_t DLL_EXPORT
pc_idfilter_size(const pc_idfilter_t *x)
{
	if (x == NULL)
		return (0);
	return (sizeof (*x) + (x->n >= 2 ? x->f.ArrayLength : 0));
}

void DLL_EXPORT
pc_idfilter_free(pc_idfilter_t *x)
{
	if (x == NULL)
		return;
	if (x->f.Fingerprints != NULL)
		binary_fuse8_free(&x->f);
	pc_mfree(x);
}
