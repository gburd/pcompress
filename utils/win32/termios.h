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
 * <termios.h> for Windows builds: just enough for the password prompt to
 * turn echo off. tcgetattr() and tcsetattr() on a console descriptor get
 * and set its console mode (ENABLE_ECHO_INPUT for ECHO); on anything else
 * (a pipe, a file) they fail with ENOTTY, as on POSIX, and change nothing.
 */

#ifndef	_PC_WIN32_TERMIOS_H
#define	_PC_WIN32_TERMIOS_H

#define	ECHO		0x0008
#define	TCSANOW		0

struct termios {
	unsigned long c_lflag;	/* ECHO only */
	unsigned long pc_mode;	/* the console mode, restored as is */
};

#ifdef	__cplusplus
extern "C" {
#endif

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);

#ifdef	__cplusplus
}
#endif

#endif
