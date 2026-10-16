/*
 * libpcompress public API.
 *
 * This file is a part of Pcompress, a chunked parallel multi-algorithm
 * lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 */

/**
 * @file pcompress/pcompress.h
 * @brief The only header a program using libpcompress needs.
 *
 * One pc_ctx_t runs one job: compress a file or stream into the .pz
 * format, decompress one, or create or extract an archive. Contexts are
 * independent: several may run at once in different threads of one
 * process. A context is not itself thread-safe; use it from one thread
 * at a time. The library installs no signal handlers and does not exit.
 *
 * Typical use:
 *
 *	pc_ctx_t *c = pc_new();
 *	pc_set_algorithm(c, "zstd");
 *	pc_set_level(c, 6);
 *	if (pc_compress_fd(c, in_fd, out_fd) != 0)
 *		fprintf(stderr, "%s\n", pc_last_error(c));
 *	pc_free(c);
 *
 * Or with command-line style arguments, exactly as the pcompress program
 * does: create_pc_context(), init_pc_context(), start_pcompress(),
 * destroy_pc_context().
 */

#ifndef PCOMPRESS_PUBLIC_H
#define	PCOMPRESS_PUBLIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C" {
#endif

/** Version of this API. Bumped on any incompatible change. */
#define	PC_API_VERSION	1

/** Opaque job context. */
typedef struct pc_ctx pc_ctx_t;

/** Error classes from pc_last_error_code(). Values are stable. */
typedef enum {
	PC_OK = 0,
	PC_ERR_ARGS = 1,	/**< invalid option or argument */
	PC_ERR_IO = 2,		/**< open/read/write/stat failed */
	PC_ERR_NOMEM = 3,	/**< allocation failed */
	PC_ERR_FORMAT = 4,	/**< not a pcompress file, or unsupported version */
	PC_ERR_CORRUPT = 5,	/**< damaged data: bad length, header, dedupe index */
	PC_ERR_VERIFY = 6,	/**< checksum, CRC or HMAC mismatch; wrong password */
	PC_ERR_CRYPTO = 7,	/**< key setup or password problem */
	PC_ERR_INTERNAL = 8	/**< anything else */
} pc_error_t;

/** Log levels for pc_set_log(). */
enum {
	PC_LOG_NONE = -2,	/**< deliver nothing (errors are still recorded) */
	PC_LOG_ERR = 0,
	PC_LOG_WARN = 1,
	PC_LOG_INFO = 2,
	PC_LOG_VERBOSE = 3
};

/** Log callback: called from library threads, never concurrently for one context. */
typedef void (*pc_log_fn)(void *arg, int level, const char *msg);

/** Library version string, e.g. "4.1.0". */
const char *pc_version(void);

/** API version the library was built with (PC_API_VERSION). */
int pc_api_version(void);

/* --- Contexts ---------------------------------------------------------- */

/** A new context with default settings, or NULL if out of memory. */
pc_ctx_t *pc_new(void);

/** Free a context. NULL is a no-op. */
void pc_free(pc_ctx_t *ctx);

/* --- Settings (call before pc_compress_fd / pc_decompress_fd) ---------- */

/**
 * Compression algorithm: "zstd", "lz4", "lzma", "lzmaMt", "bzip2", "zlib",
 * "ppmd", "lzfx", "libbsc", "adapt", "adapt2" or "none". Default "zstd".
 * @return 0, or -1 (PC_ERR_ARGS) for an unknown name.
 */
int pc_set_algorithm(pc_ctx_t *ctx, const char *name);

/** Compression level 1 (fast) to 14 (slow). Default 6. */
int pc_set_level(pc_ctx_t *ctx, int level);

/** Chunk size in bytes (unit of parallelism). 0 picks from the level. */
int pc_set_chunk_size(pc_ctx_t *ctx, uint64_t bytes);

/** Worker threads; 0 means one per core. */
int pc_set_threads(pc_ctx_t *ctx, int n);

/**
 * Chunk checksum: "BLAKE256" (default), "BLAKE512", "SHA256", "SHA512",
 * "KECCAK256", "KECCAK512", "SKEIN256", "SKEIN512" or "CRC64".
 */
int pc_set_checksum(pc_ctx_t *ctx, const char *name);

/** Deduplication modes for pc_set_dedupe(). */
enum {
	PC_DEDUPE_NONE = 0,
	PC_DEDUPE_RABIN = 1,	/**< content-defined, within a chunk (-D) */
	PC_DEDUPE_FIXED = 2,	/**< fixed blocks, within a chunk (-F) */
	PC_DEDUPE_GLOBAL = 3	/**< across the whole file (-G); needs a seekable output */
};

/** Deduplication mode. Default PC_DEDUPE_NONE (levels >= 4 may enable it). */
int pc_set_dedupe(pc_ctx_t *ctx, int mode);

/**
 * Encrypt with "AES" or "SALSA20" (CTR mode, HMAC per chunk); NULL turns
 * encryption off. keylen is 16 or 32 bytes (0: 32).
 */
int pc_set_encryption(pc_ctx_t *ctx, const char *alg, int keylen);

/**
 * Password for encryption or decryption; every byte feeds the key
 * derivation. The library copies it and wipes the copy when done; the
 * caller's buffer is not changed. (The pcompress program's prompt and -w
 * file read at most 16 bytes.)
 */
int pc_set_password(pc_ctx_t *ctx, const void *pw, size_t len);

/* --- Running a job ------------------------------------------------------ */

/**
 * Compress everything readable from in_fd into out_fd as a .pz stream.
 * Neither descriptor is closed. Global deduplication needs a seekable
 * out_fd. @return 0 on success; otherwise see pc_last_error*().
 */
int pc_compress_fd(pc_ctx_t *ctx, int in_fd, int out_fd);

/** Decompress a .pz stream from in_fd into out_fd. Neither is closed. */
int pc_decompress_fd(pc_ctx_t *ctx, int in_fd, int out_fd);

/* --- Diagnostics -------------------------------------------------------- */

/**
 * Send this context's messages at or below level to fn instead of stderr.
 * fn == NULL restores stderr; level -1 means the process default and
 * PC_LOG_NONE silences the context. Errors are always recorded for
 * pc_last_error().
 */
void pc_set_log(pc_ctx_t *ctx, pc_log_fn fn, void *arg, int level);

/** First line of the first error in the last call on this context, or "". */
const char *pc_last_error(pc_ctx_t *ctx);

/** Class of the first error in the last call on this context. */
pc_error_t pc_last_error_code(pc_ctx_t *ctx);

/** A short English name for an error class. */
const char *pc_strerror(pc_error_t code);

/**
 * Remove the temporary files of every running job. Async-signal-safe:
 * for a program's own SIGINT/SIGTERM handler before it exits.
 */
void pc_cleanup_tempfiles(void);

/* --- Command-line style API (what the pcompress program uses) ----------- */

/** A new context. Same as pc_new(). */
pc_ctx_t *create_pc_context(void);

/** Free a context. Same as pc_free(). */
void destroy_pc_context(pc_ctx_t *ctx);

/**
 * Configure from pcompress command-line arguments (see pcompress(1)).
 * argv may be permuted. @return 0 ok, 1 invalid argument (reported),
 * 2 usage error, 3 if -h was given, 4 if -V was given.
 */
int init_pc_context(pc_ctx_t *ctx, int argc, char *argv[]);

/** Same as init_pc_context() with a space-separated string (modified). */
int init_pc_context_argstr(pc_ctx_t *ctx, char *args);

/** Run the job init_pc_context() configured. @return 0 on success. */
int start_pcompress(pc_ctx_t *ctx);

/**
 * Password for the job (not copied: the buffer must stay valid until
 * start_pcompress() returns, and the library wipes it).
 */
void pc_set_userpw(pc_ctx_t *ctx, unsigned char *pw, int len);

/** Print pcompress(1) usage to stdout. */
void pc_usage(pc_ctx_t *ctx);

#ifdef	__cplusplus
}
#endif

#endif	/* PCOMPRESS_PUBLIC_H */
