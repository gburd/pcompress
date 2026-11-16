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
 * The Windows platform layer: what utils/win32.h and the headers in
 * utils/win32/ declare. Compiled only in MinGW-w64 builds (./config adds
 * it to the sources there).
 */

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <termios.h>

/* This file defines the functions the macros in win32.h name. */
#undef	open
#undef	mkstemp

/*
 * Set errno from a Win32 error code, for the few that the CRT's own
 * calls would report differently.
 */
static void
set_errno_win32(DWORD err)
{
	switch (err) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
		errno = ENOENT;
		break;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		errno = EEXIST;
		break;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		errno = EACCES;
		break;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_COMMITMENT_LIMIT:
		errno = ENOMEM;
		break;
	default:
		errno = EINVAL;
		break;
	}
}

/*
 * Open path with CreateFile() and wrap the handle as a CRT descriptor.
 * The CRT's own open() denies delete sharing, so a file could not be
 * unlinked while open; pcompress does that with its temporary files, as
 * POSIX allows. Here every handle shares read, write, and delete, and
 * the descriptor is binary and not inherited by child processes.
 * "/dev/tty" opens the console input (CONIN$), in text mode so a line
 * ends in '\n' alone, for the password prompt.
 */
static int
open_shared(const char *path, int flags)
{
	DWORD access, disp;
	HANDLE h;
	int fd, crtflags = _O_BINARY;

	switch (flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
	case O_WRONLY:
		access = GENERIC_WRITE;
		break;
	case O_RDWR:
		access = GENERIC_READ | GENERIC_WRITE;
		break;
	default:
		access = GENERIC_READ;
		crtflags |= _O_RDONLY;
		break;
	}
	if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
		disp = CREATE_NEW;
	else if ((flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC))
		disp = CREATE_ALWAYS;
	else if (flags & O_CREAT)
		disp = OPEN_ALWAYS;
	else if (flags & O_TRUNC)
		disp = TRUNCATE_EXISTING;
	else
		disp = OPEN_EXISTING;
	if (strcmp(path, "/dev/tty") == 0) {
		path = "CONIN$";
		access = GENERIC_READ | GENERIC_WRITE;
		disp = OPEN_EXISTING;
		crtflags = _O_TEXT;
	}
	if (flags & O_APPEND)
		crtflags |= _O_APPEND;

	h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		set_errno_win32(GetLastError());
		return (-1);
	}
	if ((fd = _open_osfhandle((intptr_t)h, crtflags)) == -1) {
		CloseHandle(h);
		errno = EMFILE;
		return (-1);
	}
	return (fd);
}

/* open(path, flags[, mode]): the mode has no meaning for NTFS. */
int
pc_win32_open(const char *path, int flags, ...)
{
	return (open_shared(path, flags));
}

/*
 * mkstemp(): replace the trailing XXXXXX of tmpl and create the file,
 * read-write, failing if it exists, as open_shared() opens files.
 */
int
pc_win32_mkstemp(char *tmpl)
{
	static const char letters[] =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(tmpl), i;
	unsigned int r;
	int tries, fd;

	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return (-1);
	}
	for (tries = 0; tries < 1000; tries++) {
		for (i = len - 6; i < len; i++) {
			if (rand_s(&r) != 0) {
				errno = EIO;
				return (-1);
			}
			tmpl[i] = letters[r % 62];
		}
		fd = open_shared(tmpl, O_RDWR | O_CREAT | O_EXCL);
		if (fd != -1 || errno != EEXIST)
			return (fd);
	}
	return (-1);
}

int
pc_win32_binfd(int fd)
{
	if (fd >= 0)
		(void) _setmode(fd, _O_BINARY);
	return (fd);
}

/*
 * mmap() for the two kinds of mapping pcompress makes: a read-only view
 * of part of a file (MAP_SHARED, PROT_READ), and anonymous private
 * read-write memory (MAP_ANON | MAP_PRIVATE, fd -1). off must be a
 * multiple of the allocation granularity (64 KB), which is what
 * sysconf(_SC_PAGESIZE) reports here, so the callers' page rounding
 * yields valid offsets. Anything else fails with EINVAL.
 */
void *
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	HANDLE fh, mh;
	void *p;
	uint64_t end;

	(void) addr;
	if (len == 0) {
		errno = EINVAL;
		return (MAP_FAILED);
	}
	if (flags & MAP_ANON) {
		p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p == NULL) {
			set_errno_win32(GetLastError());
			return (MAP_FAILED);
		}
		return (p);
	}
	if (prot != PROT_READ || off < 0) {
		errno = EINVAL;
		return (MAP_FAILED);
	}
	fh = (HANDLE)_get_osfhandle(fd);
	if (fh == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return (MAP_FAILED);
	}

	/*
	 * POSIX maps a range that runs past the end of the file (the tail
	 * reads as zeros); Windows refuses a view beyond the file's size,
	 * and a mapping of an empty file. Map no further than the end.
	 */
	end = (uint64_t)off + len;
	{
		LARGE_INTEGER sz;

		if (!GetFileSizeEx(fh, &sz)) {
			set_errno_win32(GetLastError());
			return (MAP_FAILED);
		}
		if ((uint64_t)sz.QuadPart <= (uint64_t)off) {
			errno = ENXIO;
			return (MAP_FAILED);
		}
		if (end > (uint64_t)sz.QuadPart) {
			end = (uint64_t)sz.QuadPart;
			len = (size_t)(end - (uint64_t)off);
		}
	}
	mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
	if (mh == NULL) {
		set_errno_win32(GetLastError());
		return (MAP_FAILED);
	}
	p = MapViewOfFile(mh, FILE_MAP_READ, (DWORD)((uint64_t)off >> 32),
	    (DWORD)((uint64_t)off & 0xffffffffU), len);
	/* The view keeps the mapping object alive. */
	CloseHandle(mh);
	if (p == NULL) {
		set_errno_win32(GetLastError());
		return (MAP_FAILED);
	}
	return (p);
}

/*
 * munmap() for either kind: VirtualQuery() tells a file view (MEM_MAPPED)
 * from VirtualAlloc() memory (MEM_PRIVATE).
 */
int
munmap(void *addr, size_t len)
{
	MEMORY_BASIC_INFORMATION mi;

	(void) len;
	if (VirtualQuery(addr, &mi, sizeof (mi)) == 0) {
		errno = EINVAL;
		return (-1);
	}
	if (mi.Type == MEM_MAPPED) {
		if (!UnmapViewOfFile(addr)) {
			errno = EINVAL;
			return (-1);
		}
	} else if (!VirtualFree(addr, 0, MEM_RELEASE)) {
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

/*
 * socketpair() between two threads of this process: a local duplex named
 * pipe, the server end in sv[0] and the client end in sv[1], both binary
 * CRT descriptors. read() and write() work on either end, and closing one
 * end gives the other EOF, which is all meta_stream.c needs. Each end is
 * used by one thread at a time there (a message, then its ack).
 */
int
socketpair(int domain, int type, int protocol, int sv[2])
{
	static volatile LONG serial;
	char name[96];
	HANDLE srv, cli;
	int fd0, fd1;

	(void) domain; (void) type; (void) protocol;
	snprintf(name, sizeof (name), "\\\\.\\pipe\\pcompress-%lu-%ld",
	    (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&serial));
	srv = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
	    1, 65536, 65536, 0, NULL);
	if (srv == INVALID_HANDLE_VALUE) {
		set_errno_win32(GetLastError());
		return (-1);
	}
	cli = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (cli == INVALID_HANDLE_VALUE) {
		set_errno_win32(GetLastError());
		CloseHandle(srv);
		return (-1);
	}
	/* The client is connected once CreateFile() returns. */
	if (!ConnectNamedPipe(srv, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
		set_errno_win32(GetLastError());
		CloseHandle(srv);
		CloseHandle(cli);
		return (-1);
	}
	fd0 = _open_osfhandle((intptr_t)srv, _O_BINARY);
	if (fd0 == -1) {
		CloseHandle(srv);
		CloseHandle(cli);
		return (-1);
	}
	fd1 = _open_osfhandle((intptr_t)cli, _O_BINARY);
	if (fd1 == -1) {
		_close(fd0);
		CloseHandle(cli);
		return (-1);
	}
	sv[0] = fd0;
	sv[1] = fd1;
	return (0);
}

/*
 * realpath(): GetFullPathName() gives the absolute path; the file must
 * exist, as POSIX requires. Separators become '/', which Windows accepts
 * and pcompress's path handling (PATHSEP_CHAR) expects.
 */
char *
realpath(const char *path, char *resolved)
{
	char buf[PATH_MAX], *p;
	DWORD n;
	struct _stat64 st;

	if (path == NULL || *path == '\0') {
		errno = path == NULL ? EINVAL : ENOENT;
		return (NULL);
	}
	n = GetFullPathNameA(path, sizeof (buf), buf, NULL);
	if (n == 0 || n >= sizeof (buf)) {
		errno = n == 0 ? ENOENT : ENAMETOOLONG;
		return (NULL);
	}
	if (_stat64(buf, &st) == -1)
		return (NULL);
	for (p = buf; *p != '\0'; p++)
		if (*p == '\\')
			*p = '/';
	if (resolved == NULL)
		return (_strdup(buf));
	memcpy(resolved, buf, n + 1);
	return (resolved);
}

/*
 * sysconf() for the names pcompress asks about. The "page size" is the
 * allocation granularity (64 KB): mmap() offsets must be multiples of it,
 * and the callers use the page size only to round mapping offsets.
 */
long
sysconf(int name)
{
	SYSTEM_INFO si;
	MEMORYSTATUSEX ms;

	switch (name) {
	case _SC_NPROCESSORS_ONLN:
		GetSystemInfo(&si);
		return ((long)si.dwNumberOfProcessors);
	case _SC_PAGESIZE:
		GetSystemInfo(&si);
		return ((long)si.dwAllocationGranularity);
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		GetSystemInfo(&si);
		ms.dwLength = sizeof (ms);
		if (!GlobalMemoryStatusEx(&ms))
			return (-1);
		return ((long)((name == _SC_PHYS_PAGES ? ms.ullTotalPhys : ms.ullAvailPhys) /
		    si.dwAllocationGranularity));
	default:
		errno = EINVAL;
		return (-1);
	}
}

/*
 * tcgetattr()/tcsetattr() on a console input descriptor: ECHO maps to
 * ENABLE_ECHO_INPUT, and everything else in the console mode is kept.
 * Not a console: ENOTTY, as on POSIX.
 */
int
tcgetattr(int fd, struct termios *t)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	DWORD mode;

	if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
		errno = ENOTTY;
		return (-1);
	}
	t->pc_mode = mode;
	t->c_lflag = (mode & ENABLE_ECHO_INPUT) ? ECHO : 0;
	return (0);
}

int
tcsetattr(int fd, int action, const struct termios *t)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	DWORD mode = (DWORD)t->pc_mode;

	(void) action;
	if (t->c_lflag & ECHO)
		mode |= ENABLE_ECHO_INPUT;
	else
		mode &= ~(DWORD)ENABLE_ECHO_INPUT;
	if (h == INVALID_HANDLE_VALUE || !SetConsoleMode(h, mode)) {
		errno = ENOTTY;
		return (-1);
	}
	return (0);
}
