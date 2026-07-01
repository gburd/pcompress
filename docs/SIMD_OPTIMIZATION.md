# SIMD Optimization in Pcompress

## Overview

Pcompress uses SIMD (Single Instruction, Multiple Data) vector instructions
extensively to accelerate cryptographic hashing, non-cryptographic hashing, and
encryption operations. The current implementation targets x86-64 with SSE2
through AVX instruction sets.

## Current SIMD Usage

### Cryptographic Hashes

| Component   | Location                     | ISA Variants                    |
|-------------|------------------------------|---------------------------------|
| BLAKE2b     | `crypto/blake2/blake2b_*.c`  | SSE2, SSSE3, SSE4.1, AVX       |
| BLAKE2bp    | `crypto/blake2/blake2bp_*.c` | SSE2, SSSE3, SSE4.1, AVX       |
| SHA-512     | `crypto/sha2/intel/`         | SSE4, AVX (YASM assembly)       |
| Keccak      | `crypto/keccak/`             | x86-64 assembly (opt64)         |
| Skein       | `crypto/skein/`              | x86-64 assembly block function  |

### Non-Cryptographic Hashes

| Component   | Location                     | ISA Variants                    |
|-------------|------------------------------|---------------------------------|
| xxHash      | `utils/xxhash_sse2.c`        | SSE2                            |
| xxHash      | `utils/xxhash_sse4.c`        | SSE4.2                          |

### Encryption

| Component   | Location                     | ISA Variants                    |
|-------------|------------------------------|---------------------------------|
| AES         | `crypto/aes/aesni-x86_64.s`  | AES-NI                          |
| AES         | `crypto/aes/vpaes-x86_64.s`  | SSSE3 (vector permutation AES)  |
| XSalsa20    | `crypto/xsalsa20/stream.s`   | x86-64 assembly (optional)      |

## Runtime CPU Detection

### Legacy x86-only Detection (`utils/cpuid.c`)

CPU capabilities are detected at startup via `utils/cpuid.c`. The
`processor_cap_t` structure stores:

```c
typedef struct {
    int sse_level;       // 0=none, 2=SSE2, 3=SSE3, 4=SSE4 ...
    int sse_sub_level;   // Sub-level (e.g., SSE4.1 vs SSE4.2)
    int avx_level;       // 0=none, 1=AVX, 2=AVX2
    int avx512_avail;    // AVX-512 foundation available
    int xop_avail;       // AMD XOP available
    int aes_avail;       // AES-NI available
    proc_type_t proc_type; // Intel, AMD, generic
} processor_cap_t;
```

The global `proc_info` variable (declared in `utils.h`) is populated by
`cpuid_basic_identify()` during initialization and used for dispatch decisions
throughout the codebase.

### Platform-Agnostic Abstraction (`utils/cpu_features.h`)

A newer abstraction layer provides a unified interface across architectures:

```c
typedef struct {
    cpu_arch_t arch;     // ARCH_X86_64, ARCH_ARM64, or ARCH_UNKNOWN
    uint32_t features;   // Bitmask of CPU_FEAT_* flags
} cpu_features_t;

void cpu_features_detect(cpu_features_t *feat);
int cpu_has_feature(const cpu_features_t *feat, uint32_t flag);
```

Feature flags are architecture-neutral:

| Flag                | x86-64 Mapping | ARM64 Mapping            |
|---------------------|----------------|--------------------------|
| `CPU_FEAT_SIMD_BASE`| SSE2           | NEON (always on AArch64) |
| `CPU_FEAT_SIMD_EXT1`| SSSE3          | (unused)                 |
| `CPU_FEAT_SIMD_EXT2`| SSE4.1         | (unused)                 |
| `CPU_FEAT_SIMD_EXT3`| SSE4.2         | (unused)                 |
| `CPU_FEAT_SIMD_WIDE`| AVX            | (unused)                 |
| `CPU_FEAT_SIMD_WIDE2`| AVX2          | (unused)                 |
| `CPU_FEAT_AES`      | AES-NI         | CE AES                   |
| `CPU_FEAT_SHA`      | SHA-NI         | CE SHA                   |
| `CPU_FEAT_CRC32`    | SSE4.2 CRC     | CE CRC32                 |
| `CPU_FEAT_NEON`     | (not set)      | Always set on AArch64    |

Detection backends:
- `utils/cpu_features_x86_64.c` -- wraps existing CPUID logic
- `utils/cpu_features_arm64.c` -- uses `getauxval(AT_HWCAP)` on Linux

## Compilation Strategy

SIMD-specific source files are compiled with the appropriate ISA flags:

```makefile
# From Makefile.in
AVX_OPT_FLAG  = -mavx
SSE4_OPT_FLAG = -msse4.2
SSE3_OPT_FLAG = -mssse3
SSE2_OPT_FLAG = -msse2

# BLAKE2 compiled separately for each ISA level
$(COMPILE) $(BASE_OPT) $(SSE2_OPT_FLAG) ... blake2b_sse2.c
$(COMPILE) $(BASE_OPT) $(SSE3_OPT_FLAG) ... blake2b_ssse3.c
$(COMPILE) $(BASE_OPT) $(SSE4_OPT_FLAG) ... blake2b_sse41.c
$(COMPILE) $(BASE_OPT) $(AVX_OPT_FLAG)  ... blake2b_avx.c
```

This ensures each variant uses only the instructions available at its ISA
level while the rest of the code remains portable.

Assembly files (`.s` and `.asm`) are assembled separately:
- GAS syntax `.s` files: assembled by GCC or a GAS-compatible assembler
- NASM/YASM `.asm` files: assembled by YASM (`crypto/sha2/intel/`)

## Dispatch Patterns

### Pattern 1: Compile-Time Selection (Assembly)

For AES, the `config` script detects AES-NI support and selects between
`aesni-x86_64.s` (hardware AES) and `vpaes-x86_64.s` (vector-permutation
software AES using SSSE3):

```
config script --> detects AES-NI --> sets @CRYPTO_ASM_COMPILE@
```

### Pattern 2: Separate Compilation, Link-Time Selection (BLAKE2)

All BLAKE2 ISA variants are compiled into separate object files. The base
`blake2b.c` / `blake2bp.c` contain dispatch logic that selects the
appropriate implementation at runtime based on `proc_info`.

### Pattern 3: Source-Level Dispatch (xxHash)

`xxhash.c` contains the base implementation. `xxhash_sse2.c` and
`xxhash_sse4.c` include `xxhash.c` with different compiler flags, creating
ISA-specific versions. The `xxhash_base.c` wrapper provides the public API
that dispatches to the best available variant.

## Adding SIMD Support for a New Architecture

### ARM64/NEON

To port SIMD optimizations to ARM64 with NEON:

1. **Create architecture-specific source variants**:
   ```
   crypto/blake2/blake2b_neon.c
   utils/xxhash_neon.c
   ```

2. **Update `cpuid.c`**:
   Add ARM64 feature detection (read `/proc/cpuinfo` or use `getauxval` with
   `AT_HWCAP`/`AT_HWCAP2` on Linux). Extend `processor_cap_t`:
   ```c
   typedef struct {
       /* ... existing x86 fields ... */
       int neon_avail;     // NEON available
       int sve_avail;      // SVE available
       int sve2_avail;     // SVE2 available
       int crypto_avail;   // ARM crypto extensions
       proc_type_t proc_type;
   } processor_cap_t;
   ```

3. **Map x86 intrinsics to NEON**:
   Common mappings:
   | x86 SSE2                | ARM NEON                    |
   |-------------------------|-----------------------------|
   | `__m128i`               | `uint32x4_t` / `uint64x2_t`|
   | `_mm_loadu_si128`       | `vld1q_u32`                 |
   | `_mm_storeu_si128`      | `vst1q_u32`                 |
   | `_mm_add_epi32`         | `vaddq_u32`                 |
   | `_mm_xor_si128`         | `veorq_u32`                 |
   | `_mm_shuffle_epi8`      | `vqtbl1q_u8` (SSSE3 equiv) |
   | `_mm_srli_epi64`        | `vshrq_n_u64`               |

4. **Update `Makefile.in`**:
   Add NEON compilation flags and conditional inclusion:
   ```makefile
   NEON_OPT_FLAG = -march=armv8-a+simd
   ```

5. **Update `config` script**:
   Detect ARM64 platform and set appropriate flags.

### RISC-V Vector Extension

For RISC-V with the V (Vector) extension:

1. Create `*_rvv.c` source variants using RVV intrinsics
2. Detect vector support via `__riscv_vector` compiler macro or runtime
   probing
3. Compile with `-march=rv64gcv` or equivalent

## Best Practices

1. **Always provide a scalar fallback**: Every SIMD-optimized function must
   have a portable C implementation that works on any architecture.

2. **Separate ISA levels into separate files**: Do not use `#ifdef` to mix
   SSE2 and AVX code in the same file. Compile each variant separately with
   appropriate flags.

3. **Test on minimum ISA**: Ensure the base build works with no SIMD
   (`--no-sse-detect` flag).

4. **Benchmark before committing**: SIMD is not always faster for small
   buffers due to setup overhead. Benchmark at realistic chunk sizes.

5. **Align buffers**: Use 16-byte (SSE) or 32-byte (AVX) aligned buffers
   where possible. The slab allocator returns naturally aligned allocations.

6. **Document ISA requirements**: Each SIMD source file should document the
   minimum ISA requirement in a comment at the top.
