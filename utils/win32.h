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
 * The Windows platform layer (MSYS2 / MinGW-w64 builds only). ./config
 * passes "-include utils/win32.h" on every C compile there and puts
 * utils/win32 (the POSIX headers MinGW lacks: <sys/mman.h>,
 * <sys/socket.h>, <termios.h>, <arpa/inet.h>) on the include path;
 * utils/win32.c holds the functions. POSIX builds never see any of it.
 *
 * MinGW-w64 already supplies most of the POSIX that pcompress uses:
 * pthreads and semaphores (winpthreads), <unistd.h>, <dirent.h>,
 * <libgen.h>, nftw(), clock_gettime(). This file declares the rest under
 * the POSIX names, so the portable code reads as before.
 *
 * Files are always binary. The programs link utils/win32_binmode.c,
 * which makes binary the default for everything they open, stdin and
 * stdout included. The library cannot rely on that, so open() and
 * mkstemp() here open in binary mode, and the job's stdin and stdout are
 * switched to binary before use (see PC_IN_FD() in pcompress.c).
 */

#ifndef	_PC_WIN32_H
#define	_PC_WIN32_H

#ifdef _WIN32

/* rand_s(), for mkstemp(). */
#define	_CRT_RAND_S

/*
 * The system headers that declare the names redefined below come first,
 * so their declarations keep the real names.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * open() and mkstemp() in binary mode, and with FILE_SHARE_DELETE, so a
 * file can be unlinked while it is open (pcompress unlinks its temporary
 * files that way, as on POSIX). open() maps "/dev/tty" to the console.
 */
int pc_win32_open(const char *path, int flags, ...);
int pc_win32_mkstemp(char *tmpl);
#define	open(...)	pc_win32_open(__VA_ARGS__)
#define	mkstemp(t)	pc_win32_mkstemp(t)

/* A binary pipe with 64 KB buffers. */
#define	pipe(fds)	_pipe((fds), 65536, _O_BINARY)

/* One-argument mkdir(); NTFS has no POSIX permission bits. */
#define	mkdir(p, m)	_mkdir(p)

/* Flush a descriptor to disk. */
#define	fsync(fd)	_commit(fd)

#ifndef	O_NOCTTY
#define	O_NOCTTY	0
#endif
#ifndef	NAME_MAX
#define	NAME_MAX	255
#endif

/*
 * No symbolic links: stat() reports what a link points to, so lstat() is
 * stat() and nothing is ever S_IFLNK. Archiving follows links.
 */
#define	lstat(p, s)	stat((p), (s))
#ifndef	S_IFLNK
#define	S_IFLNK		0120000
#endif
#ifndef	S_ISLNK
#define	S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#endif

/*
 * Owners and POSIX modes do not map onto NTFS ACLs: the CLI's copy of
 * the input file's owner and mode to the output does nothing here.
 */
typedef int uid_t;
typedef int gid_t;
typedef unsigned char u_char;

/* Never root (0): extraction then restores no owners. */
static inline uid_t geteuid(void) { return (1); }
static inline gid_t getegid(void) { return (1); }

static inline int
fchown(int fd, uid_t uid, gid_t gid)
{
	(void) fd; (void) uid; (void) gid;
	return (0);
}

static inline int
fchmod(int fd, mode_t mode)
{
	(void) fd; (void) mode;
	return (0);
}

/*
 * realpath(): the absolute path of an existing file, with '/' separators.
 * Like POSIX it fails (ENOENT) if path does not exist. resolved, if not
 * NULL, must hold PATH_MAX bytes.
 */
char *realpath(const char *path, char *resolved);

#undef	_SC_NPROCESSORS_ONLN
#undef	_SC_PAGESIZE
#undef	_SC_PAGE_SIZE
#undef	_SC_PHYS_PAGES
#undef	_SC_AVPHYS_PAGES
#define	_SC_NPROCESSORS_ONLN	1
#define	_SC_PAGESIZE		2
#define	_SC_PAGE_SIZE		_SC_PAGESIZE
#define	_SC_PHYS_PAGES		3
#define	_SC_AVPHYS_PAGES	4
long sysconf(int name);

/* Return fd after switching it to binary mode. */
int pc_win32_binfd(int fd);

#ifdef	__cplusplus
}
#endif

#endif /* _WIN32 */

#endif
