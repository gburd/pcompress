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
 * Linked into every Windows program the build makes (pcompress and the
 * tests), not into the library: binary mode for every file the program
 * opens with open() or fopen(), and for its stdin, stdout, and stderr.
 * Text mode would turn "\n" into "\r\n" on write, drop '\r' and stop at
 * ^Z on read. MinGW's binmode.o does nothing with the UCRT, so this runs
 * as a constructor, before main().
 */

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>

static void __attribute__((constructor))
pc_binmode(void)
{
	(void) _set_fmode(_O_BINARY);
	(void) _setmode(0, _O_BINARY);
	(void) _setmode(1, _O_BINARY);
	(void) _setmode(2, _O_BINARY);
}
