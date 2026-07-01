# Pcompress Dependencies Explained

## Overview

Pcompress uses a mix of git submodules and vendored code for its dependencies. This document explains the role of each dependency and why LZMA SDK remains vendored while others use submodules.

---

## Git Submodules (5 dependencies)

### 1. **libarchive** (archive/libarchive/)
- **Repository**: https://github.com/libarchive/libarchive
- **Version**: v3.7.9 (updated from v3.1.2 in 2024)
- **Purpose**: PAX archive format support for creating `.pz` archives
- **Custom patches**: Metadata streaming and extended attribute management
- **Why submodule**: Active upstream, well-maintained, clear update path

### 2. **LZ4** (lz4/)
- **Repository**: https://github.com/lz4/lz4
- **Version**: Latest stable
- **Purpose**: Fast compression algorithm (fastest option in pcompress)
- **Why submodule**: Active development, stable API, easy to update

### 3. **libbsc** (bsc/)
- **Repository**: https://github.com/IlyaGrebnov/libbsc
- **Version**: v3.3.12
- **Purpose**: Block-sorting compression (good for text files)
- **Why submodule**: Active maintenance, stable releases

### 4. **Zstandard** (zstd/)
- **Repository**: https://github.com/facebook/zstd
- **Version**: Latest stable
- **Purpose**: Modern compression algorithm (20-30% better ratio than LZ4 at similar speed)
- **Why submodule**: Industry standard, actively developed by Meta/Facebook
- **Added**: Part of 2024 modernization effort

### 5. **WavPack** (wavpack/)
- **Repository**: https://github.com/dbry/WavPack
- **Version**: Latest stable
- **Purpose**: Audio filter for lossless WAV file compression
- **Why submodule**: Active upstream, clear git history, easy updates
- **Optional**: Can be disabled with `--disable-wavpack`
- **Added**: Part of 2024 modernization effort

---

## Vendored Code (1 dependency)

### **LZMA SDK** (lzma/)
- **Upstream**: 7-Zip LZMA SDK (https://7-zip.org/sdk.html)
- **Version**: ~2008 (Igor Pavlov, public domain)
- **Purpose**: LZMA compression algorithm and CRC functions

#### Why LZMA SDK Cannot Be a Submodule

**1. No Official Git Repository**
- 7-Zip project distributes LZMA SDK as **tarballs only**
- No official GitHub repository exists
- Would require creating a custom fork with no clear upstream to track
- Maintaining a fork would be more work than vendoring

**2. Deep Custom Modifications**

The vendored LZMA SDK has extensive pcompress-specific modifications that would need to be rebased on every upstream update:

**a) Custom Struct Fields in `CLzmaEncProps` (LzmaEnc.h)**
```c
int normalized;      /* Prevents double-normalization */
size_t litprob_sz;   /* Cached literal probability table size */
```
These fields are used throughout pcompress for:
- Memory pre-allocation optimization
- Preventing redundant parameter normalization
- Performance tuning

**b) Pervasive CRC Implementation Usage**

The LZMA SDK's CRC functions are used in **20+ call sites** across pcompress, NOT just for LZMA compression:

| Use Case | Function | Files |
|----------|----------|-------|
| Data integrity | `lzma_crc32()` | pcompress.c, meta_stream.c |
| Crypto nonces | `lzma_crc64()` | crypto/aes/crypto_aes.c |
| Dedup checksums | `lzma_crc64()` | crypto/crypto_utils.c |
| Archive format | `lzma_crc32()` | Used by libarchive xz filter |

**Custom extensions**:
- `lzma_crc64_8bchk()` - Adds byte-counting for streaming use

**3. Tight Integration**

Extracting LZMA SDK would require:
- Auditing 20+ CRC call sites
- Replacing CRC functions with a standalone library
- Verifying `CLzmaEncProps` changes are compatible
- Testing decompression of old archives
- Significant refactoring with unclear benefit

#### Migration Path (If Ever Needed)

If updating LZMA SDK becomes necessary:

1. **Create a pcompress-specific fork**
   - Fork LZMA SDK tarball releases to GitHub
   - Apply custom patches to each release
   - Maintain as a git repository

2. **Decouple CRC functions**
   - Extract CRC implementation to standalone library
   - Replace 20+ call sites
   - Use a maintained CRC library (e.g., zlib's crc32)

3. **Test compatibility**
   - Verify old archives decompress correctly
   - Benchmark performance impact
   - Test multi-threaded operation

**Current Decision**: Keep LZMA SDK vendored. The ~2008 version is stable, well-tested, and the custom modifications work reliably. The effort to modernize outweighs the benefits.

---

## Dependency Initialization

After cloning the repository, initialize submodules:

```bash
git submodule update --init --recursive
```

The `config` script automatically initializes submodules if they're not present.

---

## Build Configuration

### Auto-Detection

The config script automatically detects submodules:

```bash
./config
```

Submodules are detected and enabled automatically. If a submodule is missing:
- **Critical dependencies** (libarchive, LZ4, libbsc): Build fails with helpful error
- **Optional dependencies** (WavPack, Zstandard): Auto-disabled with warning

### Manual Control

Explicitly disable optional features:

```bash
./config --disable-wavpack --disable-zstd
```

Specify custom paths (rarely needed):

```bash
./config --wavpack-dir=/path/to/custom/wavpack
```

---

## Summary

| Dependency | Type | Reason |
|------------|------|--------|
| libarchive | Submodule | Active upstream, clear API |
| LZ4 | Submodule | Active development, stable |
| libbsc | Submodule | Maintained, versioned releases |
| Zstandard | Submodule | Industry standard, Meta-backed |
| WavPack | Submodule | Active upstream, optional feature |
| **LZMA SDK** | **Vendored** | **No git upstream, deep custom mods, pervasive CRC usage** |

The submodule approach provides easy updates and upstream tracking for 5 out of 6 dependencies. LZMA SDK remains vendored due to its unique circumstances - no official repository, extensive custom modifications, and tightly integrated CRC implementation used throughout pcompress.
