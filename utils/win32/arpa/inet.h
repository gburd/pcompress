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
 * <arpa/inet.h> for Windows builds: only the byte-order conversions,
 * without Winsock (which would need ws2_32 and drags in <windows.h>).
 * Every Windows target is little-endian.
 */

#ifndef	_PC_WIN32_ARPA_INET_H
#define	_PC_WIN32_ARPA_INET_H

#include <stdint.h>

#define	htons(x)	((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define	ntohs(x)	htons(x)
#define	htonl(x)	((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define	ntohl(x)	htonl(x)

#endif
