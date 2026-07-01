# Vendored Dependency Patches

This document describes pcompress dependencies that are vendored (not git
submodules) and the custom modifications applied to each. It also covers
dependencies managed as git submodules and any API changes required.

## Dependency Overview

| Dependency     | Source                              | Version  | Type       | Notes                          |
|----------------|-------------------------------------|----------|------------|--------------------------------|
| libarchive     | archive/libarchive/                 | 3.7.9    | Submodule  | Custom patches applied at build|
| LZMA SDK       | lzma/                               | ~2008    | Vendored   | Custom struct fields, CRC code |
| LZ4            | lz4/                                | HEAD     | Submodule  | github.com/lz4/lz4            |
| libbsc         | bsc/                                | 3.3.12   | Submodule  | github.com/IlyaGrebnov/libbsc |
| Zstandard      | zstd/                               | HEAD     | Submodule  | github.com/facebook/zstd      |
| WavPack        | wavpack/                            | HEAD     | Submodule  | github.com/dbry/WavPack       |

---

## Submodule with Patches: libarchive (archive/libarchive/)

**Repository**: https://github.com/libarchive/libarchive
**Submodule version**: v3.7.9
**Patch file**: `archive/patches/pcompress_libarchive.patch`

libarchive is managed as a git submodule pointing to the upstream repository,
pinned at v3.1.2.  Custom pcompress extensions are maintained as a patch file
in `archive/patches/` and applied automatically during `./config`.  The
patches add metadata streaming and extended attribute management functions
used by the archive filter system.

### Custom Functions Added

#### `archive_request_is_metadata()` / `archive_set_metadata_streaming()`

**Files**: `archive_virtual.c`, `archive.h`, `archive_private.h`

These functions enable pcompress's separate metadata stream feature.  When
metadata streaming is active, the archive layer signals whether the current
read/write request is for metadata (file headers, directory entries, xattrs)
or for file data content.  This allows pcompress to route metadata and data
to different compression pipelines, improving overall compression ratio by
keeping similar data together.

The implementation adds two fields to `struct archive`:
- `cb_is_metadata` -- flag indicating the current callback is for metadata
- `is_metadata_streaming` -- flag to enable/disable the feature

These fields are checked in `archive_read.c` and
`archive_read_append_filter.c` to control data routing.

#### `archive_entry_xattr_delete_entry()` / `archive_entry_has_xattr()`

**Files**: `archive_entry_xattr.c`, `archive_entry.h`

These functions allow pcompress to tag individual archive entries with custom
extended attributes (xattrs) that record which pre-compression filter was
applied (e.g., packJPG for JPEGs, Dispack for executables, WavPack for audio).
During decompression, the xattr tag is read to determine the correct inverse
filter.

- `archive_entry_has_xattr(entry, name, &value, &size)` -- search for a
  named xattr and return its value.  Used during decompression to identify
  the filter type.
- `archive_entry_xattr_delete_entry(entry, name)` -- remove a named xattr.
  Used to clean up internal filter tags before writing the final output.

### Patch Management

The pcompress patches are applied automatically by the `config` script when
it detects the submodule is at a clean checkout (no `cb_is_metadata` in
`archive_private.h`).  The patches are maintained in
`archive/patches/pcompress_libarchive.patch` as a unified diff.

When updating libarchive to a newer version:

1. Update the submodule: `cd archive/libarchive && git checkout v3.x.y`
2. Test if the patch still applies: `git apply --check ../patches/pcompress_libarchive.patch`
3. If it fails, regenerate the patch against the new version
4. Review API changes between the old and new version
5. Consider submitting `archive_entry_has_xattr` and
   `archive_entry_xattr_delete_entry` upstream (they are generally useful)
6. The metadata streaming feature is pcompress-specific and will likely
   remain as a local patch

---

## Vendored: LZMA SDK (lzma/)

**Upstream**: 7-Zip LZMA SDK (https://7-zip.org/sdk.html)
**Vendored version**: ~2008 (Igor Pavlov, public domain)
**Reason for vendoring**: Custom struct modifications and tightly integrated
CRC implementation used throughout pcompress.

### Custom Modifications

#### `CLzmaEncProps` Struct Extensions

**Files**: `LzmaEnc.h` (lines 52-53), `LzmaEnc.c`

Two fields were added to `CLzmaEncProps`:

```c
int normalized;      /* Tracks whether LzmaEncProps_Normalize() was called */
size_t litprob_sz;   /* Cached literal probability table size */
```

- `normalized` prevents double-normalization of encoding properties.
  `LzmaEncProps_Normalize()` sets this to 1 after adjusting parameters,
  and subsequent calls skip re-normalization if already set.
- `litprob_sz` caches the computed literal probability table size
  (`LITPROB_SZ(lc + lp)`) to avoid recomputation.  This is used by
  pcompress to pre-allocate memory for the probability model.

#### CRC Implementation (crc32_fast.c, crc64_fast.c)

**Files**: `crc32_fast.c`, `crc32_table.c`, `crc64_fast.c`, `crc64_table.c`,
`lzma_crc.h`, associated table headers

The LZMA SDK's CRC implementation is used pervasively by pcompress, not
just for LZMA compression:

- **Data integrity**: `lzma_crc32()` verifies compressed chunk integrity
  in `pcompress.c` and file headers in `meta_stream.c`
- **Crypto nonces**: `lzma_crc64()` generates AES nonces in
  `crypto/aes/crypto_aes.c`
- **Deduplication checksums**: `lzma_crc64()` is used for dedup
  verification in `crypto/crypto_utils.c`
- **Archive format**: libarchive itself calls `lzma_crc32()` in its xz
  filter code

A custom extension `lzma_crc64_8bchk()` adds byte-counting to the CRC64
computation for streaming use cases.

### Migration Path

Replacing the LZMA SDK with a modern version (e.g., LZMA SDK 23.01) would
require:

1. Audit all callers of the CRC functions (listed above) -- there are 20+
   call sites
2. Verify the `CLzmaEncProps` changes are compatible or can be adapted
3. The CRC functions could alternatively be replaced with a standalone CRC
   library, decoupling them from LZMA
4. Test decompression of archives created with the current LZMA parameters

---

## Submodule: LZ4 (lz4/)

**Repository**: https://github.com/lz4/lz4
**Previous vendored version**: ~r90 (2012-era, before API rename)
**Current submodule version**: HEAD (well past v1.10.x)

### API Changes Applied

The old vendored LZ4 used deprecated function names that were renamed in
modern LZ4. Changes in `lz4_compress.c`:

| Old API (deprecated)     | New API                    | Notes                           |
|--------------------------|----------------------------|---------------------------------|
| `LZ4_compress()`         | `LZ4_compress_default()`   | Added `dstCapacity` parameter   |
| `LZ4_compressHC()`       | `LZ4_compress_HC()`        | Added `dstCapacity`, `level`    |
| `LZ4_uncompress()`       | `LZ4_decompress_safe()`    | Bounds-checked, returns decompressed size |

The switch from `LZ4_decompress_fast` (which was unsafe against malformed
input) to `LZ4_decompress_safe` improves security.  The return value
semantics changed: `LZ4_decompress_safe` returns the number of decompressed
bytes (checked against expected size) rather than the number of source bytes
consumed.

**Binary compatibility**: LZ4 compression format has not changed. Archives
created with the old vendored LZ4 can be decompressed by the new version.

### Build Path Change

Source files moved from `lz4/` to `lz4/lib/` to match the upstream
repository layout. Updated in `Makefile.in`:
- `LZ4SRCS`, `LZ4HDRS`: `lz4/` -> `lz4/lib/`
- `BASE_CPPFLAGS` include: `-I./lz4` -> `-I./lz4/lib`

---

## Submodule: libbsc (bsc/)

**Repository**: https://github.com/IlyaGrebnov/libbsc
**Previous vendored version**: 3.1.0
**Current submodule version**: 3.3.12

### API Changes Applied

The core API (`bsc_init`, `bsc_compress`, `bsc_decompress`, `bsc_block_info`)
is unchanged between 3.1.0 and 3.3.12.

**Removed function**: `bsc_decompress_old()` was available in BSC 3.1.0 for
backward compatibility with BSC 2.x compressed data. It is not present in
BSC 3.3.12.  A compatibility shim was added in `libbsc_compress.c`:

```c
#ifndef LIBBSC_HAS_DECOMPRESS_OLD
static inline int
bsc_decompress_old(const unsigned char *input, int inputSize,
    unsigned char *output, int outputSize, int features)
{
    return bsc_decompress(input, inputSize, output, outputSize, features);
}
#endif
```

Modern BSC handles old formats transparently via `bsc_decompress()`.

### Build System Changes

BSC 3.3.12 uses CMake instead of a plain Makefile. Since pcompress uses
Make, BSC source files are compiled directly from `Makefile.in` rather
than delegating to the submodule's build system.

New source files in BSC 3.3.12 vs 3.1.0:
- `libbsc/st/st.cpp` -- Sort Transform support
- `libbsc/bwt/libsais/libsais.c` -- replaces `divsufsort` for BWT

### Binary Compatibility

BSC 3.3.12 can decompress data compressed by BSC 3.1.0. The compression
format is backward compatible.

---

## Submodule: Zstandard (zstd/)

**Repository**: https://github.com/facebook/zstd
**Version**: HEAD (latest)
**New addition**: Zstandard was not present in the original pcompress codebase.

### Integration

Zstandard is integrated as a new compression algorithm (`-a zstd`), providing
a modern alternative that sits between LZ4 (fast, lower ratio) and LZMA
(slow, highest ratio) in terms of performance characteristics.

**Wrapper file**: `zstd_compress.c`

**Level mapping**: Pcompress levels 1-14 are mapped to Zstandard levels 1-22:

| pcompress level | Zstandard level | Profile            |
|-----------------|------------------|--------------------|
| 1-3             | 1-3              | Fast (LZ4-class)   |
| 4-6             | 5-9              | Balanced            |
| 7-9             | 12-16            | High compression    |
| 10-14           | 17-21            | Ultra (LZMA-class)  |

**Multi-threading**: Zstandard's native multi-threading (`ZSTD_c_nbWorkers`)
is used when pcompress is configured with multiple threads.

**Build**: Zstd library sources are compiled directly from `Makefile.in` with
`ZSTD_MULTITHREAD` and `XXH_NAMESPACE=ZSTD_` defined. The `-lpthread` flag
is added to link flags.

**Adaptive mode**: Zstandard is not yet integrated into adaptive compression
mode (`adapt`/`adapt2`). This requires file format changes to the chunk
header encoding and will be addressed in a future update.

---

## Submodule: WavPack (wavpack/)

**Repository**: https://github.com/dbry/WavPack
**Version**: HEAD (latest)
**Usage**: Audio filter for lossless compression of WAV files

### Integration

WavPack is used as a pre-compression filter for audio files in pcompress's
archive mode. When archiving WAV files, the WavPack filter can significantly
reduce size by applying specialized audio compression before the main
compression algorithm.

**Filter integration**: The WavPack filter is invoked from the adaptive
compression pipeline when processing audio data.

**Build**: The config script auto-detects the WavPack submodule. If not found,
it automatically disables WavPack support with a warning. Users can explicitly
specify a WavPack source tree with `--wavpack-dir=/path/to/wavpack` or
disable it with `--disable-wavpack`.

**Optional dependency**: Unlike the core compression algorithms, WavPack is
optional. Pcompress can be built without WavPack support if audio filtering
is not needed.

---

## Initializing Submodules

After cloning the repository, initialize submodules:

```sh
git submodule update --init --recursive
```

The `config` script does this automatically if submodules are not yet
initialized.
