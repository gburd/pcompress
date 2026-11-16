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
 * <sys/socket.h> for Windows builds. pcompress uses one socketpair() as a
 * bidirectional channel between two threads of the same process (see
 * meta_stream.c); utils/win32.c builds it from two anonymous pipes, so
 * read() and write() on the descriptors work as with a socket.
 */

#ifndef	_PC_WIN32_SYS_SOCKET_H
#define	_PC_WIN32_SYS_SOCKET_H

#define	AF_UNIX		1
#define	SOCK_STREAM	1

#ifdef	__cplusplus
extern "C" {
#endif

int socketpair(int domain, int type, int protocol, int sv[2]);

#ifdef	__cplusplus
}
#endif

#endif
