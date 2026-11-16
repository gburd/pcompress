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
 * The host byte order, from the compiler alone. WORDS_BIGENDIAN is defined
 * on big-endian hosts and left undefined on little-endian ones.
 *
 * GCC (4.6 and later) and clang predefine __BYTE_ORDER__, so no system
 * header is needed. That matters: <endian.h> and <arpa/nameser_compat.h>
 * do not exist everywhere (Windows), and glibc's <endian.h> leaves
 * BYTE_ORDER undefined when a file asks for strict X/Open, which made
 * "BYTE_ORDER == BIG_ENDIAN" compare 0 with 0 and pass on little-endian
 * hosts. MSVC defines no byte-order macro; every Windows target is
 * little-endian.
 */

#ifndef	_PC_BYTEORDER_H
#define	_PC_BYTEORDER_H

#undef	WORDS_BIGENDIAN
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#	if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#		define	WORDS_BIGENDIAN	1
#	endif
#elif !defined(_WIN32)
#	error "Cannot tell the byte order: the compiler does not define __BYTE_ORDER__."
#endif

#endif
