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
the writer check `main_cancel` after every wait.

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
for a failed write. `start_compress`/`start_decompress` return
`t_errored`, which becomes the exit status. Any new failure path must do
all of this; setting only `main_cancel` makes pcompress stop *and exit
0*.

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

The slab allocator (`allocator.c`) caches these large, same-sized
buffers. `ALLOCATOR_BYPASS=1` uses `malloc` directly; on a 192-core
Graviton4 the two were within 2.5% of each other.

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
