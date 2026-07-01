# Migration Guide: Pcompress v3.x to v4.x

This guide covers changes that affect users and developers when upgrading
from Pcompress 3.x to the modernized 4.x branch.

## Overview of Changes

The v4.x modernization focuses on build system improvements, new platform
support, and additional compression algorithms while maintaining backward
compatibility with the existing `.pz` file format.

## File Format Compatibility

**The `.pz` file format is unchanged.** Files compressed with v3.x can be
decompressed with v4.x and vice versa (assuming the same compression
algorithms are available in both builds).

New compression algorithms added in v4.x (e.g., Zstandard) will only be
available for decompression in v4.x or later.

## Build System Changes

### Dependencies as Git Submodules

Previously bundled libraries (libarchive, LZ4, LZFX, etc.) are now
managed as git submodules. After cloning or pulling:

```sh
git submodule update --init --recursive
```

This replaces the old approach of bundled source copies.

### CMake Support (Planned)

A CMake build system is being added alongside the existing `config`/`make`
system. Both will be supported during the transition.

### OpenSSL Compatibility

v4.x includes a compatibility layer for OpenSSL 1.1.x and 3.x APIs. The
minimum supported OpenSSL version remains 0.9.8, but the build system now
handles API differences automatically.

## New Features

### Zstandard Compression

A new `zstd` algorithm is available:

```sh
pcompress -c zstd -l6 file.tar file.compressed
```

Effective levels: 1-14. Zstandard provides a good balance of speed and
compression ratio, sitting between LZ4 and LZMA.

### ARM64 / AArch64 Support

Pcompress now builds and runs on ARM64 Linux. SIMD-optimized code paths
use portable C fallbacks on non-x86 architectures, with optional NEON
optimizations where available.

### RISC-V Support (Experimental)

Basic support for RISC-V 64-bit Linux is included. All functionality works
using portable C code paths.

### AVX-512 Optimizations

On x86-64 processors with AVX-512 support, additional SIMD-optimized code
paths are available for BLAKE2 and other hash functions.

## API Changes

### Library API (libpcompress)

The public API in `pcompress.h` is backward compatible. New functions
(guarded by `#ifdef ENABLE_PC_ZSTD`):

| Function            | Purpose                          |
|---------------------|----------------------------------|
| `zstd_init`         | Initialize Zstandard context     |
| `zstd_compress`     | Compress a buffer with Zstandard |
| `zstd_decompress`   | Decompress a Zstandard buffer    |
| `zstd_deinit`       | Free Zstandard context           |
| `zstd_props`        | Report Zstandard properties      |
| `zstd_stats`        | Print Zstandard statistics       |

Existing function signatures are unchanged.

### CPU Feature Detection API

A new platform-agnostic CPU feature detection API is available in
`utils/cpu_features.h`:

```c
cpu_features_t feat;
cpu_features_detect(&feat);
if (cpu_has_feature(&feat, CPU_FEAT_AES)) {
    /* Use hardware AES */
}
```

The existing `processor_cap_t` and `cpuid_basic_identify()` API continues
to work on x86-64. The new API is recommended for code that needs to be
portable across architectures.

### Doxygen Documentation

All public headers now include Doxygen comments. Generate HTML documentation:

```sh
cd docs && doxygen Doxyfile
# Output in docs/api/html/
```

## Configuration Changes

### New `config` Options

| Option                    | Description                            |
|---------------------------|----------------------------------------|
| `--with-zstd=<path>`     | Path to Zstandard installation         |
| `--disable-zstd`         | Disable Zstandard support              |
| `--enable-neon`           | Enable ARM NEON optimizations          |
| `--enable-avx512`         | Enable AVX-512 optimizations           |

### `config` Script Improvements

The `config` script has been improved for portability:

- **pkg-config support**: Library detection uses `pkg-config` first, then
  falls back to path scanning. This improves detection on systems with
  non-standard library locations.
- **Multiarch support**: Automatic detection of Debian/Ubuntu multiarch
  triplets (e.g., `x86_64-linux-gnu`) for library paths.
- **macOS Homebrew/MacPorts**: Searches `/opt/homebrew` (Apple Silicon) and
  `/opt/local` (MacPorts) paths.
- **FreeBSD**: Searches `/usr/local/lib`.
- **CC/CXX overrides**: Respects `CC` and `CXX` environment variables for
  compiler selection (e.g., `CC=clang ./config`).
- **Git submodule auto-init**: Automatically runs
  `git submodule update --init --recursive` if submodules are uninitialized.
- **ARM64 detection**: Detects `aarch64`/`arm64` platforms and disables
  x86-specific SSE/AVX probing.
- **Helpful error messages**: Dependency detection failures now suggest
  package installation commands for common distributions.

### Removed Options

None. All existing `config` options continue to work.

## Environment Variable Changes

No changes to existing environment variables. All `PCOMPRESS_*` variables
work as before.

## For Plugin/Extension Developers

If you have written custom compression algorithm wrappers for Pcompress:

1. The algorithm interface (`init`, `compress`, `decompress`, `deinit`,
   `props`, `stats` functions) is unchanged
2. Function pointer typedefs in `utils.h` are unchanged
3. `algo_props_t` structure is unchanged

See `docs/ADDING_ALGORITHMS.md` for the complete integration guide.

## Deprecations

- The `--with-external-libbsc` option is deprecated. Libbsc is now managed
  as a git submodule. The option still works but will be removed in a
  future release.

## Adaptive Mode Changes

Zstandard has been added as an adaptive mode algorithm
(`ADAPT_COMPRESS_ZSTD = 6` in `pcompress.h`). In adaptive modes (`adapt`,
`adapt2`), the analyzer may select Zstandard for medium-entropy data and
executable code sections where it provides a good speed/ratio trade-off
between LZ4 (fast) and LZMA (slow).

The decompressor handles Zstandard chunks transparently via the
`CHDR_ALGO()` macro in the chunk header.

## Known Issues

- ARM64 performance: Without NEON-optimized hash functions, cryptographic
  checksum computation is slower than on x86-64 with SSE/AVX. Use CRC64
  (`-S CRC64`) if data integrity verification speed is critical and
  cryptographic strength is not required.
- RISC-V: Only scalar (non-vectorized) code paths are available. Performance
  on RISC-V is expected to be lower than on x86-64 or ARM64 for hash-heavy
  workloads.

## Reporting Issues

File issues at: https://github.com/moinakg/pcompress/issues
