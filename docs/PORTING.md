# Porting Pcompress to New Architectures

## Overview

Pcompress was originally written for x86-64 Linux and Solaris. This guide
covers what needs to change to support additional architectures such as
ARM64 (AArch64) and RISC-V.

## Architecture-Dependent Components

The following components contain architecture-specific code:

### 1. CPU Feature Detection

Two detection mechanisms are available:

**Legacy** (`utils/cpuid.c`, `utils/cpuid.h`): x86-64 only. Uses CPUID
instruction to detect SSE/AVX/AES-NI. Guarded by `#ifdef __x86_64__`.
Still used by existing SIMD dispatch code (e.g., BLAKE2 `blake2_digest.h`).

**New abstraction** (`utils/cpu_features.h`, `utils/cpu_features.c`):
Platform-agnostic. Provides a unified `cpu_features_t` structure with
architecture-neutral feature flags. Platform-specific backends exist in:
- `utils/cpu_features_x86_64.c` -- wraps CPUID
- `utils/cpu_features_arm64.c` -- uses `getauxval(AT_HWCAP)` on Linux

**What to do for a new architecture**:
- Create `utils/cpu_features_<arch>.c` implementing `cpu_features_detect()`
- Add a new `ARCH_*` enum value to `cpu_arch_t` in `cpu_features.h`
- Map architecture-specific features to the `CPU_FEAT_*` flags
- Update `cpu_features.c` to dispatch to the new backend

### 2. Assembly Files

These files are x86-64 specific and have no portable fallbacks built in:

| File                                    | Purpose                   |
|-----------------------------------------|---------------------------|
| `crypto/aes/aesni-x86_64.s`            | AES-NI encryption         |
| `crypto/aes/vpaes-x86_64.s`            | Vector-permutation AES    |
| `crypto/sha2/intel/sha512_avx.asm`     | SHA-512 AVX               |
| `crypto/sha2/intel/sha512_sse4.asm`    | SHA-512 SSE4              |
| `crypto/skein/skein_block_x64.s`       | Skein block function      |
| `crypto/keccak/KeccakF-1600-x86-64-*.s`| Keccak permutation        |
| `crypto/xsalsa20/stream.s`             | Salsa20 stream            |

**What to do**:
- Ensure portable C fallbacks exist for every assembly-optimized function
- The build system already supports C fallbacks for some (e.g., Skein has
  `skein_block.c` vs `skein_block_x64.s`)
- For new architectures, either use the C fallback or write new optimized
  assembly/intrinsics

### 3. SIMD-Optimized C Files

These use x86 SSE/AVX intrinsics (`<emmintrin.h>`, `<smmintrin.h>`, etc.):

| Component        | Files                               |
|------------------|-------------------------------------|
| BLAKE2           | `crypto/blake2/blake2b_sse2.c`, etc.|
| xxHash           | `utils/xxhash_sse2.c`, etc.         |

**What to do**:
- Create NEON or RVV variants of these files (see `docs/SIMD_OPTIMIZATION.md`)
- Update dispatch logic to select the right variant at runtime

### 4. Byte Order Handling (`utils/utils.h`)

The byte-order macros handle big-endian vs little-endian and use
platform-specific swap functions:

```c
#if defined(__APPLE__)
    #define LE64(x) OSSwapInt64(x)
#elif defined(sun)
    #define LE64(x) BSWAP_64(x)
#else
    #define LE64(x) __bswap_64(x)  // glibc
#endif
```

**What to do**:
- These should work on ARM64 Linux (glibc provides `__bswap_*`)
- For other platforms, add appropriate `#elif` branches
- Verify `htonll`/`ntohll` definitions

### 5. Semaphore Compatibility (`utils/utils.h`)

The `Sem_t` wrapper handles differences between named and unnamed POSIX
semaphores:

```c
typedef struct _compat_sem {
    char name[15];
    sem_t sem, *sem1;
} Sem_t;
```

**What to do**:
- POSIX semaphores are available on most Unix-like systems
- macOS uses named semaphores (already handled)
- ARM64 Linux uses standard unnamed semaphores

### 6. Build System (`config`, `Makefile.in`)

The `config` script detects SSE/AVX capabilities and sets compiler flags.
`Makefile.in` uses `@substitution@` variables for platform-specific settings.

**What to do**:
- Add architecture detection to `config` (`uname -m` or similar)
- Define new `@variables@` for ARM/RISC-V compiler flags
- Conditionally include assembly sources based on architecture
- Set `KECCAK_SRCS` to C-only variants on non-x86

## Porting Checklist: ARM64

- [x] Update `config` to detect `aarch64` architecture
- [x] Skip SSE/AVX detection on non-x86
- [x] Create `utils/cpu_features_arm64.c` backend
- [ ] Set KECCAK sources to `KeccakF-1600-opt64.c` (pure C)
- [ ] Set SKEIN block source to `skein_block.c` (pure C)
- [ ] Set XSalsa20 stream source to `stream.c` (pure C)
- [ ] Disable YASM-dependent targets
- [ ] Skip SHA-2 assembly targets; use `crypto/sha2/sha512.c` only
- [ ] Verify byte-order macros compile
- [ ] Build and run `make test`
- [ ] Optionally: add NEON-optimized BLAKE2 and xxHash variants
- [ ] Optionally: add ARM Crypto Extension AES implementation

## Porting Checklist: RISC-V

- [ ] Same as ARM64 checklist above (use C fallbacks for all assembly)
- [ ] Update `config` to detect `riscv64` architecture
- [ ] Verify `__bswap_64` availability (glibc on RISC-V Linux)
- [ ] Optionally: add RVV-optimized hash implementations if V extension
      is available

## Testing on a New Architecture

1. Build with all assembly disabled (use `--no-sse-detect` or equivalent):
   ```
   ./config --no-sse-detect
   make
   make test
   ```

2. Run the full test suite (`test/t1.tst` through `test/t9.tst`)

3. Verify cross-architecture compatibility: compress on x86-64, decompress
   on the new architecture and vice versa. The `.pz` file format is
   architecture-independent.

4. Benchmark to establish baseline performance numbers.

## Platform-Specific Notes

### Linux
- Primary development platform. Uses `sysinfo()` for memory detection,
  standard pthreads, unnamed semaphores.

### macOS
- Uses named semaphores (unnamed `sem_init` is deprecated)
- Uses `OSByteOrder.h` for byte swapping
- Provides a `clock_gettime` polyfill in `utils.c`

### Solaris/illumos
- Uses `<sys/byteorder.h>` (`BSWAP_*` macros)
- Uses `<atomic.h>` for atomic operations
- `uchar_t` already defined in system headers
