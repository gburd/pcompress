# Pcompress design

This document describes how pcompress works internally: the stream
format, the thread pipeline and its synchronisation protocol, and the
invariants that the code relies on. `docs/ARCHITECTURE.md` gives the
component tour; `compressed_file_format.txt` has the bit-level header
diagrams. Read this before changing `pcompress.c`, `rabin/` or
`archive/`.

## Goals

- Use every core: the input is cut into independent chunks that are
  processed in parallel, while output stays strictly in input order.
- Stream: compress from a pipe and decompress to a pipe without knowing
  the input size, except where a feature makes that impossible (global
  dedupe restore, metadata-stream archives).
- Verify everything: each chunk carries a digest of its original bytes
  (or an HMAC when encrypted) and the header carries a CRC32/HMAC.
  Decompression never writes output it has not verified.
- Be adaptive: pick a compressor per chunk from data analysis, and run
  content-specific filters (JPEG, PNM, x86 code, numeric tables, text).

## Stream format

```
file header   algo[8] version[2] flags[2] chunksize[8] level[4]
              [salt, nonce, keylen if encrypted]  CRC32 or HMAC
chunk*        len_cmp[8] digest[n] mac[m] flags[1] payload [orig_size[8]]
trailer       0[8]
```

Header fields and `len_cmp` are big-endian; dedupe indexes and the
metadata-chunk length are little-endian. `len_cmp == 0` is end of stream.

**Metadata chunks.** In archive mode the libarchive headers (names,
modes, xattrs) are written to a separate *metadata stream* that is
interleaved with data chunks (`meta_stream.c`). A metadata chunk is
marked by `len_cmp == METADATA_INDICATOR (1)` followed by its real length.
That value is ambiguous: a one-byte data chunk also has `len_cmp == 1`.
The reader therefore only treats `1` as a metadata marker when the file
header has `FLAG_META_STREAM`. Do not add another sentinel in the length
field; extend the flags instead.

**Versioning.** `VERSION` in `pcompress.h` is the format version (10).
The reader accepts `VERSION-4` and newer; older-version quirks are
handled inline (look for `version <`). Bump it only when the format
changes, never for a release.

## Pipeline

```
                 +-> worker 0 -+
reader thread ---+-> worker 1 -+--> writer thread
(main)           +-> worker n -+
```

- **Compression**: the main thread reads chunk *i* into worker
  `i mod n`. Workers dedupe, filter, compress, checksum and encrypt. The
  writer emits chunks round-robin in the same order.
- **Decompression**: the main thread parses chunk headers and hands
  payloads to workers round-robin; workers verify and decompress; the
  writer emits them in order (or feeds the libarchive extractor thread
  in archive mode).

Each worker owns three semaphores:

| semaphore        | posted by | means                             |
|------------------|-----------|-----------------------------------|
| `start_sem`      | reader    | input buffer is ready             |
| `cmp_done_sem`   | worker    | output buffer is ready            |
| `write_done_sem` | writer    | buffers may be reused             |

`len_cmp == 0` in a worker's output tells the writer to stop. Workers and
the writer check `main_cancel` after every wait. At shutdown the main
thread sets each worker's `cancel` and posts its semaphores; it does not
touch `len_cmp`, which belongs to the worker, so the writer tests
`cancel` first. Flags read by one thread and set by another
(`main_cancel`, `t_errored`, `cancel`, `arc_closed`) use atomic loads and
stores.

### Global dedupe token ring

Global deduplication (`-G`) needs a shared block index that must be
updated in input order. Workers form a ring: worker *i* waits on its
`index_sem` and posts `index_sem_next` (worker *i+1*) when done, so
index access is serialised without locks. During decompression the
writer posts `index_sem_next` after writing a chunk, because the next
chunk may reference bytes that are only then on disk.

Rules that keep the ring live:

1. Every chunk must take and pass the token exactly once, including
   chunks that are too small to dedupe and chunks that fail.
2. Take the token as late as possible. Checksum verification of a chunk
   that needs no index access happens *before* the wait, so it runs in
   parallel; with the wait first, decompression ran on ~1.3 of 192 cores.
3. On error, set `t_errored`, skip the wait, and let shutdown post every
   `index_sem` so no worker stays parked.

### Error propagation

A worker that fails sets `pctx->t_errored = 1` and `pctx->main_cancel =
1`, sets `len_cmp = 0` and posts `cmp_done_sem`. The writer does the same
for a failed write. `start_compress`/`start_decompress` read `t_errored`
*after* joining the threads and return it, which becomes the exit status.
Any new failure path must do all of this; setting only `main_cancel`
makes pcompress stop *and exit 0*. (Reading `t_errored` before the joins
let about 1 in 200 corrupt files exit 0 with truncated output.)

Before logging an error, a failure site may call
`log_error_code(PC_ERR_...)`; the first class recorded in a call is what
`pc_last_error_code()` returns. An error logged with `errno` counts as
`PC_ERR_IO` unless a site said otherwise.

## Library and contexts

One `pc_ctx_t` is one job; the struct is private (`pcompress.h`) and
programs see only `include/pcompress/pcompress.h`. Contexts are
independent so several can run in one process:

- Set once per process with `pthread_once`: the CPU probe, the hash and
  AES dispatch tables and the Rabin polynomial tables. They depend only
  on the CPU.
- Per context: settings, the global-dedupe index (`pctx->dedupe_index`,
  shared only by that job's workers), the archive filter table, the log
  sink and level, the last error, and option parsing (`pc_getopt`, a
  reentrant replacement for `getopt`).
- Every library thread (worker, writer, archiver, extractor, metadata)
  starts with `log_bind(&pctx->log)`, so `log_msg()` anywhere reaches the
  right context; the public entry points bind it for the caller's thread.
- The library does not install signal handlers, `exit()` or `chdir()`.
  Extraction prefixes entry paths with the target directory instead.
- packJPG and packPNM keep their state in file statics, so calls into
  them are serialised by a mutex. The adaptive `-C` counters are shared.

`pc_compress_fd()`/`pc_decompress_fd()` run the CLI's stream (`-p`) path
with the caller's descriptors in place of stdin/stdout, in a fresh
internal context built from the caller's settings, so one `pc_ctx_t` can
run many jobs. Only the symbols in `libpcompress.map` are exported.

## Deduplication

`rabin/rabin_dedup.c` cuts a chunk into content-defined blocks with a
Rabin rolling hash (`-D`) or fixed blocks (`-F`), hashes each block with
XXH32 and replaces repeats with references. With `-E`, blocks whose
MinHash sketch matches an earlier block at least `deltac_min_distance`
bytes away are delta-encoded with bsdiff.

Global dedupe (`rabin/global/`) keeps a hash index of strong block
digests (SHA-256 by default) across the whole input. If the index would
not fit in memory, a segmented mode stores per-segment block lists in a
temp file and indexes only a few MinHash values per segment.

References in a decompressed stream point either into the chunk being
rebuilt or backwards into the output file (which is `mmap`ed). Both are
untrusted input: `dedupe_decompress` checks that a reference lies in data
that already exists before copying it.

## Hashes

XXH32 values index dedupe tables, so every build of a given file format
must produce the same values on every CPU. pcompress's one-shot `XXH32`
is a 32-byte-stripe variant of xxHash32 (for SIMD) and is pinned by
known-answer tests in `test/unit/test_xxhash.c`. The SSE2/SSE4/AVX-512
builds, the portable `_GENERIC` build (all non-x86 targets) and the
ARM64 build must all pass it.

BLAKE2b/BLAKE2bp (the default chunk checksum) dispatch to
SSE2/SSSE3/SSE4.1/AVX/AVX-512 on x86, NEON on ARM64 and a portable RFC
7693 implementation elsewhere. All are checked against the official KAT
vectors.

## Buffers

Every worker has one input and one output buffer of
`compressed_chunksize`, which is the chunk size plus the worst-case
expansion of the selected algorithm (`*_buf_extra()`/`props->buf_extra`)
plus headers. A compressor must never write more than `*dstlen` bytes:
if the output would not fit it returns -1 and the chunk is stored raw.
Two heap overflows (lz4, libbsc) came from compressors that sized their
output from the input length instead of `*dstlen`.

Buffers come from the system allocator. Pcompress used to ship a
process-wide slab allocator; it was removed in 4.1 because it was not
safe with several contexts in one process and measured no faster than
`malloc` (within 2.5% on a 192-core Graviton4, within noise on a
16-vCPU c7i for every algorithm at 1 MB to 256 MB chunks).

## Chunk objects

`chunk.c` gives backup tools three building blocks; none of them touches
the `.pz` format.

- **Chunker.** The Rabin rolling hash of the dedupe code (16-byte window,
  the same polynomial tables), restarted at every boundary. A boundary
  follows byte *i* once the chunk has `min` bytes and
  `(roll ^ ir[out]) & (avg - 1) == 0`, or at `max`. Since state resets at
  each boundary, the boundaries depend only on the data, never on how it
  was split across `pc_chunker_feed()` calls.
- **Ids.** BLAKE2b-256, optionally keyed.
- **Objects** (`pc_chunk_seal`/`pc_chunk_open`), little-endian:

| offset | size | field |
|--------|------|-------|
| 0  | 4  | magic `PCK1` |
| 4  | 1  | version (1) |
| 5  | 1  | flags: 1 encrypted, 2 stored raw |
| 6  | 1  | algorithm id (stable table in `chunk.c`; 0 when raw) |
| 7  | 1  | level (low 4 bits), adaptive sub-algorithm (high 4) |
| 8  | 8  | plaintext length |
| 16 | 24 | nonce (random when encrypted, else 0) |
| 40 | n  | payload |
| 40+n | 32 | tag: BLAKE2b-256 over bytes [0, 40+n) |

With a key, the payload is XSalsa20-encrypted and the tag is keyed
(encrypt-then-MAC); the encryption and MAC keys are BLAKE2b-256 of the
caller's key with distinct labels. `pc_chunk_open()` checks magic,
version, flags, algorithm id and the declared length (at most 4 GB, and
for raw objects exactly the payload length) before anything else, then
the tag in constant time, and only then decrypts and decompresses into
exactly the declared length. The algorithm ids never change meaning:
add new ones at the end.

**Key files** (`pc_keyfile_create`/`open`, 128 bytes): magic `PCKEY1`,
KDF id (1 = scrypt), log2 N, r, p, a 16-byte salt and a 24-byte nonce,
8 reserved bytes, the 32-byte key XSalsa20-encrypted, and a BLAKE2b-256
tag over everything before it. scrypt yields 64 bytes: 32 to encrypt, 32
for the tag. New files use N=2^17, r=8, p=1; open refuses log2 N outside
10-22, r outside 1-32 and p outside 1-16, so a crafted file cannot make
it allocate more than 512 MB.

**Passwords for `.pz`.** The whole password is used. Up to 4.1.0 a `-w`
file over 16 bytes was read as its first 15 bytes; decryption tries that
form too when the two differ, and the header HMAC decides.

## Archive mode

The archiver thread walks the paths with libarchive, sorts members by
type (so similar data shares chunks), runs per-file filters and writes a
PAX stream into the chunker. Metadata goes through the metadata stream so
that headers compress well and data chunks stay homogeneous.

pcompress carries a small patch to libarchive
(`archive/patches/pcompress_libarchive.patch`, applied by `./config`):

- a per-request *metadata* flag, set while libarchive reads or writes a
  header, so the callbacks can route bytes to the right stream;
- a *shadow* read filter, so metadata and data reads keep separate
  buffers and positions;
- skipping an entry's unread data before switching to the metadata
  stream (otherwise `-K` desynchronised the archive);
- two xattr helpers used to tag filtered entries.

On extraction, filters are reversed per entry; if a filter cannot decode
an entry the stored bytes are written unchanged and the path is listed in
`filter_failures.txt`.

## Encryption

A key is derived from the password and a random salt with scrypt.
Chunks are encrypted in place with AES-CTR or XSalsa20, using the chunk
number in the nonce, and authenticated with HMAC over the header and
ciphertext. The HMAC is checked before decryption. Passwords and key
material are zeroed after use.

## Portability

- POSIX threads and semaphores, `mmap`, `socketpair` and libarchive are
  required. Tested on Linux (x86-64, ARM64, riscv64) and illumos
  (SPARC); macOS and FreeBSD have code paths but are not regularly
  tested. A native Windows port would need replacements for all of
  these.
- x86 SIMD code is selected at runtime from CPUID, so `--portable`
  builds run on any x86-64. Non-x86 targets use portable C everywhere;
  ARM64 also has NEON BLAKE2.
- Only 64-bit targets are supported.

## Testing

`make test-unit`, `test/run_all_tests.sh` and `test/feature_matrix.sh`
are described in `docs/TESTING.md`. `./config --enable-coverage && make
coverage` reports coverage of project-owned code. When you fix a bug,
add the reproducer to `test/feature_matrix.sh` or a unit test.
