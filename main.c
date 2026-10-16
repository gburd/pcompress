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
 * pcompress - Do a chunked parallel compression/decompression of a file.
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <pcompress/pcompress.h>

/* The library never installs signal handlers; the CLI does. */
static void
on_signal(int signo)
{
	(void) signo;
	pc_cleanup_tempfiles();
	_exit(1);
}

int
main(int argc, char *argv[])
{
	int err;
	pc_ctx_t *pctx;

	err = 0;
	pctx = create_pc_context();

	/*
	 * init_pc_context: 0 ok, 1 invalid arguments (already reported),
	 * 2 usage error, 3 -h, 4 -V.
	 */
	err = init_pc_context(pctx, argc, argv);
	if (err == 3 || err == 4 || (err == 2 && argc == 1)) {
		if (err != 4)
			pc_usage(pctx);
		destroy_pc_context(pctx);
		return (0);
	} else if (err == 2) {
		pc_usage(pctx);
		destroy_pc_context(pctx);
		return (2);
	} else if (err != 0) {
		fprintf(stderr, "Invalid arguments to pcompress. "
		    "Run 'pcompress -h' for usage.\n\n");
		destroy_pc_context(pctx);
		return (err);
	}

	/*
	 * Start the main routines.
	 */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	err = start_pcompress(pctx);
	destroy_pc_context(pctx);
	return (err);
}
