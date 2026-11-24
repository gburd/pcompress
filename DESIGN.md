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
              [salt, KDF cost if encrypted]  CRC32 or AEAD tag
chunk*        len_cmp[8] digest[n] mac[m] flags[1] payload [orig_size[8]]
trailer       0[8]  [end record if encrypted: counts[16] tag[16]]
```

(Version 12, encrypted. Versions 7 to 11 have salt, nonce, key length
and, from 11, the scrypt cost in the header, an HMAC in place of the
tag, and no end record.)

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
Encrypted files are written as `VERSION_AEAD` (12, since 4.5): an AEAD
per chunk bound to its stream and position, and an end record (see
Encryption); 4.3 and 4.4 wrote them as `VERSION_KDF` (11), which this
reader still reads. Keeping plain files at 10 lets older releases still
read them, and 4.4 refuses version 12 as newer than it knows. The reader accepts `VERSION-4` up to
`VERSION_MAX`; older-version quirks are handled inline (look for
`version <`). Bump it only when the format changes, never for a release.

Version 13 (`VERSION_FLAGS`, since 4.5) is version 10, or version 12
when encrypted, with one rule added: the reader refuses a header flag
bit it does not know (`FLAG_KNOWN`). Readers of 10 to 12 ignore unknown
bits, so a new bit alone could not stop them from misreading data that
needs it. A feature that changes how data must be decoded therefore
takes a free flag bit, and `write_file_header()` writes version 13 for
any flags outside the version 12 set (`FLAG_V12`): 4.4 and older refuse the file as newer, and a later build
without the feature refuses the bit, rather than either producing wrong
output. A file that uses no such feature keeps version 10 or 12, so
older releases still read it. Encrypted version 13 files follow every
version 12 rule (Argon2id, the AEAD streams, the end record); an
unencrypted one is a version 10 file in all but the number. Taken
bits: none yet. Free: 0x8, 0x40, 0x80, 0x2000, 0x4000, 0x8000.

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

`pc_set_progress()` cancels the same way, from the calling thread: after
it hands a chunk to a worker, `report_progress()` calls the host's
callback, and a non-zero return records `PC_ERR_CANCELLED`, sets
`pctx->cancelled` and `main_cancel`, and leaves the feed loop. The usual
teardown then releases and joins every thread; `cancelled` makes the
job fail (a cancel is not a worker failure, so `t_errored` stays 0) and
removes the output file the job created.

## Seek index

`-X` appends an index after the end-of-stream marker, so that `-r`
(a byte range of a single file) and `-R` (one archive member) decode
only the chunks they need (`seek_index.c` builds and parses the body;
`pcompress.c` places, authenticates and uses it).

```
... chunk*  0[8]  [end record]  body  tag[t]  len[8]  "PCSEEK01"
```

`len` (little-endian) is the body's size and `t` is the chunk tag size:
the checksum size, or 16 (the AEAD tag) when encrypted. The end record
is there only in encrypted files (format version 12). The body is
little-endian:

| field | size |
|-------|------|
| magic `PCSXIDX1` | 8 |
| t, then 7 zero bytes | 8 |
| file offset of the end-of-stream marker | 8 |
| chunk count, member count | 8 + 8 |
| per chunk: file offset of its length field, uncompressed length, its tag (the `t` bytes after its length field: its checksum, or its AEAD tag) | 16 + t |
| per member: PAX stream offset, length, u16 name length, name | 18 + n |

**Old readers.** A reader stops at the end-of-stream marker and never
reads past it, so 4.4 and earlier read an unencrypted `-X` file
(format version 10) byte for byte as any other (checked with the 4.4.0
binary by ci/xcompat.sh, for files and archives). This needs no new
version or flag, and an old reader cannot misread the index because it
never reads it. Encrypted files are version 12, which 4.4 refuses
whether or not they have an index.

**Authentication.** `sx_seal()` and `sx_check()` are the one place the
tag is defined. Without encryption it is the file's chunk checksum over
the body, which anyone can recompute: it catches damage, not forgery,
like the chunk checksums. With encryption (version 12) the body is
sealed with the file's AEAD as stream 4, ordinal 0 (see "Encryption"),
and the associated data is the body's length, the trailer magic, and
the end record that precedes the body. The end record is itself under
the file's key and holds the chunk counts, so an index cannot be moved
to another file, cut, or put after other chunks, and nothing else is
sealed in stream 4, so no other piece of the file passes as an index.
The body records the end marker's offset, so it cannot be moved along
the file either. Each chunk entry holds that chunk's own tag (its
checksum, or its 16-byte AEAD tag), and a random read checks every
chunk it reads against it before decoding. Without encryption that is
what binds a chunk to its place; with it, each chunk is also opened as
its own ordinal of stream 1 (the index gives the ordinal), so a moved
or swapped chunk fails twice over (test_seek.sh swaps two).

**Random reads of version 12 files.** A random read stops inside the
chunks, so it does not reach the end record and does not check the end
record's own tag. It does not need to: the index tag covers the end
record's bytes and the index holds the chunk count and every chunk's
offset and tag, so a truncated or extended file, or one whose chunks
were dropped, fails the index or the chunk check. A full decode of an
indexed file checks the end record as usual and then accepts the index
after it unparsed (the trailer magic must be last).

**Untrusted.** The reader checks the trailer magic, then bounds `len`
by the file and by 1 GB, reads the 8 bytes before the body (they must
be the zero marker), and verifies the tag over the body mapped from the
file (constant time for the checksum; the AEAD only checks, decrypting
into a scratch buffer it wipes), so a forged length costs no memory. Only
then is the body read into memory; its tag is checked again on that
copy (the file may have changed), and it is decrypted and parsed. `sx_parse()` checks every count against the body's
size, the end marker against the file, every chunk inside
[header, marker) in increasing order and decoding to 1 to chunksize
bytes, and every member inside the stream, in order, with a nonempty
name and no NUL. `test_seek_index` pins the encoding with a vector
built independently in Python, and `test/fuzz/fuzz_seek_index.c` fuzzes
the parser.

**What it serves.**

- Global dedupe (`-G`, and the automatic choice at level > 3) makes a
  chunk depend on any chunk before it. `-X` turns the automatic choice
  off and refuses `-G`. Per-chunk dedupe (`-D`, `-F`) and delta (`-E`)
  stay chunk-local and work.
- The metadata stream puts entry headers in chunks of their own, so a
  member would be two streams. `-X` turns it off (as `-T` does). A
  member is then one range of the PAX stream: its record runs from its
  header to the end of its padding, written by `write_header()` after it
  finishes the previous entry. A member read feeds that range to the
  extractor, followed by two zero blocks, so libarchive sees a
  one-member archive and applies the usual path checks.
- Pipe output works, since the index goes at the end. Reading needs a
  seekable file: `-r`/`-R` on standard input, on a file without the
  trailer, or on a global-dedupe or metadata-stream file is refused
  with a message. It never falls back to a full decode.
- Decoding the chosen chunks runs on the usual workers. `dec_feed()`
  seeks to each one and starts numbering at the first (the number keys
  its cipher stream). The writer checks that each chunk decodes to the
  length the index gives it, and keeps the part inside the range.

Cost, on a c7i.4xlarge, zstd -l6, cold cache: a 1.3 GB tar of /usr/lib
(21525 files) with `-T -D` against `-T -D -X` grows from 426.6 MB to
428.6 MB (+0.47%: a 2.0 MB index plus 0.1% from the PAX stream's
per-member alignment). Time is 25.7 s against 26.0 s, within run-to-run
noise (25.2 to 26.3 s). Extracting one 20 KB member near the end takes
0.08 s with the index and 3.0 s without. The index is opt-in because
`-X` gives up global dedupe and the metadata stream (those make the
default archive 419.5 MB), not because of its own size.

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

## Embedding

A host runtime (an event loop, a backup daemon) can give the library its
own process-wide hooks before the first call. They are plain globals set
once: a host sets them at start-up, before it creates threads that use
the library, and does not change them while jobs run.

- `pc_set_allocator()`: every allocation in the library's own code goes
  through `pc_malloc()`, `pc_calloc()`, `pc_realloc()`, `pc_mfree()` and
  `pc_strdup()` (`utils/pc_alloc.h`, in `allocator.c`), which call the
  host's functions or the C library's. The `slab_*` wrappers the codecs
  and the pipeline use call them too. zstd gets them through
  `ZSTD_customMem`, libbsc through `bsc_init_full()`, lz4 through
  `LZ4_USER_MEMORY_FUNCTIONS`; lzma, PPMd, zlib and bzip2 through their
  allocator structs. Aligned memory (BLAKE2b and Keccak states, scrypt)
  is over-allocated through `pc_malloc()` with the block's own address
  stored below the aligned one, so there is no `posix_memalign()` /
  `_aligned_malloc()` split. The first allocation sets a flag, and from
  then on `pc_set_allocator()` refuses: a block must go back to the
  allocator it came from.
  Memory another library allocates goes back through that library:
  `realpath()` results are copied (`pc_realpath()`) and `free()`d,
  packJPG/packPNM output is copied into `pc_malloc()` memory
  (`pc_adopt()`), OpenSSL HMAC contexts go to `HMAC_CTX_free()`.
  Not covered: libarchive, OpenSSL, the C++ filters (packJPG, packPNM,
  dispack, the dictionary filter) and the OpenMP runtime. Nothing the
  library allocates outlives the object that holds it: the extension
  table is static, and `test_alloc` checks that no block is live once
  every object is freed.
- `pc_set_default_log()`: the sink and level `log_msg()` uses when the
  thread has no context bound, or the bound context has no sink or
  level of its own (`utils/utils.c`, next to `log_msg()`).
- `pc_set_thread_hooks()`: every thread the library creates goes through
  `pc_thread_create()` (`utils/utils.c`), which runs the host's start
  hook with the thread's name, the thread function, then the stop hook.
  A thread keeps the hooks that were set when it was created. Threads
  started inside OpenMP, zstd's pool, lzmaMt and libbsc are not covered.

## Deduplication

`rabin/rabin_dedup.c` cuts a chunk into content-defined blocks (`-D`) or
fixed blocks (`-F`), hashes each block with XXH32 and replaces repeats
with references. A content-defined block ends after the first byte past
`min` (about 1 KB below the `-B` average) where the top 10 bits of the
gear hash of `chunk.c` are zero, or at 128 KB (64 KB for `-G` below
`-B3`). Before 4.5 this was a Rabin rolling hash; the decoder reads
block lengths, so the boundaries are not part of the format and files
from either version read in both. On a 16-vCPU c7i host the gear scan
cut single-thread CPU time for `-D` by 18 to 30% (tar of five PostgreSQL
trees, a tar of `/usr/lib`, two ext4 images) with output within 0.5%
of Rabin's; `-G` output moved by under 1.2% either way, which is the
noise of where boundaries fall. FastCDC's normalized chunking deduped
11% worse at `-G` and is not used. With `-E`, blocks whose
MinHash sketch matches an earlier block at least `deltac_min_distance`
bytes away are delta-encoded with bsdiff.

Global dedupe (`rabin/global/`) keeps a hash index of strong block
digests (SHA-256 by default) across the whole input. If the index would
not fit in memory, a segmented mode stores per-segment block lists in a
temp file and indexes only a few MinHash values per segment.

References in a decompressed stream point either into the chunk being
rebuilt or backwards into the output file (which is `mmap`ed). Both are
untrusted input: `dedupe_decompress` checks that a reference lies in data
that already exists before copying it.  The index is untrusted too: the
block count must fit the context's block array, a duplicate or delta block
must name an earlier literal block, and every literal, reference and
bsdiff patch must lie inside the chunk's block data and the restored size
before anything is copied.  `test/fuzz/fuzz_dedupe.c` fuzzes all three
forms (`-D`, `-F`, `-G`).

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

Buffers come from the system allocator, or the host's (see Embedding
below). Pcompress used to ship a
process-wide slab allocator; it was removed in 4.1 because it was not
safe with several contexts in one process and measured no faster than
`malloc` (within 2.5% on a 192-core Graviton4, within noise on a
16-vCPU c7i for every algorithm at 1 MB to 256 MB chunks).

## Chunk objects

`chunk.c` gives backup tools three building blocks; none of them touches
the `.pz` format.

- **Chunker.** A Rabin rolling hash (16-byte window, the polynomial
  tables of `rabin/rabin_dedup.c`), restarted at every boundary. A boundary
  follows byte *i* once the chunk has `min` bytes and
  `(roll ^ ir[out]) & (avg - 1) == 0`, or at `max`. Since state resets at
  each boundary, the boundaries depend only on the data, never on how it
  was split across `pc_chunker_feed()` calls.
- **Ids.** BLAKE2b-256, optionally keyed (`pc_chunk_id`), or, chosen per
  repository with `pc_chunk_id2`, BLAKE3-256, keyed with exactly 32
  bytes. BLAKE3 is the bundled official C code (`crypto/blake3`, 1.8.7),
  with its own run-time dispatch to SSE2, SSE4.1, AVX2 and AVX-512 on
  x86 and NEON on little-endian ARM64, portable C elsewhere; only the C
  intrinsics files are built (no `.S`), so it builds wherever the rest
  does. Object tags, subkeys and `pc_derive_key` stay BLAKE2b: the id
  function is the only part that hashes every byte for naming, and
  derive runs once per repository open, where speed does not matter.
- **Objects** (`pc_chunk_seal`/`pc_chunk_open`), little-endian:

| offset | size | field |
|--------|------|-------|
| 0  | 4  | magic `PCK1`, or `PCK2` when keyed (4.5) |
| 4  | 1  | version (1, or 2 with `PCK2`) |
| 5  | 1  | flags: 1 encrypted, 2 stored raw, 4 dictionary (4.5) |
| 6  | 1  | algorithm id (stable table in `chunk.c`; 0 when raw) |
| 7  | 1  | level (low 4 bits), adaptive sub-algorithm (high 4) |
| 8  | 8  | plaintext length |
| 16 | 24 | nonce (random when encrypted, else 0) |
| 40 | 32 | dictionary id, only with flag 4 |
| h  | n  | payload (h is 72 with flag 4, else 40) |
| h+n | 32 | v1 tag: BLAKE2b-256 over bytes [0, h+n) |
| h+n | 16 | v2 tag: Poly1305, from XChaCha20-Poly1305 |

Without a key an object is version 1 with an unkeyed BLAKE2b tag (an
integrity check). With a key, since 4.5, it is version 2: flag 1 always
set, the payload sealed with XChaCha20-Poly1305 under the random 24-byte
nonce and the first half of BLAKE2b-512(caller's key, "pcompress chunk
aead"), with header bytes [0, h) as associated data, so the whole header
is authenticated. A random 192-bit nonce per object: a repeat among 2^48
objects under one key has probability about 2^-96. 4.4 sealed keyed
objects as version 1: XSalsa20 with the first half of BLAKE2b-512(key,
"pcompress chunk subkeys") and a BLAKE2b-256 tag keyed with the second
(encrypt-then-MAC); those still open, and 4.4 refuses version 2 objects
(unknown magic). Subkeys are halves of 64-byte digests, never 32-byte
BLAKE2b outputs: chunk ids are exactly those, keyed with the same key
over caller data, and they are public as object names. Sealing with a
key ran at 468 MB/s and opening at 681 MB/s with v1 (zstd:3,
bench_chunk on a Xeon 8488C), 897 and 2226 MB/s with v2: one AEAD pass
in place of a cipher pass and a BLAKE2b pass. `pc_chunk_open()` checks magic,
version, flags, algorithm id and the declared length (at most 4 GB, and
for raw objects exactly the payload length) before anything else, then
the tag in constant time, and only then decrypts and decompresses into
exactly the declared length. The algorithm ids never change meaning:
add new ones at the end.

**Incompressible chunks.** Before running the codec, seal takes 16 runs
of 128 bytes spread over a chunk of 16 KB or more and computes the
chi-square statistic of their byte histogram against uniform. If it is
under 1000 (random bytes score 255), each 256 KB segment of the chunk
(or the whole chunk, if smaller) is sampled again with 64 runs, and
both its byte histogram and the histogram of differences between
neighbouring bytes must score under 350; then the chunk is stored raw
without compressing. The difference test keeps data whose byte values
are spread evenly but follow each other predictably (0..255 ramps,
lookup tables, counters) out of the raw path; the per-segment samples
keep a compressible region of a 4 MB chunk from falling between them.
On gear chunks of random data and H.264 video this skips 99% of chunks
and sealing at zstd -9 runs at 730 MB/s instead of 41; on `.gz`, `.xz`
and `.zst` files it skips 77% and loses 0.03% of the input in size; on
text, code, a tar of `/usr/lib`, ramps and raw RGB test images it skips
nothing and costs about 1% of seal speed. The `.pz` pipeline does not use it. Tried there (4 MB
chunks, one thread), it made zstd:3 output of `.gz`/`.xz`/`.zst` files
0.7% larger, since those chunks still hold headers and padding worth
compressing, and changed neither size nor time at lzma:6 or adapt2:6
(those runs take the preprocessing path, which it did not cover).

**Dictionaries** (`pc_set_dict`, 4.5). With flag 4 the payload is a zstd
frame compressed with a dictionary, and bytes 40-71 name it: BLAKE2b-256
of the dictionary bytes (`pc_dict_id`). The tag covers the id, so it
cannot be swapped. The flag needs algorithm 1 (zstd) and is never set
with flag 2: an object stored raw needs no dictionary. Open compares the
id with the context's dictionary after the tag check and fails with
`PC_ERR_DICT` when they differ or none is set, so a wrong dictionary is
an error, not garbage. zstd's own 4-byte dictionary id is left out of
the frame (`ZSTD_c_dictIDFlag` 0): the object's id already says which
one, with no chance of collision between repositories. The id costs 32
bytes, so seal keeps the dictionary frame only when it is at least 33
bytes smaller than the input, and `pc_chunk_seal_bound()` is unchanged.
Seal uses the dictionary only for chunks under 128 KB; larger ones are
plain zstd objects. zstd compresses with the match-finder tables of the
digested dictionary, sized for small inputs, and on 1 to 4 MB chunks
those cost up to 8% in size (`PCK_DICT_CHUNK` in chunk.c has the
numbers). Objects without a dictionary are byte-for-byte what 4.4 writes;
4.4 and older refuse flag 4 (an unknown flag) as corrupt.

**Key files** (`pc_keyfile_create`/`open`, 128 bytes): magic `PCKEY1`,
KDF id and its cost in bytes 6-15, a 16-byte salt and a 24-byte nonce,
8 reserved bytes, the 32-byte key XSalsa20-encrypted, and a BLAKE2b-256
tag over everything before it. The KDF yields 64 bytes: 32 to encrypt,
32 for the tag. KDF 1 is scrypt (byte 7 log2 N, then r and p as 32-bit
words), written up to 4.4; open refuses log2 N outside 10-22, r outside
1-32 and p outside 1-16, so a crafted file cannot make it allocate more
than 512 MB. KDF 2 is Argon2id (byte 7 lanes, then memory in KiB and
passes), written since 4.5 with 128 MiB, 4 passes and 4 lanes (0.29 s
on a Xeon 8488C); open refuses more than 1 GiB, 16 passes, 16 lanes or
4 GiB of memory traffic, before any work. 4.4 refuses KDF 2 files.

**Chunk index** (`pc_index_*`): the file is `PCIDX1`, two zero bytes,
then 32-byte ids in the order they were added. It is only appended to
(`O_APPEND`, whole records), loaded into an open-addressing hash table on
open, and a torn last record is cut off. The table holds 32 bits of each
id's hash and its record number, not the id: a match is confirmed by
reading the record back, so memory is 11 to 21 bytes per id. The `.pz` global-dedupe index
(`rabin/global/index.c`) is separate and still per file: making it
persistent would make one `.pz` depend on others.

**Manifests** (`pc_manifest_*`): `PCMF2`, three zero bytes, a u64 entry
count, then the entries sorted by path bytes, each: u16 path length and
path, u8 type, u32 mode, uid, gid, i64 mtime, u32 nanoseconds, u64 size,
u16 target length and target, u32 chunk count, and per chunk a 32-byte id
and a u32 length; then the entry's records, each a u8 tag, a u32 length
and a value, ended by a 0 byte. Little-endian throughout. The tags are
1 birth time, 2 change time, 3 device and inode, 4 rdev, 5 flags (u8
kind, u64), 6 owner name, 7 group name, 8 xattr (u8 name length, name,
value), 9 ACL (u8 kind, text), 10 subtree (32-byte id), and 11 sparse.
They come in tag order; only xattrs (by name) and ACLs (by kind) repeat.
A tag this library does not know is kept as the entry's `ext` bytes and
written back, unless it has bit 0x80 set: that marks it critical, and
the manifest is refused. `PCMF1` (4.2 and 4.3) is the same with no
records or end byte, and only the first three entry types; it still
decodes, and encodes as `PCMF2`.

The encoding of a manifest is canonical (sorted, no choices), so equal
snapshots give equal bytes and the same object id. The decoder checks
every length against what is left before allocating, every path
(relative, no empty, `.` or `..` component, no NUL), a hard link's
target the same way, strict order, no entry below a symlink entry or a
directory with a subtree, that a file's chunk lengths add up to its
size, and that every record is well formed, in order and allowed for the
entry's type. A manifest can hold a whole snapshot or one directory with
single-component names, its subdirectories pointing at their own
manifests; the library does not walk subtrees.

`examples/backup/pcbackup.c` puts the pieces together: a key file holds
the repository key, chunks and manifests are sealed objects named by
their keyed id, the index makes an unchanged backup cost no store
lookups, and prune deletes objects no snapshot names and rebuilds the
index. Restore checks every object against its name and only writes
below a directory it created.

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

**Format version 12 (since 4.5).** One key per file: Argon2id (RFC 9106)
of the password and a random 256-bit salt, at 128 MiB, 4 passes and 4
lanes (0.29 s on a Xeon 8488C). The header records the salt and cost;
the reader refuses more than 1 GiB, 16 passes, 16 lanes or 4 GiB of
memory traffic before any work. `-e SALSA20` selects XChaCha20-Poly1305
(OpenSSL's ChaCha20-Poly1305 behind HChaCha20, draft-irtf-cfrg-xchacha)
and `-e AES` AES-256-GCM (OpenSSL); 16-byte keys (`-k 16`) are not used
any more, the AEAD key is always 32 bytes.

Every authenticated piece of the file belongs to a *stream* and has an
*ordinal* in it (`pz_crypt.h`):

| stream | id | ordinal | sealed |
|--------|----|---------|--------|
| header | 0 | 0 | nothing; associated data: every header byte before the tag |
| data chunks | 1 | chunk number from 0 | payload; AD: `len_cmp`, flags, original size |
| metadata chunks | 2 | metadata chunk number from 0 | payload; AD: the 25 header bytes (indicator, lengths, flags) |
| end record | 3 | 0 | nothing; AD: data chunk count, metadata chunk count (u64 BE each) |
| seek index | 4 | 0 | the index body; AD: its length (u64 LE), `PCSEEK01`, the end record |

The nonce of a piece is its stream id (u32 BE) and ordinal (u64 BE),
zero-padded to the AEAD's length (12 bytes for GCM, 24 for XChaCha), and
its associated data starts with the same 12 bytes. Nonce uniqueness: a
key belongs to one file, since the salt is 256 random bits from the
system generator (no fallback: no random bytes, no file), and within a
file each (stream, ordinal) pair is sealed once, because data and
metadata chunks are numbered by the single thread that hands them out,
and the header and end record are written once. Ordinals and the
counters behind them are 64 bits end to end, so they do not wrap: 4.4
held the chunk number in 32 bits, and chunk 2^32 would have reused
chunk 0's nonce. Encrypted version 11 and earlier files are read only up
to chunk 2^32 - 1 (metadata chunk 2^31 - 1): past that their writer had
wrapped, and the reader refuses the file. So no nonce repeats
under a key, with either AEAD, and a deterministic nonce avoids GCM's
96-bit random-nonce limit. In 4.4 (version 11) data chunk *k* and
metadata chunk *k* used the same key and CTR nonce, a two-time pad; here
they are different streams.

What the tags bind: a chunk opens only at its own position in its own
stream, so reordering, repeating, or moving a chunk between streams
fails (the nonce differs and so does the AD). The chunk's length field,
flags and original size are AD, so none can change. The end record,
after the zero length that ends the chunks, holds both chunk counts; the
reader checks it after the last chunk (the data reader counts the
metadata chunks it skips; the metadata thread checks its own count when
it reaches the end), so dropping chunks off the end, or the end record
itself, fails. Decompressed output is written as chunks verify, so a
file truncated after chunk *k* has written *k* chunks when the error is
reported; pcompress then exits non-zero and removes the output file if
it created it (not a caller's descriptor, standard output, or files an
archive extraction already wrote). The same holds for any chunk that
fails. A reader given a password refuses a file that is not
encrypted, since a plain file put in place of an encrypted one would be
authenticated by nothing. Chunks keep the version 10 layout with the 16-byte tag in
the `mac` field and no digest: the AEAD tag is the integrity check.

The seek index (`-X`, see "Seek index") is sealed as stream 4, ordinal
0, with the end record in its associated data; the per-chunk tags it
lists are inside the sealed body. After the end record the reader
accepts the end of the file, or a region whose last 8 bytes are the
index trailer magic `PCSEEK01` (it does not parse the region; the
index reader authenticates it), and refuses anything else as corrupt,
so data appended to a file is not silently ignored.

**Version 11 and earlier** (read only). scrypt derives the key, at
N = 2^17, r = 8, p = 1 for version 11 (the header records the cost, and
the reader checks its bounds before any work), and a fixed N = 2^11 for
version 10 and earlier. Chunks are encrypted in place with AES-CTR or
XSalsa20, using the chunk number in the nonce, and authenticated with
HMAC over the header and ciphertext, checked before decryption. These
files have the weaknesses above, which is why new files are version 12.

Passwords and key material are zeroed after use, with pcmono_wipe()
(Monocypher's volatile store, which the compiler cannot drop as a dead
store): password buffers once the key is derived, the version 12 file
key when the job returns (compress or decompress, the end record
written or checked), and the chunk-object subkeys after each call.
Decrypted chunk buffers are not wiped before they are freed: on enwik8
(8 MB chunks, 16 threads, x86-64) that cost 45 ms on a 385 ms
decompression, 11%.

## Page cache

A file pcompress reads or writes is used once, front to back, and
would otherwise fill the page cache and push out what the rest of the
machine had cached (`utils/pc_cache.c`). For regular files only (never
pipes): the input is opened with `POSIX_FADV_SEQUENTIAL`, and every
32 MB consumed is dropped with `POSIX_FADV_DONTNEED`; an archive member
is dropped one 1 MB map at a time as it is copied. Output pages are
dirty, and `DONTNEED` ignores dirty pages, so on Linux the writer hands
each 32 MB window to writeback with `sync_file_range()` and drops it one
window later, by which time most of it is written. It does so only past
a tenth of RAM (the kernel's default background writeback threshold): a
smaller output stays cached, since starting writeback earlier made a
1.5 GB job wait for the disk (8.2 s against 0.4 s). Elsewhere (FreeBSD,
illumos) only input is dropped; macOS and Windows have no
`posix_fadvise()` and get no hints. The output of a global-dedupe
restore stays cached, since later chunks map it to copy references.
The hints are always on: they cost no throughput (see the Changelog).
They apply only to files the library opens: descriptors a caller hands
to `pc_compress_fd()` or `pc_decompress_fd()` are the caller's to
manage (`own_fd()`).

Archive extraction with a metadata stream reads the file through two
descriptors: the main reader, which drops pages behind it, and the
metadata thread's. Pages the main reader drops are not read again from
disk. The main reader seeks over metadata chunks without reading them,
and the metadata thread seeks over data chunks, so the only pages both
read are those holding chunk length fields, at most one per chunk.
Measured cold-cache extractions on a c7i.4xlarge (lz4 archives of
/usr/lib at 1.3 GB, of /usr/share and /usr/include, of 100000 files of
200 B to 3 KB, and of a mix of those with 60 MB files; chunks from
64 KB to 64 MB) read 0.5 to 0.7% more from disk than the file's size,
the same as a build without the hints, while adding half as much to
the page cache (569 MB against 1049 MB for the 520 MB mixed archive).

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
