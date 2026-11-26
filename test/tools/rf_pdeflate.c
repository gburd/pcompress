/*
 * Raw deflate with the reflate filter's pinned zlib (rf_deflate()), for
 * test/integration/test_reflate.sh: the containers it builds must hold
 * streams that -Z can reproduce on every platform, and the system zlib
 * may be another deflater (zlib-ng on Fedora and Debian riscv64).
 *
 *   rf_pdeflate <level> <memlevel> <wbits> <strategy> < raw > deflated
 *
 * Writes the raw (headerless) deflate stream of stdin to stdout. Exit
 * status 0, or 1 on bad arguments, a read error, or a deflate failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pc_alloc.h>
#include "filters/reflate/rf_deflate.h"

int
main(int argc, char **argv)
{
	rf_params_t p;
	unsigned char *raw = NULL, *out;
	size_t n = 0, cap = 0, olen, r;

	if (argc != 5)
		return (1);
	p.level = atoi(argv[1]);
	p.memlevel = atoi(argv[2]);
	p.wbits = atoi(argv[3]);
	p.strategy = atoi(argv[4]);
	/* Read all of stdin, doubling the buffer as needed. */
	for (;;) {
		if (n == cap) {
			unsigned char *nb;

			cap = cap ? cap * 2 : 1 << 16;
			if ((nb = pc_realloc(raw, cap)) == NULL)
				return (1);
			raw = nb;
		}
		if ((r = fread(raw + n, 1, cap - n, stdin)) == 0)
			break;
		n += r;
	}
	if (ferror(stdin))
		return (1);
	/* A stored deflate never grows beyond this; compressed ones less. */
	cap = n + n / 8 + 1024;
	if ((out = pc_malloc(cap)) == NULL || rf_deflate(raw, n, out, cap, &p, &olen) != 0)
		return (1);
	if (fwrite(out, 1, olen, stdout) != olen || fflush(stdout) != 0)
		return (1);
	pc_mfree(raw);
	pc_mfree(out);
	return (0);
}
