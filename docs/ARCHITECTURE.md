# Pcompress Architecture

## Overview

Pcompress is a chunked parallel multi-algorithm lossless compression and
decompression utility. It splits input data into chunks that are processed in
parallel across multiple threads. It supports archiving (via libarchive/PAX),
variable block deduplication (via Rabin fingerprinting), delta compression
(via bsdiff), multiple compression algorithms, encryption, and a rich set of
pre-processing filters to maximize compression effectiveness.

## High-Level Data Flow

### Compression Pipeline

```
Input File/Stream
       |
       v
  [Archiver]  (if -a archive mode)
  PAX stream via libarchive
       |
       v
  [Chunking]
  Split into fixed-size segments (configurable via -s)
       |
       v
  [Per-Thread Processing]  (parallel, one chunk per thread)
       |
       +---> [Deduplication]  (if -D, -F, or -G)
       |         Rabin fingerprinting / fixed-block / global index
       |
       +---> [Delta Compression]  (if -E)
       |         bsdiff between similar blocks (MinHash similarity)
       |
       +---> [Pre-Processing Filters]
       |         LZP, Delta2, Dispack (x86), PackJPG, WavPack, etc.
       |
       +---> [Compression]
       |         Algorithm selected per-chunk (adaptive) or globally
       |         LZMA, Bzip2, PPMD, Zstandard, LZ4, LZFX, Zlib, Libbsc, none
       |
       +---> [Encryption]  (if -e)
       |         AES-CTR or Salsa20-CTR
       |         Key derived via Scrypt (from Tarsnap)
       |
       +---> [Checksum / HMAC]
       |         BLAKE2, SHA256, SHA512, Keccak, CRC64, SKEIN
       |
       v
  [Serialization]
  Write chunk headers + compressed data to output
       |
       v
  Output .pz File
```

### Decompression Pipeline

The decompression pipeline reverses the above steps:

```
Input .pz File
       |
       v
  [Parse File Header]
  Recover algorithm, flags, encryption params
       |
       v
  [Per-Chunk Processing]  (parallel)
       |
       +---> [Verify Checksum / HMAC]
       +---> [Decrypt]  (if encrypted)
       +---> [Decompress]
       +---> [Reverse Pre-Processing Filters]
       +---> [Reverse Deduplication / Delta]
       |
       v
  [Extractor]  (if archive mode)
  Restore files via libarchive
       |
       v
  Output File(s)
```

## Source Code Organization

```
pcompress/
|-- main.c                    Entry point
|-- pcompress.c               Core compression/decompression engine (~107K)
|-- pcompress.h               Main public API and data structures
|-- adaptive_compress.c       Adaptive mode: auto-select algo per chunk
|-- meta_stream.c/h           Metadata stream handling (pathname metadata)
|-- allocator.c/h             Slab allocator for repeated similar allocations
|
|-- archive/
|   |-- pc_archive.c/h        PAX archiving via libarchive
|   |-- pc_arc_filter.c/h     Archive-level content filters
|   |-- dispack_helper.cpp    x86 Dispack filter integration
|   |-- pjpg_helper.cpp       PackJPG filter integration
|   |-- ppnm_helper.cpp       PackPNM filter integration
|   |-- wavpack_helper.c      WavPack audio filter integration
|   `-- libarchive/            Bundled libarchive sources
|
|-- crypto/
|   |-- crypto_utils.c/h      Encryption, checksums, HMAC API
|   |-- sha2_utils.c/h        SHA-2 family wrappers
|   |-- sha3_utils.c/h        SHA-3 (Keccak) wrappers
|   |-- aes/                   AES implementation (AESNI + VPAES)
|   |-- blake2/                BLAKE2 hash (SSE2/3/4/AVX variants)
|   |-- keccak/                Keccak/SHA-3 (opt64 + x86-64 asm)
|   |-- scrypt/                Scrypt KDF from Tarsnap
|   |-- sha2/                  SHA-512 (Intel AVX/SSE4 asm)
|   |-- skein/                 Skein hash (x86-64 asm block)
|   `-- xsalsa20/              XSalsa20 stream cipher
|
|-- rabin/
|   |-- rabin_dedup.c/h        Rabin fingerprint deduplication engine
|   `-- global/
|       |-- index.c/h          Global deduplication hash index
|       `-- dedupe_config.c/h  Dedup configuration and segmented mode
|
|-- bsdiff/
|   |-- bsdiff.c               Binary diff for delta compression
|   |-- bspatch.c              Binary patch for delta decompression
|   `-- rle_encoder.c          RLE encoder for bsdiff output
|
|-- filters/
|   |-- analyzer/              Data type analyzer (text vs binary detection)
|   |-- delta2/                Adaptive delta encoding
|   |-- dict/                  Dictionary-based filter
|   |-- dispack/               x86 instruction stream filter
|   |-- lzp/                   LZ Prediction pre-compressor
|   |-- packjpg/               JPEG recompressor
|   |-- packpnm/               PNM image recompressor
|   `-- transpose/             Matrix transpose filter
|
|-- lzma/                      LZMA SDK (encoder, decoder, PPMD, CRC)
|-- lz4/                       LZ4 fast compression library (submodule)
|-- lzfx/                      LZFX fast compression library
|-- zstd/                      Zstandard compression library (submodule)
|-- bsc/                       Libbsc block-sorting compressor
|
|-- utils/
|   |-- utils.c/h              Common utilities, types, byte-order macros
|   |-- cpuid.c/h              x86 CPUID detection (SSE/AVX capabilities)
|   |-- cpu_features.c/h       Platform-agnostic CPU feature abstraction
|   |-- cpu_features_x86_64.c  x86-64 feature detection backend
|   |-- cpu_features_arm64.c   ARM64 feature detection backend
|   |-- heap.c/h               Min-heap data structure
|   |-- xxhash.c/h             xxHash non-cryptographic hash (SSE2/SSE4)
|   |-- phash/                 Perfect hashing (file type lookup)
|   `-- sse_level.c            Runtime SSE level detection
|
|-- *_compress.c               Per-algorithm wrappers:
|   |-- zlib_compress.c          Zlib/gzip
|   |-- bzip2_compress.c         Bzip2
|   |-- lzma_compress.c          LZMA / LZMA-MT
|   |-- ppmd_compress.c          PPMD
|   |-- zstd_compress.c          Zstandard (with level mapping and MT support)
|   |-- lz4_compress.c           LZ4
|   |-- lzfx_compress.c          LZFX
|   |-- libbsc_compress.c        Libbsc
|   `-- none_compress.c          Pass-through (no compression)
|
|-- test/                      Test suites (t1.tst through t9.tst)
|-- config                     Build configuration script
`-- Makefile.in                Makefile template
```

## Core Components

### 1. Compression Context (`pc_ctx_t`)

Defined in `pcompress.h`, `pc_ctx_t` is the central state structure holding all
configuration and runtime state. It stores function pointers to the selected
algorithm's compress/decompress/init/deinit/stats/props functions, plus all
flags for deduplication, encryption, archive mode, preprocessing, and threading.

Key lifecycle:
1. `create_pc_context()` -- allocate and zero-initialize
2. `init_pc_context()` -- parse CLI arguments, configure all options
3. `start_pcompress()` -- run compression or decompression
4. `destroy_pc_context()` -- free all resources

### 2. Algorithm Interface

Every compression algorithm implements a uniform interface:

| Function        | Signature                                                                  | Purpose                    |
|-----------------|----------------------------------------------------------------------------|----------------------------|
| `*_init`        | `int (void **data, int *level, int nthreads, uint64_t chunksize, int file_version, compress_op_t op)` | Initialize algorithm state |
| `*_compress`    | `int (void *src, uint64_t srclen, void *dst, uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data)` | Compress a buffer          |
| `*_decompress`  | Same as compress                                                           | Decompress a buffer        |
| `*_deinit`      | `int (void **data)`                                                        | Free algorithm state       |
| `*_props`       | `void (algo_props_t *data, int level, uint64_t chunksize)`                | Report algorithm properties|
| `*_stats`       | `void (int show)`                                                          | Print statistics           |

These are stored as function pointers (`compress_func_ptr`, `init_func_ptr`,
etc.) in the `pc_ctx_t` and in `utils.h`.

### 3. Threading Model

Pcompress uses a producer-consumer model with POSIX threads and semaphores:

- **Main thread**: reads input, fills chunk buffers, signals worker threads
- **Worker threads** (one per `-t` count): each owns a `struct cmp_data`
  containing pre-allocated buffers, semaphores for synchronization, and a
  reference to the compression function pointers
- **Writer**: serializes compressed chunks to output in order

Semaphore-based coordination (`Sem_t` wrapper around POSIX `sem_t`) ensures:
- Workers start only when their input buffer is ready
- Output ordering is preserved despite parallel processing
- Back-pressure when the writer falls behind

### 4. Deduplication Engine

Located in `rabin/rabin_dedup.c` (~50K lines), the deduplication engine
supports three modes:

- **Per-chunk Rabin dedup** (`-D`): content-defined chunking within each
  compression segment using polynomial fingerprinting
- **Fixed-block dedup** (`-F`): fixed-size block boundaries, faster but lower
  dedup ratio
- **Global dedup** (`-G`): maintains an in-memory hash index across the entire
  dataset. Falls back to segmented similarity-based dedup when RAM is
  insufficient

Delta compression (`-E`) uses MinHash-based similarity detection followed by
bsdiff to encode differences between similar blocks.

### 5. Cryptographic Subsystem

All cryptographic operations are in `crypto/`:

- **Checksums**: BLAKE2 (default), SHA-256, SHA-512, Keccak-256/512, CRC64,
  SKEIN-256/512. Selected via `-S` flag.
- **Encryption**: AES-256-CTR or XSalsa20. Key derivation uses Scrypt from
  Tarsnap. A unique salt is generated per session.
- **HMAC**: When encryption is active, chunk authentication uses HMAC with the
  selected hash algorithm.

SIMD-optimized variants are compiled for multiple ISA levels (SSE2, SSSE3,
SSE4.1, AVX) with runtime dispatch via `cpuid.c`.

### 6. Archive Subsystem

When `-a` is specified, Pcompress operates as a full archiver:

1. `setup_archiver()` configures libarchive to produce a PAX stream
2. `start_archiver()` runs in a separate thread, feeding the PAX stream into
   compression chunks
3. Content-aware filters (`pc_arc_filter.c`) are applied per-file based on
   detected type (JPEG, WAV, executable, etc.)
4. Metadata streams (`meta_stream.c`) pack pathname information into separate
   chunks for better compression

### 7. Pre-Processing Filters

Filters transform data before compression to improve ratios:

| Filter           | Location                       | Purpose                                      |
|------------------|--------------------------------|----------------------------------------------|
| LZP              | `filters/lzp/`                 | LZ Prediction: replace repeating text runs   |
| Delta2           | `filters/delta2/`              | Adaptive delta + RLE for numerical tables    |
| Dispack          | `filters/dispack/`             | x86 call/jmp relative-to-absolute transform  |
| PackJPG          | `filters/packjpg/`            | Lossless JPEG recompression                  |
| PackPNM          | `filters/packpnm/`            | Lossless PNM image recompression             |
| WavPack          | `archive/wavpack_helper.c`     | Lossless WAV audio recompression             |
| Dict             | `filters/dict/`                | Dictionary-based pre-compression             |
| Transpose        | `filters/transpose/`           | Matrix transpose for columnar data           |
| Analyzer         | `filters/analyzer/`            | Data type detection (text/binary/subtypes)   |

## File Format

The `.pz` file format is documented in `compressed_file_format.txt`. In summary:

```
[File Header: algo(8) + version(2) + flags(2) + chunksize(8) + level(4)]
[Optional: encryption salt + nonce + keylen]
[Header checksum: CRC32 or HMAC]
[Metadata chunks]  (if archive mode, pathname data)
[Data chunks]*     (compressed segments with individual headers)
[Trailer: 8 zero bytes]
```

Each chunk header contains: compressed length, data verification hash, header
CRC32/HMAC, chunk flags (compression status, dedup status, adaptive algo
selection, pre-processing indicators, variable-size flag), and optionally the
original uncompressed size.

## SIMD Architecture

SIMD optimizations are used throughout the codebase for x86-64:

- **BLAKE2**: SSE2, SSSE3, SSE4.1, and AVX variants compiled separately
  (`crypto/blake2/blake2b_sse2.c`, etc.)
- **xxHash**: SSE2 and SSE4.2 variants (`utils/xxhash_sse2.c`,
  `utils/xxhash_sse4.c`)
- **SHA-512**: Intel AVX and SSE4 assembly (`crypto/sha2/intel/`)
- **Keccak**: x86-64 assembly optimized (`crypto/keccak/`)
- **AES**: AESNI and VPAES assembly (`crypto/aes/`)
- **Skein**: x86-64 assembly block function (`crypto/skein/`)
- **XSalsa20**: Optional assembly stream function

### CPU Feature Detection

Two layers of CPU detection exist:

1. **Legacy x86-only** (`utils/cpuid.c`, `utils/cpuid.h`): Uses CPUID
   instructions to populate `processor_cap_t` with SSE/AVX/AES-NI levels.
   Guarded by `#ifdef __x86_64__`.

2. **Platform-agnostic abstraction** (`utils/cpu_features.h`,
   `utils/cpu_features.c`): Provides a unified `cpu_features_t` structure
   with architecture-neutral feature flags (`CPU_FEAT_SIMD_BASE`,
   `CPU_FEAT_AES`, etc.). Dispatches to platform-specific backends:
   - `utils/cpu_features_x86_64.c` -- wraps existing CPUID logic
   - `utils/cpu_features_arm64.c` -- uses `getauxval(AT_HWCAP)` on Linux

   The `cpu_features_detect()` function populates the feature struct and
   `cpu_has_feature()` queries individual capabilities. See
   `docs/SIMD_OPTIMIZATION.md` for dispatch patterns.

## Build System

The build uses a custom `config` script (not autoconf) that generates a
`Makefile` from `Makefile.in`. Key configuration options include:

- Debug vs release builds
- OpenSSL, zlib, bzip2 library paths
- External libbsc linking
- WavPack support
- SSE/AVX detection and flag generation
- YASM assembler for Intel SHA-2 assembly

The build produces both a shared library (`libpcompress.so`) and the
`pcompress` command-line binary that links against it.

## Memory Management

The slab allocator (`allocator.c/h`) provides an arena-based allocation scheme
optimized for the repeated alloc/free pattern of compression buffers. It
maintains size-class caches to avoid repeated calls to `malloc`/`free`.

Can be bypassed by setting `ALLOCATOR_BYPASS=1` in the environment.

## Dependencies

| Dependency   | Purpose                    | License        | Integration    |
|-------------|----------------------------|----------------|----------------|
| OpenSSL     | Cryptographic primitives   | Apache-2.0     | System library |
| zlib        | Zlib/gzip compression      | zlib License   | System library |
| libbz2      | Bzip2 compression          | BSD-like       | System library |
| libarchive  | PAX archive handling       | BSD            | Submodule      |
| Libbsc      | Block-sorting compressor   | Apache-2.0     | Submodule      |
| LZMA SDK    | LZMA/PPMD compression      | Public Domain  | Bundled        |
| Zstandard   | Fast balanced compression  | BSD-3-Clause   | Submodule      |
| LZ4         | Fast compression           | BSD-2-Clause   | Submodule      |
| LZFX        | Fast compression           | BSD            | Bundled        |
| Scrypt      | Key derivation (Tarsnap)   | BSD            | Bundled        |
| PackJPG     | JPEG recompression         | LGPLv3         | Bundled        |
| WavPack     | WAV recompression          | BSD            | Bundled        |
