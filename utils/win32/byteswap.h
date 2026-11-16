/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/* <byteswap.h> for Windows builds: glibc's names for the builtins. */

#ifndef	_PC_WIN32_BYTESWAP_H
#define	_PC_WIN32_BYTESWAP_H

#define	bswap_16(x)	__builtin_bswap16(x)
#define	bswap_32(x)	__builtin_bswap32(x)
#define	bswap_64(x)	__builtin_bswap64(x)
#define	__bswap_16(x)	__builtin_bswap16(x)
#define	__bswap_32(x)	__builtin_bswap32(x)
#define	__bswap_64(x)	__builtin_bswap64(x)

#endif
