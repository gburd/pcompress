/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
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
 */

#ifndef	_PC_CACHE_H
#define	_PC_CACHE_H

#include <sys/types.h>

/* Page-cache hints for one descriptor read or written front to back. */
typedef struct {
	int fd;
	int on;		/* a regular file: hints apply */
	int out;	/* written, not read */
	off_t done;	/* pages before this were dropped */
	off_t wb;	/* output: writeback started up to here */
	off_t start;	/* output: no hints before this offset */
} pc_cache_t;

void pc_cache_open(pc_cache_t *c, int fd, int out);
void pc_cache_step(pc_cache_t *c);
void pc_cache_drop(int fd, off_t off, off_t len);

#endif
