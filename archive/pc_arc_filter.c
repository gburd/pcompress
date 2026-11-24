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

/*
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <utils.h>
#include <sys/mman.h>
#include <ctype.h>
#include "pc_arc_filter.h"
#include "pc_archive.h"
#include "filters/bcj/bcj.h"

struct scratch_buffer {
	uchar_t *in_buff;
	size_t in_bufflen;
};

#ifndef _MPLV2_LICENSE_
extern size_t packjpg_filter_process(uchar_t *in_buf, size_t len, uchar_t **out_buf);
ssize_t packjpg_filter(struct filter_info *fi, void *filter_private);

extern size_t packpnm_filter_process(uchar_t *in_buf, size_t len, uchar_t **out_buf);
ssize_t packpnm_filter(struct filter_info *fi, void *filter_private);
#endif

#ifdef _ENABLE_WAVPACK_
extern size_t wavpack_filter_encode(uchar_t *in_buf, size_t len, uchar_t **out_buf,
    int cmp_level);
extern size_t wavpack_filter_decode(uchar_t *in_buf, size_t len, uchar_t **out_buf,
    ssize_t out_len);
ssize_t wavpack_filter(struct filter_info *fi, void *filter_private);
#endif

ssize_t bcj_filter(struct filter_info *fi, void *filter_private);

size_t dispack_filter_encode(uchar_t *inData, size_t len, uchar_t **out_buf);
size_t dispack_filter_decode(uchar_t *inData, size_t len, uchar_t **out_buf);
ssize_t dispack_filter(struct filter_info *fi, void *filter_private);

void
add_filters_by_type(struct type_data *typetab, struct filter_flags *ff)
{
	/* Filters use per-thread scratch buffers (ensure_buffer()); none here. */
	static struct scratch_buffer unused;
	struct scratch_buffer *sdat = &unused;
	int slot;
#ifndef _MPLV2_LICENSE_

	if (ff->enable_packjpg) {

		slot = TYPE_JPEG >> 3;
		typetab[slot].filter_private = sdat;
		typetab[slot].filter_func = packjpg_filter;
		typetab[slot].filter_name = "packJPG";
		typetab[slot].result_type = -1;

		slot = TYPE_BMP >> 3;
		typetab[slot].filter_private = sdat;
		typetab[slot].filter_func = packpnm_filter;
		typetab[slot].filter_name = "packPNM";
		typetab[slot].result_type = TYPE_BINARY;

		slot = TYPE_PNM >> 3;
		typetab[slot].filter_private = sdat;
		typetab[slot].filter_func = packpnm_filter;
		typetab[slot].filter_name = "packPNM";
		typetab[slot].result_type = TYPE_BINARY | TYPE_MEDIA_BSC;
	}
#endif

	if (ff->exe_preprocess) {
		slot = TYPE_EXE32_PE >> 3;
		typetab[slot].filter_private = sdat;
		typetab[slot].filter_func = dispack_filter;
		typetab[slot].filter_name = "Dispack";
		typetab[slot].result_type = 0;

		slot = TYPE_EXE_ARM64 >> 3;
		typetab[slot].filter_private = NULL;
		typetab[slot].filter_func = bcj_filter;
		typetab[slot].filter_name = BCJ_ARM64_NAME;
		typetab[slot].result_type = -1;

		slot = TYPE_EXE_RISCV >> 3;
		typetab[slot].filter_private = NULL;
		typetab[slot].filter_func = bcj_filter;
		typetab[slot].filter_name = BCJ_RISCV_NAME;
		typetab[slot].result_type = -1;
	}

#ifdef _ENABLE_WAVPACK_
	if (ff->enable_wavpack) {
		slot = TYPE_WAV >> 3;
		typetab[slot].filter_private = sdat;
		typetab[slot].filter_func = wavpack_filter;
		typetab[slot].filter_name = "WavPack";
		typetab[slot].result_type = -1;
	}
#endif
}

int
type_tag_from_filter_name(struct type_data *typetab, const char *fname, size_t len)
{
    size_t i;

    for (i = 0; i <= NUM_SUB_TYPES; i++)
    {
        if (typetab[i].filter_name &&
            strncmp(fname, typetab[i].filter_name, len) == 0)
        {
            return (i << 3);
        }
    }
    return (TYPE_UNKNOWN);
}
/*
 * The filter table is shared by every context in the process, so its
 * scratch_buffer cannot hold per-call data. Each thread (an archiver or
 * extractor thread runs one entry at a time) gets its own buffer instead;
 * the table's sdat argument is ignored. Filters run only in those two
 * threads, and each releases its buffer with arc_filter_thread_done()
 * before it exits. A thread-local pointer, not a pthread key: a key's
 * destructor would run after the pc_set_thread_hooks() stop hook, and
 * winpthreads crashed when the key was cleared before thread exit.
 */
static __thread struct scratch_buffer *scratch;

static struct scratch_buffer *
thread_scratch(void)
{
	if (scratch == NULL)
		scratch = pc_calloc(1, sizeof (*scratch));
	return (scratch);
}

/* Release the calling thread's scratch buffer, if it has one. */
void
arc_filter_thread_done(void)
{
	if (scratch != NULL) {
		pc_mfree(scratch->in_buff);
		pc_mfree(scratch);
		scratch = NULL;
	}
}

/* Returns this thread's scratch buffer with room for len bytes, or NULL. */
static struct scratch_buffer *
ensure_buffer(struct scratch_buffer *unused, uint64_t len)
{
	struct scratch_buffer *sdat = thread_scratch();

	(void) unused;
	if (sdat == NULL)
		return (NULL);
	if (sdat->in_buff == NULL || sdat->in_bufflen < len) {
		pc_mfree(sdat->in_buff);
		sdat->in_bufflen = len;
		sdat->in_buff = pc_malloc(sdat->in_bufflen);
		if (sdat->in_buff == NULL)
			sdat->in_bufflen = 0;
	}
	return (sdat);
}

/*
 * Copy current entry data from the archive being extracted into out_buf,
 * which holds cap bytes. The block offsets come from the archive, so a
 * block that would land past cap fails the copy (returns 0).
 */
static ssize_t
copy_archive_data(struct archive *ar, uchar_t *out_buf, uint64_t cap)
{
	int64_t offset;
	const void *buff;
	size_t size, tot;
	int r;

	tot = 0;
	for (;;) {
		r = archive_read_data_block(ar, &buff, &size, &offset);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK)
			return (0);
		/*
		 * Filtered members are written contiguously. A block anywhere
		 * else (a hostile sparse map with overlapping blocks) would
		 * leave part of the buffer unwritten while tot still adds up.
		 */
		if (offset < 0 || (uint64_t)offset != tot || size > cap - tot)
			return (0);
		memcpy(out_buf + offset, buff, size);
		tot += size;
	}
	return (tot);
}

#ifndef _MPLV2_LICENSE_
int
pjg_version_supported(char ver)
{
	return (ver >= PJG_APPVERSION1 && ver <= PJG_APPVERSION2);
}

ssize_t
packjpg_filter(struct filter_info *fi, void *filter_private)
{
	struct scratch_buffer *sdat = (struct scratch_buffer *)filter_private;
	uchar_t *mapbuf, *out;
	uint64_t len, in_size = 0, len1;

	len = archive_entry_size(fi->entry);
	len1 = len;
	if (len > PJG_FILE_SIZE_LIMIT) // Bork on massive JPEGs
		return (FILTER_RETURN_SKIP);

	if (fi->compressing) {
		mapbuf = mmap(NULL, len, PROT_READ, MAP_SHARED, fi->fd, 0);
		if (mapbuf == MAP_FAILED) {
			log_msg(LOG_ERR, 1, "Mmap failed in packJPG filter.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * We are trying to compress and this is not a proper jpeg. Skip.
		 */
		if (mapbuf[0] != 0xFF || mapbuf[1] != 0xD8) {
			munmap(mapbuf, len);
			return (FILTER_RETURN_SKIP);
		}
		if (strncmp((char *)&mapbuf[6], "Exif", 4) != 0 &&
		    strncmp((char *)&mapbuf[6], "JFIF", 4) != 0) {
			munmap(mapbuf, len);
			return (FILTER_RETURN_SKIP);
		}
	} else {
		/*
		 * Allocate input buffer and read archive data stream for the entry
		 * into this buffer.
		 */
		sdat = ensure_buffer(sdat, len);
		if (sdat == NULL || sdat->in_buff == NULL) {
			log_msg(LOG_ERR, 1, "Out of memory.");
			return (FILTER_RETURN_ERROR);
		}

		in_size = copy_archive_data(fi->source_arc, sdat->in_buff, len);
		if (in_size != len) {
			log_msg(LOG_ERR, 0, "Failed to read archive data.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * First 8 bytes in the data is the compressed size of the entry.
		 * LibArchive always zero-pads entries to their original size so
		 * we need to separately store the compressed size.
		 */
		in_size = LE64(U64_P(sdat->in_buff));
		mapbuf = sdat->in_buff + 8;

		/*
		 * We are trying to decompress and this is not a packJPG file.
		 * Write the raw data and skip. Third byte in PackJPG file is
		 * version number. We also check if it is supported.
		 */
		if (mapbuf[0] != 'J' || mapbuf[1] != 'S' || !pjg_version_supported(mapbuf[2])) {
			uint8_t *out = pc_malloc(len);

			memcpy(out, sdat->in_buff, len);
			fi->fout->output_type = FILTER_OUTPUT_MEM;
			fi->fout->out = out;
			fi->fout->out_size = len;
			return (FILTER_RETURN_SOFT_ERROR);
		}
	}

	/*
	 * Compression case.
	 */
	if (fi->compressing) {
		out = NULL;
		len = packjpg_filter_process(mapbuf, len, &out);
		if (len == 0 || len >= (len1 - 8)) {
			munmap(mapbuf, len1);
			pc_mfree(out);
			return (FILTER_RETURN_SKIP);
		}
		munmap(mapbuf, len1);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		fi->fout->hdr.in_size = LE64(len);
		return (ARCHIVE_OK);
	}

	/*
	 * Decompression case.
	 */
	out = NULL;
	if ((len = packjpg_filter_process(mapbuf, in_size, &out)) == 0) {
		/*
		 * If filter failed we indicate a soft error to continue the
		 * archive extraction.
		 */
		/*
		 * Was: memcpy of 'len', which is 0 here, so the entry was
		 * silently written empty. Emit the stored data as-is, like
		 * the not-our-format path above, and flag a soft error.
		 */
		pc_mfree(out);
		len = archive_entry_size(fi->entry);
		out = pc_malloc(len);
		if (out == NULL)
			return (FILTER_RETURN_ERROR);
		memcpy(out, sdat->in_buff, len);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		return (FILTER_RETURN_SOFT_ERROR);
	}

	fi->fout->output_type = FILTER_OUTPUT_MEM;
	fi->fout->out = out;
	fi->fout->out_size = len;
	return (ARCHIVE_OK);
}

ssize_t
packpnm_filter(struct filter_info *fi, void *filter_private)
{
	struct scratch_buffer *sdat = (struct scratch_buffer *)filter_private;
	uchar_t *mapbuf, *out;
	uint64_t len, in_size = 0, len1;

	len = archive_entry_size(fi->entry);
	len1 = len;
	if (len > PJG_FILE_SIZE_LIMIT) // Bork on massive JPEGs
		return (FILTER_RETURN_SKIP);

	if (fi->compressing) {
		mapbuf = mmap(NULL, len, PROT_READ, MAP_SHARED, fi->fd, 0);
		if (mapbuf == MAP_FAILED) {
			log_msg(LOG_ERR, 1, "Mmap failed in packPNM filter.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * We are trying to compress and this is not a proper PNM. Skip.
		 */
		if (identify_pnm_type(mapbuf, len) != 1) {
			munmap(mapbuf, len);
			return (FILTER_RETURN_SKIP);
		}
	} else {
		/*
		 * Allocate input buffer and read archive data stream for the entry
		 * into this buffer.
		 */
		sdat = ensure_buffer(sdat, len);
		if (sdat == NULL || sdat->in_buff == NULL) {
			log_msg(LOG_ERR, 1, "Out of memory.");
			return (FILTER_RETURN_ERROR);
		}

		in_size = copy_archive_data(fi->source_arc, sdat->in_buff, len);
		if (in_size != len) {
			log_msg(LOG_ERR, 0, "Failed to read archive data.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * First 8 bytes in the data is the compressed size of the entry.
		 * LibArchive always zero-pads entries to their original size so
		 * we need to separately store the compressed size.
		 */
		in_size = LE64(U64_P(sdat->in_buff));
		mapbuf = sdat->in_buff + 8;

		/*
		 * We are trying to decompress and this is not a packPNM file.
		 * Write the raw data and skip.
		 */
		if (identify_pnm_type(mapbuf, len - 8) != 2) {
			uint8_t *out = pc_malloc(len);

			memcpy(out, sdat->in_buff, len);
			fi->fout->output_type = FILTER_OUTPUT_MEM;
			fi->fout->out = out;
			fi->fout->out_size = len;
			return (FILTER_RETURN_SOFT_ERROR);
		}
	}

	/*
	 * Compression case.
	 */
	if (fi->compressing) {
		out = NULL;
		len = packpnm_filter_process(mapbuf, len, &out);
		if (len == 0 || len >= (len1 - 8)) {
			munmap(mapbuf, len1);
			pc_mfree(out);
			return (FILTER_RETURN_SKIP);
		}
		munmap(mapbuf, len1);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		fi->fout->hdr.in_size = LE64(len);
		return (ARCHIVE_OK);
	}

	/*
	 * Decompression case.
	 */
	out = NULL;
	if ((len = packpnm_filter_process(mapbuf, in_size, &out)) == 0) {
		/*
		 * If filter failed we indicate a soft error to continue the
		 * archive extraction.
		 */
		/*
		 * Was: memcpy of 'len', which is 0 here, so the entry was
		 * silently written empty. Emit the stored data as-is, like
		 * the not-our-format path above, and flag a soft error.
		 */
		pc_mfree(out);
		len = archive_entry_size(fi->entry);
		out = pc_malloc(len);
		if (out == NULL)
			return (FILTER_RETURN_ERROR);
		memcpy(out, sdat->in_buff, len);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		return (FILTER_RETURN_SOFT_ERROR);
	}

	fi->fout->output_type = FILTER_OUTPUT_MEM;
	fi->fout->out = out;
	fi->fout->out_size = len;
	return (ARCHIVE_OK);
}
#endif /* _MPLV2_LICENSE_ */

#ifdef _ENABLE_WAVPACK_
ssize_t
wavpack_filter(struct filter_info *fi, void *filter_private)
{
	struct scratch_buffer *sdat = (struct scratch_buffer *)filter_private;
	uchar_t *mapbuf, *out;
	uint64_t len, in_size = 0, len1;

	len = archive_entry_size(fi->entry);
	len1 = len;
	if (len > WVPK_FILE_SIZE_LIMIT)
		return (FILTER_RETURN_SKIP);

	if (fi->compressing) {
		mapbuf = mmap(NULL, len, PROT_READ, MAP_SHARED, fi->fd, 0);
		if (mapbuf == MAP_FAILED) {
			log_msg(LOG_ERR, 1, "Mmap failed in WavPack filter.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * We are trying to compress and this is not a proper WAV. Skip.
		 */
		if (identify_wav_type(mapbuf, len) != 1) {
			munmap(mapbuf, len);
			return (FILTER_RETURN_SKIP);
		}
	} else {
		char *wpkstr;

		/*
		 * Allocate input buffer and read archive data stream for the entry
		 * into this buffer.
		 */
		sdat = ensure_buffer(sdat, len);
		if (sdat == NULL || sdat->in_buff == NULL) {
			log_msg(LOG_ERR, 1, "Out of memory.");
			return (FILTER_RETURN_ERROR);
		}

		in_size = copy_archive_data(fi->source_arc, sdat->in_buff, len);
		if (in_size != len) {
			log_msg(LOG_ERR, 0, "Failed to read archive data.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * First 8 bytes in the data is the compressed size of the entry.
		 * LibArchive always zero-pads entries to their original size so
		 * we need to separately store the compressed size.
		 */
		in_size = LE64(U64_P(sdat->in_buff));
		mapbuf = sdat->in_buff + 8;

		/*
		 * We are trying to decompress and this is not a Wavpack file.
		 * Write the raw data and skip.
		 */
		wpkstr = (char *)mapbuf;
		if (strncmp(wpkstr, "wvpk", 4) != 0) {
			uint8_t *out = pc_malloc(len);

			memcpy(out, sdat->in_buff, len);
			fi->fout->output_type = FILTER_OUTPUT_MEM;
			fi->fout->out = out;
			fi->fout->out_size = len;
			return (FILTER_RETURN_SOFT_ERROR);
		}
	}

	/*
	 * Compression case.
	 */
	if (fi->compressing) {
		out = NULL;
		len = wavpack_filter_encode(mapbuf, len, &out, fi->cmp_level);
		if (len == 0 || len >= (len1 - 8)) {
			munmap(mapbuf, len1);
			pc_mfree(out);
			return (FILTER_RETURN_SKIP);
		}
		munmap(mapbuf, len1);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		fi->fout->hdr.in_size = LE64(len1);
		return (ARCHIVE_OK);
	}

	/*
	 * Decompression case.
	 */
	out = NULL;
	if ((len = wavpack_filter_decode(mapbuf, in_size, &out, len)) == 0) {
		/*
		 * If filter failed we indicate a soft error to continue the
		 * archive extraction.
		 */
		/*
		 * Was: memcpy of 'len', which is 0 here, so the entry was
		 * silently written empty. Emit the stored data as-is, like
		 * the not-our-format path above, and flag a soft error.
		 */
		pc_mfree(out);
		len = archive_entry_size(fi->entry);
		out = pc_malloc(len);
		if (out == NULL)
			return (FILTER_RETURN_ERROR);
		memcpy(out, sdat->in_buff, len);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		return (FILTER_RETURN_SOFT_ERROR);
	}

	fi->fout->output_type = FILTER_OUTPUT_MEM;
	fi->fout->out = out;
	fi->fout->out_size = len;
	return (ARCHIVE_OK);
}
#endif /* _ENABLE_WAVPACK_ */

ssize_t
dispack_filter(struct filter_info *fi, void *filter_private)
{
	struct scratch_buffer *sdat = (struct scratch_buffer *)filter_private;
	uchar_t *mapbuf, *out;
	uint64_t len, in_size = 0, len1;

	len = archive_entry_size(fi->entry);
	len1 = len;
	if (len > WVPK_FILE_SIZE_LIMIT) // Bork on massive files
		return (FILTER_RETURN_SKIP);

	if (fi->compressing) {
		mapbuf = mmap(NULL, len, PROT_READ, MAP_SHARED, fi->fd, 0);
		if (mapbuf == MAP_FAILED) {
			log_msg(LOG_ERR, 1, "Mmap failed in Dispack filter.");
			return (FILTER_RETURN_ERROR);
		}

		/*
		 * No check for supported 32-bit exe here. EXE types are always
		 * detected by file header analysis. So no need to duplicate here.
		 */
	} else {
		/*
		 * Allocate input buffer and read archive data stream for the entry
		 * into this buffer.
		 */
		sdat = ensure_buffer(sdat, len);
		if (sdat == NULL || sdat->in_buff == NULL) {
			log_msg(LOG_ERR, 1, "Out of memory.");
			return (FILTER_RETURN_ERROR);
		}

		in_size = copy_archive_data(fi->source_arc, sdat->in_buff, len);
		if (in_size != len) {
			log_msg(LOG_ERR, 0, "Failed to read archive data.");
			return (FILTER_RETURN_ERROR);
		}
		mapbuf = sdat->in_buff;

		/*
		 * No check for supported EXE types needed here since supported
		 * and filtered files are tagged in the archive using xattrs during
		 * compression.
		 */
	}

	/*
	 * Compression case.
	 */
	if (fi->compressing) {
		out = NULL;
		len = dispack_filter_encode(mapbuf, len, &out);
		if (len == 0) {
			munmap(mapbuf, len1);
			pc_mfree(out);
			return (FILTER_RETURN_SKIP);
		}
		munmap(mapbuf, len1);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		fi->fout->hdr_valid = 0;
		return (ARCHIVE_OK);
	}

	/*
	 * Decompression case.
	 */
	out = NULL;
	if ((len = dispack_filter_decode(mapbuf, in_size, &out)) == 0) {
		/*
		 * If filter failed we indicate a soft error to continue the
		 * archive extraction.
		 */
		/*
		 * Was: memcpy of 'len', which is 0 here, so the entry was
		 * silently written empty. Emit the stored data as-is, like
		 * the not-our-format path above, and flag a soft error.
		 */
		pc_mfree(out);
		len = archive_entry_size(fi->entry);
		out = pc_malloc(len);
		if (out == NULL)
			return (FILTER_RETURN_ERROR);
		memcpy(out, sdat->in_buff, len);

		fi->fout->output_type = FILTER_OUTPUT_MEM;
		fi->fout->out = out;
		fi->fout->out_size = len;
		return (FILTER_RETURN_SOFT_ERROR);
	}

	fi->fout->output_type = FILTER_OUTPUT_MEM;
	fi->fout->out = out;
	fi->fout->out_size = len;
	return (ARCHIVE_OK);
}

/*
 * The ARM64 and RISC-V branch filter (filters/bcj/): the member is
 * converted in place, so its size does not change and no header is
 * needed. Only members of at most BCJ_FILE_SIZE_LIMIT bytes are
 * filtered, one copy in memory each way. Every byte string decodes, so
 * extraction cannot fail here beyond reading the member or allocating.
 */
ssize_t
bcj_filter(struct filter_info *fi, void *filter_private)
{
	uint64_t len = archive_entry_size(fi->entry);
	int arm64 = (*fi->type_ptr & PC_SUBTYPE_MASK) == TYPE_EXE_ARM64;
	uchar_t *out, *mapbuf;

	(void) filter_private;
	if (fi->compressing && (len < BCJ_FILE_SIZE_MIN || len > BCJ_FILE_SIZE_LIMIT))
		return (FILTER_RETURN_SKIP);
	/* Extracting: no filtered member is larger; the size is untrusted. */
	if (len > BCJ_FILE_SIZE_LIMIT) {
		log_msg(LOG_ERR, 0, "BCJ member size %" PRIu64 " too large.", len);
		return (FILTER_RETURN_ERROR);
	}
	if ((out = pc_malloc(len > 0 ? len : 1)) == NULL) {
		log_msg(LOG_ERR, 0, "Out of memory.");
		return (FILTER_RETURN_ERROR);
	}

	if (fi->compressing) {
		mapbuf = mmap(NULL, len, PROT_READ, MAP_SHARED, fi->fd, 0);
		if (mapbuf == MAP_FAILED) {
			log_msg(LOG_ERR, 1, "Mmap failed in BCJ filter.");
			pc_mfree(out);
			return (FILTER_RETURN_ERROR);
		}
		memcpy(out, mapbuf, len);
		munmap(mapbuf, len);
		if (arm64)
			bcj_arm64_encode(out, len);
		else
			bcj_riscv_encode(out, len);
		fi->fout->hdr_valid = 0;
	} else {
		if (copy_archive_data(fi->source_arc, out, len) != (ssize_t)len) {
			log_msg(LOG_ERR, 0, "Failed to read archive data.");
			pc_mfree(out);
			return (FILTER_RETURN_ERROR);
		}
		if (arm64)
			bcj_arm64_decode(out, len);
		else
			bcj_riscv_decode(out, len);
	}
	fi->fout->output_type = FILTER_OUTPUT_MEM;
	fi->fout->out = out;
	fi->fout->out_size = len;
	return (ARCHIVE_OK);
}
