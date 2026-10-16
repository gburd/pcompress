# Roadmap: libpcompress as a library

Goal: a library that serves two users without breaking either.

1. **The `pcompress` command line as it exists today.** Same options,
   same `.pz` file format (version 10), and same exit statuses. Every step
   below keeps `make check`, the feature matrix and the reference
   archives in `test/backward_compat` passing.
2. **A program built on the library**, such as a replica of
   [Arq](https://www.arqbackup.com/documentation/arq7/English.lproj/):
   content-defined chunks, named by hash, compressed and encrypted,
   deduplicated across backups, stored as objects in a backend the
   program chooses, with single-file restore.

The library supplies the data-path building blocks. Snapshots, retention,
scheduling and storage backends belong in the consuming program.

## What is wrong today (verified, 4.0.2)

| # | Problem | Evidence |
|---|---------|----------|
| P1 | Two contexts in one process crash | Two threads, each with `create_pc_context()` + `start_pcompress()`: SIGSEGV every run, also with `--disable-allocator`. |
| P2 | Process-global state | Slab allocator, global-dedupe index (`rabin_dedup.c: arc`), log destination, CPU/hash dispatch tables, `getopt` state, archive filter table, OpenMP thread count. `destroy_pc_context()` runs `slab_cleanup()` for the whole process. |
| P3 | The API is the command line | `init_pc_context_argstr("pcompress -c zstd …")`, then `start_pcompress()`. Inputs and outputs are file paths. The library opens files, picks names, writes a temp file and renames it. No buffer or callback I/O. |
| P4 | No public headers | `make install` ships `libpcompress.so.1` and no headers. `pcompress.h` exposes `pc_ctx_t` internals. Bundled zstd, lz4, libbsc and libarchive symbols are exported and would clash with another copy in the same program. |
| P5 | Errors are log text | Results are 0/1/2; reasons go only to stderr or a log callback. `abort()` in the allocator. |
| P6 | Dedupe forgets everything | The global index is in memory or a temp file and is discarded at exit ("Disk based index not yet implemented"). |
| P7 | Chunks are not content-addressed | Per-chunk checksums only verify data; they are never exposed as an identity. |
| P8 | No single-file restore | An archive is one PAX stream through the compressor. Listing reads the whole file. |
| P9 | Key handling | Prompted and file passwords are capped at 16 bytes. No caller-supplied key, no key wrapping, no algorithm or version agility. |
| P10 | Process side effects | SIGINT/SIGTERM handlers that call `exit()`; `PCOMPRESS_*` environment variables; output written as a temp file beside the input. |

## Phases

Each phase lands on its own with tests, keeps the CLI's behaviour identical,
and does not change the `.pz` format unless the phase says so.

### Phase 1: Reentrant core (fixes P1, P2, P5, P10)

- Move every process global into either an explicit one-time
  `pc_global_init()`, which is idempotent and thread-safe (CPU detection,
  hash dispatch, filter registry), or into the context (allocator state,
  log sink, dedupe index, option parsing).
- Slab allocator: one per context, or remove it. Benchmarks showed
  `--disable-allocator` within noise.
- Replace `getopt` in the library with a reentrant parser, or parse in
  `main.c` and fill a settings struct.
- Signal handlers and `exit()` only in `main.c`.
- `pc_error_t` codes, plus `pc_strerror()` and a per-context last-error message.
- **Done when:** N threads × M sequential jobs, mixing compress,
  decompress, encrypt and global dedupe, pass under TSan and ASan.

**Status: done (03cee65).** Several contexts run safely in one process.
`test/unit/test_reentrant.c` (in `make check`) runs sequential reuse and
then 4 threads × 8 rounds over zstd, lz4, lzma, adapt2, `-D`, `-G`, AES
and SALSA20, plus an archive-and-extract per round. It is clean under ASan
and under TSan with libarcher, as are `test_api`, `test_errors.sh` and
`test_archive.sh`.

- Process-wide state is either set up once (`pthread_once`: CPU probe,
  hash and AES dispatch, Rabin tables) or per context: dedupe index,
  archive filter table, log sink and level, last error, option parsing.
- The slab allocator is gone (it was process-global and no faster).
- Logging: `pc_set_log()`. Errors: `pc_last_error()`,
  `pc_last_error_code()` → `pc_error_t`, `pc_strerror()`.
- The library installs no signal handlers; `main.c` does, calling
  `pc_cleanup_tempfiles()`.
- Extraction no longer `chdir()`s.

Bugs found on the way (each with a test):
- A corrupt file could exit 0 with truncated output (about 1 in 200 runs).
- `-j` and `-x` were ignored.
- `-i` printed nothing for archives with a metadata stream.
- A crafted archive could write outside the target directory.

Left for later, recorded as `shortcut:` in the code:
- Adaptive `-C` counters are process-wide.
- `PCOMPRESS_*` environment variables are still read.
- packJPG and packPNM calls are serialised.

### Phase 2: Public API and packaging (fixes P3, P4)

- Installed header `<pcompress/pcompress.h>` with an opaque
  `pc_ctx_t` and a versioned, documented ABI (`pc_version()`,
  `PC_API_VERSION`).
- Settings through setters (`pc_set_algorithm()`, `pc_set_level()`,
  `pc_set_chunk_size()`, …), not argv strings. The argv parser remains
  as a convenience used by the CLI.
- Stream I/O through callbacks: `pc_reader_fn` and `pc_writer_fn`,
  with file-descriptor and memory helpers. Compress and decompress take
  streams; file paths are a wrapper.
- Hide the symbols of the bundled libraries (a version script or
  `-fvisibility` plus symbol prefixing). Ship a `pkg-config` file.
- The CLI is rebuilt on this API only.
- **Done when:** the CLI links only against the public header;
  `abi-compliance-checker` runs in CI.

### Phase 3: Chunk-level primitives (fixes P7)

- Content-defined chunker as a streaming API:
  `pc_chunker_feed()` yields chunk boundaries plus a strong hash
  (BLAKE2b-256 or SHA-512/256), with configurable average and min/max
  sizes. This exposes the existing Rabin code.
- `pc_chunk_seal()`: compress (adaptive or fixed) and then encrypt plus
  MAC one chunk into a self-describing object (version, algorithm,
  nonce, MAC). `pc_chunk_open()` verifies and inverts it. Buffers in,
  buffers out.
- Keyed chunk identity (HMAC of the plaintext hash), so the object store
  does not reveal content hashes to the storage provider.
- **Done when:** property tests show open(seal(x)) == x for every
  algorithm, level and key, and every corrupt object is rejected.

### Phase 4: Keys (fixes P9)

- Caller-supplied raw keys (`pc_set_key()`). The library no longer
  needs a password.
- A key-file format: a random master key, wrapped by an
  scrypt-or-Argon2-derived key from a password of any length, with
  versioning so the KDF and its parameters can change.
- Remove the 16-byte password cap on every path. The CLI keeps its
  current derivation for existing `.pz` files.
- **Done when:** existing encrypted `.pz` files still decrypt, and new
  key files round-trip and reject a wrong password.

### Phase 5: Persistent, pluggable dedupe index (fixes P6)

- Index interface: `pc_index_ops { lookup(hash) -> present?,
  insert(hash, location), … }`, supplied by the caller (Arq-style:
  "does the object store already have this?") or a built-in on-disk
  implementation.
- Global dedupe in `.pz` uses the same interface. Its in-memory and
  temp-file behaviour becomes the default implementation, so the CLI is
  unchanged.
- **Done when:** a second backup of an unchanged tree stores no new chunks.

### Phase 6: Indexed containers and single-file restore (fixes P8)

- A seekable container variant: chunk table and file index at the end,
  so a reader can find and extract one file without reading everything.
  This is a new format version, alongside v10, which is still read and
  written.
- A tree/manifest record format (path, metadata, ordered chunk IDs) the
  consuming program can store as an object of its own.
- **Done when:** extracting one file from a large archive reads
  O(file size) bytes; v10 archives are unchanged.

### Phase 7: Example consumer

- `examples/backup/`: a minimal Arq-like tool on the public API only.
  - Chunk, seal and store objects in a local directory.
  - Manifests per snapshot.
  - Incremental backup using the index callback.
  - Restore a snapshot or one file.
  - Prune unreferenced objects.
- This is a test of the API, not a product. S3/B2 backends and
  scheduling stay out of scope.

## Non-goals

- Storage backends, scheduling, retention policy, GUI: they belong to the
  consuming program.
- Changing the default `.pz` format or the CLI's options.

## Working environment

Development for this roadmap happens on a Debian 13 host in EC2
(AWS account "kiwi", us-east-2), tagged `Project=pcompress-dev`, and is
torn down after each work session.
