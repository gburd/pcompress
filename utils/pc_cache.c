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

/*
 * Page-cache hints for files pcompress reads or writes once, front to
 * back. Without them a 20 GB compression fills the page cache with the
 * input and the output and evicts everything else the machine had
 * cached. Each helper is a hint: a failure changes nothing but speed,
 * so none of them reports one.
 */

/* sync_file_range() is a GNU extension of <fcntl.h>. */
#define	_GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "utils/utils.h"
#include "utils/pc_cache.h"

/*
 * Bytes handled between two hints. Large enough that the system calls
 * cost nothing next to the I/O, small enough that the job's footprint
 * in the cache stays a few windows.
 */
#define	PC_CACHE_WINDOW	(32 * 1024 * 1024)

/*
 * Tell the system the pages of [off, off + len) of fd will not be read
 * again. Clean pages are dropped at once; dirty ones are left alone.
 */
void
pc_cache_drop(int fd, off_t off, off_t len)
{
#ifdef POSIX_FADV_DONTNEED
	if (len > 0)
		(void) posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);
#else
	(void) fd; (void) off; (void) len;
#endif
}

/*
 * Start following fd: an input (out == 0) or an output (out == 1).
 * Nothing is done unless fd is a regular file (pipes and devices have
 * no pages of their own to drop). An input is also marked sequential,
 * which doubles read-ahead on Linux.
 */
void
pc_cache_open(pc_cache_t *c, int fd, int out)
{
	struct stat sb;

	c->fd = fd;
	c->out = out;
	c->on = (fd >= 0 && fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode));
	c->done = c->wb = c->on ? lseek(fd, 0, SEEK_CUR) : 0;
	if (c->done < 0)
		c->on = 0;
	/* Linux's default dirty_background_ratio: see pc_cache_step(). */
	c->start = out ? (off_t)(get_total_ram() / 10) : 0;
#ifdef POSIX_FADV_SEQUENTIAL
	if (c->on && !out)
		(void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
}

/*
 * Call after each read from, or write to, the descriptor: once its file
 * offset is a window past the last hint, drop what lies behind it.
 *
 * An input's pages are clean and go at once. An output's are dirty, and
 * dropping a dirty page does nothing, so on Linux the window just
 * written is handed to writeback (SYNC_FILE_RANGE_WRITE, which does not
 * wait for it), and the window before it, whose writeback has had a
 * window's time to finish, is dropped: the pages already written go,
 * the rest stay. Elsewhere output pages are left to the system.
 *
 * Handing pages to writeback from the writer thread also paces it to the
 * disk once the device queue is full. For an output that fits under the
 * kernel's dirty limit that made the job wait for the disk (1.5 GB to a
 * gp3 volume: 8.2 s against 0.4 s), so output hints start only after a
 * tenth of RAM has been written, where the kernel's own writeback (the
 * default dirty_background_ratio) starts and the writer is soon paced
 * anyway. A smaller output stays cached, as any file write does.
 *
 * NB: the caller serialises calls with every other writer of fd, since
 * the file offset is read with lseek().
 */
void
pc_cache_step(pc_cache_t *c)
{
	off_t cur;

	if (!c->on || (cur = lseek(c->fd, 0, SEEK_CUR)) < 0 ||
	    cur - c->wb < PC_CACHE_WINDOW || cur < c->start)
		return;
	if (!c->out) {
		pc_cache_drop(c->fd, c->done, cur - c->done);
		c->done = c->wb = cur;
		return;
	}
#ifdef SYNC_FILE_RANGE_WRITE
	(void) sync_file_range(c->fd, c->wb, cur - c->wb, SYNC_FILE_RANGE_WRITE);
	pc_cache_drop(c->fd, c->done, c->wb - c->done);
	c->done = c->wb;
	c->wb = cur;
#endif
}
