/*
 * pzcat: decompress a .pz stream from stdin to stdout, or compress with -c.
 * A minimal program on the public libpcompress API, built by CI against the
 * installed header and pkg-config file.
 *
 *   cc $(pkg-config --cflags libpcompress) pzcat.c -o pzcat $(pkg-config --libs libpcompress)
 *   pzcat -c < file > file.pz && pzcat < file.pz | cmp - file
 */
#include <stdio.h>
#include <string.h>
#include <pcompress/pcompress.h>

int
main(int argc, char **argv)
{
	int compress = (argc > 1 && strcmp(argv[1], "-c") == 0);
	pc_ctx_t *ctx = pc_new();
	int rv;

	if (ctx == NULL)
		return (2);
	pc_set_log(ctx, NULL, NULL, PC_LOG_NONE);	/* we print the error */
	rv = compress ? pc_compress_fd(ctx, 0, 1) : pc_decompress_fd(ctx, 0, 1);
	if (rv != 0)
		fprintf(stderr, "pzcat: %s (%s)\n", pc_last_error(ctx),
		    pc_strerror(pc_last_error_code(ctx)));
	pc_free(ctx);
	return (rv != 0);
}
