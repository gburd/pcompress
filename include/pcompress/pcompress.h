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
 *
 * Every path given to the library is UTF-8, on every platform, and the
 * names stored in archives are UTF-8. On Windows the library converts
 * the paths it opens itself to UTF-16; a program using the command-line
 * style API there should also make UTF-8 its code page with an
 * application manifest (activeCodePage), as pcompress.exe does, because
 * some of the C runtime calls behind it take the process code page.
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

/* --- Embedding: process-wide hooks, set before any other call --------- */

/**
 * Memory functions for the library's allocations. size is
 * sizeof (pc_allocator_t), set by the caller; all three functions are
 * required, and ud is passed to each. The library calls them from its own
 * threads, so they must be thread-safe. realloc and free are never given
 * NULL; calloc is malloc plus memset.
 */
typedef struct {
	size_t size;
	void *(*malloc)(void *ud, size_t n);
	void *(*realloc)(void *ud, void *p, size_t n);
	void (*free)(void *ud, void *p);
	void *ud;
} pc_allocator_t;

/**
 * Use a for the library's memory, for the whole process; NULL restores the
 * C library's. Call it before anything that allocates (pc_new(),
 * create_pc_context(), pc_chunker_new(), pc_index_open(), pc_manifest_*,
 * pc_keyfile_*, pc_chunk_*): once the library has allocated anything it
 * returns -1 and changes nothing, as it does for a struct whose size is
 * too small or that lacks a function. 0 on success.
 *
 * Not covered: libarchive (archive mode) and OpenSSL allocate on their own
 * (OpenSSL's CRYPTO_set_mem_functions() covers it), as do the C++ filters
 * (packJPG, packPNM, dispack, the dictionary filter), the OpenMP runtime,
 * and the internals of the system zlib and bzip2 beyond the allocation
 * hooks the library gives them, which they honour.
 */
int pc_set_allocator(const pc_allocator_t *a);

/**
 * The process's default log sink: messages from contexts without their
 * own pc_set_log() sink, and messages logged outside any context, go to
 * fn(arg, level, msg) instead of stderr. fn NULL restores stderr. level
 * is the highest level delivered to the default sink, for contexts whose
 * pc_set_log() level is -1 too; PC_LOG_NONE delivers nothing, and -1
 * keeps the current level (PC_LOG_INFO at start). Set it before other
 * threads use the library. fn may be called from library threads, and
 * from several at once for different contexts.
 */
void pc_set_default_log(pc_log_fn fn, void *arg, int level);

/**
 * Hooks around the threads the library creates (the compress and
 * decompress workers, the writer, and the archive, extract and metadata
 * threads). size is sizeof (pc_thread_hooks_t), set by the caller.
 * start(ud, name) is the first thing a new thread runs, with a short
 * name: "pc-compress", "pc-decompress", "pc-writer", "pc-meta",
 * "pc-archive" or "pc-extract". stop(ud) is the last thing it runs
 * before it exits. Either may be NULL, not both.
 */
typedef struct {
	size_t size;
	void (*start)(void *ud, const char *name);
	void (*stop)(void *ud);
	void *ud;
} pc_thread_hooks_t;

/**
 * Install h for the whole process; NULL removes the hooks. -1 (nothing
 * changed) for a struct whose size is too small or with neither function.
 * Set them before other threads use the library: a thread runs the hooks
 * that were installed when it was created. Threads that OpenMP, zstd's
 * worker pool, lzmaMt or libbsc start are not covered.
 */
int pc_set_thread_hooks(const pc_thread_hooks_t *h);

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
 * The name is copied; the caller's string need not outlive the call.
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
 * caller's buffer is not changed.
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

/* --- Chunks: building blocks for backup tools --------------------------- */

/**
 * Content-defined chunker. Boundaries depend only on the data, so the
 * same bytes give the same chunks wherever they occur and however they
 * are fed. Sizes in bytes; 0 picks a default (avg 64 KB, min avg/4, max
 * avg*4). avg is rounded down to a power of two; need 64 <= min <= avg <=
 * max <= 64 MB. NULL on bad sizes or out of memory.
 */
typedef struct pc_chunker pc_chunker_t;
pc_chunker_t *pc_chunker_new(size_t avg, size_t min, size_t max);

/**
 * pc_chunker_new() with a choice of boundary function. Each one's
 * boundaries are fixed forever (they decide which chunks deduplicate), so
 * a repository records which it uses. PC_CHUNKER_RABIN is
 * pc_chunker_new()'s: a 16-byte Rabin window; chunk sizes past min are
 * spread geometrically, averaging about min + avg. PC_CHUNKER_GEAR is
 * FastCDC's gear hash with normalized chunking: several times faster, and
 * sizes cluster around min + avg with few near max. Prefer it for new
 * repositories.
 */
#define	PC_CHUNKER_RABIN	1
#define	PC_CHUNKER_GEAR		2
pc_chunker_t *pc_chunker_new2(int algorithm, size_t avg, size_t min, size_t max);
void pc_chunker_free(pc_chunker_t *c);

/**
 * Feed len bytes. Appends the length of each chunk that ends inside them
 * to lens[] and returns how many (0 if none). Bytes after the last boundary
 * are remembered for the next call. -1 on bad arguments; -2 if lens[]
 * (nlens entries) filled up, in which case some bytes were consumed and
 * the state is undefined: size lens[] as len / min + 1.
 */
int pc_chunker_feed(pc_chunker_t *c, const void *buf, size_t len, size_t *lens, int nlens);

/** End of input: the final, shorter chunk (0 or 1 entries). Resets c. */
int pc_chunker_finish(pc_chunker_t *c, size_t *lens, int nlens);

/**
 * Chunk identity: BLAKE2b-256 of the bytes, keyed if key != NULL (keylen
 * 1-64; key NULL with keylen 0 for unkeyed). With a secret key the id does
 * not reveal the content's hash to whoever stores the objects. 0 or -1.
 */
int pc_chunk_id(const void *buf, size_t len, const void *key, size_t keylen, uint8_t id[32]);

/**
 * Raw 256-bit key for pc_chunk_seal()/pc_chunk_open(); NULL, 0 clears it.
 * With a key, objects are encrypted (XSalsa20) and authenticated (keyed
 * BLAKE2b over header and payload); without one they carry a plain
 * BLAKE2b checksum. -1 unless keylen is 32.
 */
int pc_set_key(pc_ctx_t *ctx, const void *key, size_t keylen);

/**
 * A 256-bit key for one purpose ("chunk-id", "seal", "index", ...), 1-255
 * bytes of purpose name, from a master key such as pc_keyfile_open()
 * returns. Use separate derived keys for pc_chunk_id() and pc_set_key()
 * rather than the master for both. Deterministic. 0 or -1.
 */
int pc_derive_key(const uint8_t master[32], const char *purpose, uint8_t out[32]);

/**
 * Key files: a random 256-bit key wrapped with a password of any length
 * (scrypt, then XSalsa20 and a BLAKE2b-256 tag). pc_keyfile_create()
 * writes pc_keyfile_size() bytes to out and returns the new key in key;
 * pc_keyfile_open() recovers the key, failing on a wrong password or any
 * change to the file. The KDF and its parameters are stored in the file.
 * Both 0 or -1; the key is zeroed on failure. Use the key with pc_set_key().
 */
size_t pc_keyfile_size(void);
int pc_keyfile_create(const void *pw, size_t pwlen, uint8_t key[32], void *out);
int pc_keyfile_open(const void *in, size_t len, const void *pw, size_t pwlen, uint8_t key[32]);

/**
 * Persistent set of chunk ids (an append-only file), so a backup tool can
 * skip chunks it already stored without asking its object store.
 * pc_index_open() creates the file if needed (refusing a file that is not
 * an index) and loads it; path is UTF-8. pc_index_has() and pc_index_add() return 1 /
 * 0 (present / absent, added / already there) or -1. Adds are written by
 * pc_index_sync() and pc_index_close(); add an id only after its object is
 * durably stored, so a crash can only make the tool store a chunk twice.
 * One thread at a time per index.
 */
typedef struct pc_index pc_index_t;
pc_index_t *pc_index_open(const char *path);
int pc_index_has(pc_index_t *idx, const uint8_t id[32]);
int pc_index_add(pc_index_t *idx, const uint8_t id[32]);
size_t pc_index_count(pc_index_t *idx);
int pc_index_sync(pc_index_t *idx);
int pc_index_close(pc_index_t *idx);

/**
 * An object does not say which chunk it is: whoever controls the storage
 * can put a valid object under another one's name. After pc_chunk_open(),
 * check pc_chunk_id() of the result against the name it was fetched by
 * (examples/pzstore.c does).
 */

/**
 * Snapshot manifests: the entries of one backup, or of one directory of
 * it (path, metadata and, for files, the ids and lengths of their chunks
 * in order), encoded to bytes the caller stores as an object of its own
 * (seal it to encrypt it). Paths are relative and '/'-separated; entries
 * are kept sorted by path. pc_manifest_decode() treats its input as
 * hostile: it refuses absolute paths, "." and ".." components,
 * duplicates, entries under a symlink entry or under a directory entry
 * with a subtree, chunk lengths that do not add up to the size, malformed
 * or out-of-order metadata, and trailing bytes.
 *
 * A manifest can hold a whole snapshot, or one directory: then entries
 * have one-component names and each directory entry's subtree is the id
 * of the manifest object holding its contents, so an unchanged directory
 * is the same object in every snapshot and need not be stored or read
 * again. The library does not walk subtrees; the caller does.
 *
 * pc_manifest_encode() writes format 2 (PCMF2), which 4.3 and older
 * cannot read; pc_manifest_decode() reads both 1 and 2. The same entries
 * always encode to the same bytes, so a manifest's id names its content.
 *
 * A pc_entry_t from get/find points into the manifest: valid until it is
 * freed. One thread at a time per manifest.
 */
enum {
	PC_ENTRY_FILE = 1, PC_ENTRY_DIR = 2, PC_ENTRY_SYMLINK = 3,
	PC_ENTRY_HARDLINK = 4,	/* target: path, in the snapshot, of a file */
	PC_ENTRY_CHARDEV = 5, PC_ENTRY_BLOCKDEV = 6,	/* rdev */
	PC_ENTRY_FIFO = 7
};

/** pc_entry_t.have: which optional fields hold a value. */
#define	PC_HAVE_BTIME	0x1	/* btime, btime_nsec */
#define	PC_HAVE_CTIME	0x2	/* ctime, ctime_nsec */
#define	PC_HAVE_INODE	0x4	/* dev, ino: for change detection only */
#define	PC_HAVE_SPARSE	0x8	/* files: restore runs of zeros as holes */

/** pc_entry_t.flags_kind: whose file flags flags holds. */
enum {
	PC_FLAGS_BSD = 1,	/* st_flags (chflags(2)): FreeBSD, macOS */
	PC_FLAGS_LINUX = 2,	/* FS_IOC_GETFLAGS (chattr(1)) */
	PC_FLAGS_WINDOWS = 3	/* FILE_ATTRIBUTE_* */
};

/** An extended attribute: name, 1-255 bytes, and value (may be empty). */
typedef struct {
	const char *name;
	const void *value;
	size_t len;
} pc_xattr_t;

/**
 * An access control list in its system's own text form: acl_to_text() for
 * POSIX.1e, NFSv4 and macOS ACLs, SDDL for a Windows security descriptor.
 * At most one per kind.
 */
enum {
	PC_ACL_POSIX_ACCESS = 1, PC_ACL_POSIX_DEFAULT = 2, PC_ACL_NFS4 = 3,
	PC_ACL_DARWIN = 4, PC_ACL_WINDOWS_SD = 5
};
typedef struct {
	int kind;		/* PC_ACL_* */
	const void *data;
	size_t len;		/* > 0 */
} pc_acl_t;

/*
 * NB: a hardlink's target is a checked relative path, but it may pass
 * through a symlink stored elsewhere (another directory's manifest). A
 * restore must link only to a file it restored itself in the same run.
 */
typedef struct {
	const char *path;
	int type;		/* PC_ENTRY_* */
	uint32_t mode, uid, gid;
	int64_t mtime;
	uint32_t mtime_nsec;
	uint64_t size;		/* files: sum of lens[]; else 0 */
	const char *target;	/* symlinks, hardlinks: the target; else NULL */
	size_t nchunks;
	const uint8_t *ids;	/* nchunks * 32 bytes */
	const uint32_t *lens;	/* nchunks lengths, each > 0 */
	/* Since 4.4: read through the _sz functions; 0 or NULL when absent. */
	uint32_t have;		/* PC_HAVE_* */
	uint32_t btime_nsec, ctime_nsec;
	int64_t btime, ctime;	/* birth time; status change time */
	uint64_t dev, ino;
	uint64_t rdev;		/* character and block devices: required */
	int flags_kind;		/* PC_FLAGS_*, 0 if no flags */
	uint64_t flags;
	const char *owner, *group;	/* names, 1-255 bytes, for another host */
	size_t nxattrs;
	const pc_xattr_t *xattrs;	/* any order on add; by name on get */
	size_t nacls;
	const pc_acl_t *acls;	/* any order on add; by kind on get */
	const uint8_t *subtree;	/* dirs: 32-byte id of their contents' manifest */
	/*
	 * Metadata from a newer library this one does not know, as encoded.
	 * Pass it back unchanged when carrying an entry into a new manifest.
	 */
	const void *ext;
	size_t extlen;
} pc_entry_t;
typedef struct pc_manifest pc_manifest_t;
pc_manifest_t *pc_manifest_new(void);
void pc_manifest_free(pc_manifest_t *m);
/** Copies the entry. -1 if it is invalid (see above) or out of memory. */
int pc_manifest_add(pc_manifest_t *m, const pc_entry_t *e);
size_t pc_manifest_count(pc_manifest_t *m);
/** i-th entry in path order. 0 or -1. */
int pc_manifest_get(pc_manifest_t *m, size_t i, pc_entry_t *e);
/** Index of path, filling e if not NULL; -1 if absent. */
int64_t pc_manifest_find(pc_manifest_t *m, const char *path, pc_entry_t *e);

/**
 * pc_entry_t may gain fields at its end in later versions. These take the
 * size of the caller's pc_entry_t, so a program built against an older or
 * newer header still works with this library: fields one side does not
 * know read as zero, and add refuses (-1) an entry that sets fields this
 * library does not know rather than drop them. The macros below make the
 * plain names use them; the plain functions remain for programs built
 * before they existed.
 */
int pc_manifest_add_sz(pc_manifest_t *m, const pc_entry_t *e, size_t esize);
int pc_manifest_get_sz(pc_manifest_t *m, size_t i, pc_entry_t *e, size_t esize);
int64_t pc_manifest_find_sz(pc_manifest_t *m, const char *path, pc_entry_t *e,
    size_t esize);
#define	pc_manifest_add(m, e)	pc_manifest_add_sz((m), (e), sizeof (pc_entry_t))
#define	pc_manifest_get(m, i, e)	pc_manifest_get_sz((m), (i), (e), sizeof (pc_entry_t))
#define	pc_manifest_find(m, p, e)	pc_manifest_find_sz((m), (p), (e), sizeof (pc_entry_t))
/** buf NULL: set *len to the size needed. -1 on duplicates, an entry
 *  under a symlink or a subtree, or if *len is too small. */
int pc_manifest_encode(pc_manifest_t *m, void *buf, size_t *len);
pc_manifest_t *pc_manifest_decode(const void *buf, size_t len);

/** Largest object pc_chunk_seal() can produce for len input bytes. */
size_t pc_chunk_seal_bound(size_t len);

/**
 * Compress (with the context's algorithm and level) and, if a key is set,
 * encrypt one chunk into a self-describing object at out. *outlen is the
 * buffer size on entry (at least pc_chunk_seal_bound(len)) and the object
 * size on return. Incompressible data is stored raw. 0 or -1.
 */
int pc_chunk_seal(pc_ctx_t *ctx, const void *in, size_t len, void *out, size_t *outlen);

/**
 * Verify and invert pc_chunk_seal(). The tag is checked before anything
 * is decrypted or decompressed; any change to the object fails with
 * PC_ERR_VERIFY (or PC_ERR_CORRUPT/PC_ERR_FORMAT for a bad header). *outlen
 * is the buffer size on entry, the plaintext size on return.
 */
int pc_chunk_open(pc_ctx_t *ctx, const void *in, size_t len, void *out, size_t *outlen);

/**
 * Plaintext size an object declares, from its header only (not verified),
 * to size the buffer for pc_chunk_open(); (size_t)-1 if it is not a chunk
 * object. 0 is a valid size.
 */
size_t pc_chunk_open_size(const void *obj, size_t len);

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
