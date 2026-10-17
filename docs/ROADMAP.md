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

## What was wrong (verified on 4.0.2), and status

| # | Problem | Evidence | Status |
|---|---------|----------|--------|
| P1 | Two contexts in one process crash | Two threads, each with `create_pc_context()` + `start_pcompress()`: SIGSEGV every run, also with `--disable-allocator`. | Fixed in 4.1.0 |
| P2 | Process-global state | Slab allocator, global-dedupe index (`rabin_dedup.c: arc`), log destination, CPU/hash dispatch tables, `getopt` state, archive filter table, OpenMP thread count. `destroy_pc_context()` runs `slab_cleanup()` for the whole process. | Fixed in 4.1.0 (adaptive `-C` counters still shared) |
| P3 | The API is the command line | `init_pc_context_argstr("pcompress -c zstd …")`, then `start_pcompress()`. Inputs and outputs are file paths. The library opens files, picks names, writes a temp file and renames it. No buffer or callback I/O. | Fixed in 4.1.0 (fd I/O; no callback I/O) |
| P4 | No public headers | `make install` ships `libpcompress.so.1` and no headers. `pcompress.h` exposes `pc_ctx_t` internals. Bundled zstd, lz4, libbsc and libarchive symbols are exported and would clash with another copy in the same program. | Fixed in 4.1.0 |
| P5 | Errors are log text | Results are 0/1/2; reasons go only to stderr or a log callback. `abort()` in the allocator. | Fixed in 4.1.0 |
| P6 | Dedupe forgets everything | The global index is in memory or a temp file and is discarded at exit ("Disk based index not yet implemented"). | Fixed for chunk stores after 4.1.0 (Phase 5); `.pz` global dedupe stays per file |
| P7 | Chunks are not content-addressed | Per-chunk checksums only verify data; they are never exposed as an identity. | Fixed after 4.1.0 (Phase 3) |
| P8 | No single-file restore | An archive is one PAX stream through the compressor. Listing reads the whole file. | Open (Phase 6) |
| P9 | Key handling | Prompted and file passwords are capped at 16 bytes. No caller-supplied key, no key wrapping, no algorithm or version agility. | Fixed after 4.1.0 (Phase 4); raw keys for `.pz` wait for a new format version |
| P10 | Process side effects | SIGINT/SIGTERM handlers that call `exit()`; `PCOMPRESS_*` environment variables; output written as a temp file beside the input. | Partly: no handlers or `exit()`; `PCOMPRESS_*` env vars remain |

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

**Status: done (c8f8bb2).**
- `include/pcompress/pcompress.h`: an opaque `pc_ctx_t`, `pc_version()`
  and `PC_API_VERSION`.
- Typed setters, plus `pc_compress_fd()`/`pc_decompress_fd()`.
- Exports are limited to `libpcompress.map` (32 symbols, down from 733).
- `make install` adds the header, `libpcompress.pc` and `libpcompress(3)`.
- `main.c` includes only the public header.
- CI runs `make abi-check` (libabigail rather than
  abi-compliance-checker: it is packaged everywhere, including nixpkgs).
  The same job builds `examples/pzcat.c` against the installed copy.

Not done, deliberately:
- Callback I/O (`pc_reader_fn`/`pc_writer_fn`). Descriptors cover pipes,
  sockets and files, and a callback wrapper is a pipe plus a thread in
  the caller.
- Global dedupe and archives through the fd API. Both need a seekable
  file; they remain available through the command-line style API.

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

**Status: done (unreleased).**
- `pc_chunker_new/feed/finish/free`, `pc_chunk_id`, `pc_set_key`, and
  `pc_chunk_seal/open/open_size/seal_bound` are in place. The object
  format is described in `DESIGN.md`.
- `test/unit/test_chunk.c`:
  - open(seal(x)) == x for all 12 algorithms × levels 1/6/9 × 5 sizes
    × with and without a key × 3 kinds of data;
  - every bit-flip and truncation of a sealed object is rejected (with
    and without a key, compressed and raw);
  - wrong and missing keys are rejected;
  - chunk boundaries don't depend on how the input is split;
  - min and max sizes are respected;
  - an insertion moves only nearby boundaries;
  - the BLAKE2b-256 known answer matches.
- `test_reentrant` seals and opens chunks from 4 threads; ASan and TSan
  are clean.
- `examples/pzstore.c` is a dedupe-and-encrypt chunk store built on the
  API. A second `put` of the same file stores nothing.

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

**Status: done (unreleased).**
- Raw keys: `pc_set_key()` for chunk objects (Phase 3).
- Key files: `pc_keyfile_create/open`, with scrypt-wrapped,
  versioned KDF parameters.
- The 16-byte cap is gone from the prompt, `-w` files and the
  library.
- Old files: the legacy truncation is tried on decryption, and four
  encrypted references written by 4.1.0 are checked by `make check`.

Not done:
- A raw-key or key-file option for `.pz` streams. That would need a
  header flag the v10 format doesn't have, so it waits for the next
  format version (Phase 6).

### Phase 5: Persistent, pluggable dedupe index (fixes P6)

- Index interface: `pc_index_ops { lookup(hash) -> present?,
  insert(hash, location), … }`, supplied by the caller (Arq-style:
  "does the object store already have this?") or a built-in on-disk
  implementation.
- Global dedupe in `.pz` uses the same interface. Its in-memory and
  temp-file behaviour becomes the default implementation, so the CLI is
  unchanged.
- **Done when:** a second backup of an unchanged tree stores no new chunks.

**Status: done (unreleased), narrowed.**
- `pc_index_*`: a persistent, append-only set of chunk ids.
  `examples/pzstore` uses it, so a second `put` of unchanged data
  stores nothing and asks the store nothing. CI checks this.
- "Pluggable" turned out to be free: the chunk calls (Phase 3) never
  look anything up. A caller with its own catalogue checks it with
  `pc_chunk_id()`, and no callback interface is needed.

Not done:
- Global dedupe in `.pz` still uses its own per-file index. A
  persistent one would make a `.pz` depend on other files, a format
  change for Phase 6 if anyone wants it.

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
