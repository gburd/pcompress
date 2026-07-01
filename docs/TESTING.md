# Pcompress Testing Guide

## Overview

Pcompress uses a layered testing strategy with four test categories: unit
tests, integration tests, legacy tests, and fuzz tests. A performance
benchmarking suite is also available. All tests are coordinated by the
master runner script `test/run_all_tests.sh`.

## Test Categories

| Category      | Location                     | Type     | Purpose                          |
|---------------|------------------------------|----------|----------------------------------|
| Unit          | `test/unit/`                 | C        | Isolated function-level tests    |
| Integration   | `test/integration/`          | Shell    | End-to-end round-trip tests      |
| Legacy        | `test/t1.tst` - `test/t9.tst`| Shell   | Original test suite              |
| Fuzz          | `test/fuzz/`                 | C        | Crash/UB detection via fuzzing   |
| Performance   | `test/performance/`          | C        | Throughput and ratio benchmarks  |

## Quick Start

### Running All Tests

```sh
cd test
sh run_all_tests.sh
```

This runs unit tests and integration tests by default (the `all` category).

### Running Specific Categories

```sh
sh run_all_tests.sh unit          # Unit tests only
sh run_all_tests.sh integration   # Integration tests only
sh run_all_tests.sh legacy        # Original t1-t9 test suite
sh run_all_tests.sh bench         # Performance benchmarks
sh run_all_tests.sh fuzz          # Fuzz smoke test (60s per target)
sh run_all_tests.sh full          # Everything including legacy and benchmarks
```

## Unit Tests

Unit tests are C programs that test individual functions in isolation. They
use a minimal test framework (`test/unit/minunit.h`).

### Available Unit Tests

| Test Binary     | Source                      | Tests                              |
|-----------------|-----------------------------|------------------------------------|
| `test_compress` | `test/unit/test_compress.c` | Algorithm round-trip correctness   |
| `test_xxhash`   | `test/unit/test_xxhash.c`  | xxHash non-cryptographic hash      |
| `test_crc`      | `test/unit/test_crc.c`     | CRC32/CRC64 correctness           |
| `test_slab`     | `test/unit/test_slab.c`    | Slab allocator alloc/free cycles   |
| `test_blake2`   | `test/unit/test_blake2.c`  | BLAKE2b hash correctness           |

### Building Unit Tests

```sh
make test-unit-build
```

This compiles all unit test binaries into `buildtmp/`.

### Running Unit Tests

```sh
# All unit tests
sh test/run_all_tests.sh unit

# Individual test
LD_LIBRARY_PATH=. ./buildtmp/test_compress
```

### Writing New Unit Tests

1. Create a new file in `test/unit/`, for example `test_myfeature.c`.
2. Include the minunit header:
   ```c
   #include "minunit.h"
   ```
3. Write test functions using `mu_assert` and `mu_run_test`:
   ```c
   static int tests_run = 0;

   static char *
   test_something(void)
   {
       int result = my_function(42);
       mu_assert("expected 100", result == 100);
       return NULL;
   }

   static char *
   all_tests(void)
   {
       mu_run_test(test_something);
       return NULL;
   }

   int
   main(void)
   {
       char *result = all_tests();
       if (result != NULL) {
           printf("FAIL: %s\n", result);
           return 1;
       }
       printf("ALL TESTS PASSED (%d tests)\n", tests_run);
       return 0;
   }
   ```
4. Add the build rule to `Makefile.in` (see existing test targets).
5. Add the binary to `run_all_tests.sh` under `run_unit_tests()`.

## Integration Tests

Integration tests exercise the `pcompress` binary end-to-end. They are
POSIX shell scripts located in `test/integration/`.

### Available Integration Tests

| Script                  | Tests                                           |
|-------------------------|-------------------------------------------------|
| `test_compress.sh`      | Round-trip for all algorithms, levels, chunk sizes |
| `test_archive.sh`       | Archive mode (`-a`) with directories            |
| `test_edge_cases.sh`    | Empty files, single byte, invalid params, corruption |
| `test_crypto.sh`        | Encryption (`-e AES`, `-e SALSA20`)             |
| `test_dedup.sh`         | Deduplication modes (`-D`, `-F`, `-G`)          |
| `test_multithread.sh`   | Multi-threaded compression and decompression    |

### Running Integration Tests

```sh
sh test/run_all_tests.sh integration

# Or run one test directly
cd test
sh integration/test_compress.sh
```

### Test Data

Integration tests generate synthetic test data in `test/datafiles/`:

- `text.dat` -- Repeating text (highly compressible)
- `random.dat` -- Pseudo-random bytes (incompressible)
- `zeros.dat` -- All zeros
- `mixed.dat` -- Concatenation of above (mixed data types)

Tests create these files automatically if they do not exist.

### Writing New Integration Tests

1. Create `test/integration/test_myfeature.sh`.
2. Follow the pattern from existing scripts:
   ```sh
   #!/bin/sh
   PCOMPRESS="../../pcompress"
   PASS=0; FAIL=0; SKIP=0

   log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
   log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
   log_skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1"; }

   # ... test logic ...

   echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
   [ $FAIL -gt 0 ] && exit 1
   exit 0
   ```
3. The master runner `run_all_tests.sh` automatically discovers
   `integration/test_*.sh` scripts via glob.

### LD_LIBRARY_PATH Note

The `pcompress` wrapper script at the repository root sets
`LD_LIBRARY_PATH=.` so that `libpcompress.so` is found at runtime. If
running tests from a different working directory, ensure the library path
is set:

```sh
LD_LIBRARY_PATH=/path/to/pcompress ./buildtmp/pcompress ...
```

## Legacy Test Suite

The original test suite consists of test definition files `t1.tst` through
`t9.tst`, executed by `test/run_test.sh`. These test various compression
algorithms, levels, deduplication modes, encryption, and archive operations.

### Running Legacy Tests

```sh
sh test/run_all_tests.sh legacy
```

### Known Issues

- **Sandbox constraints**: Tests that write to `/tmp` (e.g., crypto tests
  writing password files to `/tmp/pwf`) will fail in sandboxed environments.
- **Stale files**: Previous `.pz` / `.1` output files can cause cascading
  failures. Run `make clean-test` or manually remove stale output before
  re-running.
- **Algorithm detection**: The `algo_available()` function in integration
  test scripts checks help text output, which does not list algorithm names.
  This causes false "not compiled in" reports. A fix is to probe by
  attempting a small compression instead.

## Fuzz Testing

Fuzz tests detect crashes, undefined behavior, and memory errors when
processing malformed input. Three fuzz harnesses are provided.

### Available Fuzz Targets

| Target              | Source                       | Tests                      |
|---------------------|------------------------------|----------------------------|
| `fuzz_decompress`   | `test/fuzz/fuzz_decompress.c`| Decompression of random data|
| `fuzz_compress`     | `test/fuzz/fuzz_compress.c`  | Compression of random data  |
| `fuzz_crc`          | `test/fuzz/fuzz_crc.c`       | CRC functions with random input |

### Building Fuzz Targets

**With libFuzzer (Clang)**:

```sh
make fuzz-build CC=clang FUZZ_FLAGS="-fsanitize=fuzzer,address"
```

**With AFL++**:

```sh
make fuzz-build CC=afl-gcc CPPFLAGS="-DFUZZ_AFL"
```

### Running Fuzz Tests

**Quick smoke test** (60 seconds per target):

```sh
sh test/run_all_tests.sh fuzz
```

**Extended fuzzing** (e.g., 1 hour per target):

```sh
FUZZ_SECONDS=3600 sh test/run_all_tests.sh fuzz
```

**Manual libFuzzer run**:

```sh
LD_LIBRARY_PATH=. ./buildtmp/fuzz_decompress \
    -max_total_time=300 \
    -print_final_stats=1 \
    corpus/decompress/
```

### Corpus Management

- Start with an empty corpus directory; the fuzzer will generate inputs.
- Seed the corpus with small valid `.pz` files for better coverage of the
  decompression path.
- Save interesting inputs (crashes, timeouts) for regression testing.

## Performance Benchmarks

The `bench_algos` program measures compression throughput (MB/s) and
compression ratio for each algorithm at representative levels.

### Building Benchmarks

```sh
make bench-build
```

### Running Benchmarks

```sh
LD_LIBRARY_PATH=. ./buildtmp/bench_algos [iterations]
```

Default iteration count is 5. Output is a tab-separated table suitable for
import into a spreadsheet or comparison tool.

### Data Types Tested

Benchmarks run on two synthetic patterns:
- **Text**: Repeating sentence (highly compressible)
- **Random**: Pseudo-random bytes (incompressible baseline)

For representative real-world results, benchmark with the Silesia corpus
(200MB mixed data). Place the corpus files in `test/datafiles/` and update
`bench_algos.c` to read them.

## Test Infrastructure

### Master Runner (`test/run_all_tests.sh`)

Dispatches to all test categories and produces a summary report:

```
=================================================================
 SUMMARY
=================================================================
 Sections passed:  12
 Sections failed:  0
 Sections skipped: 5
=================================================================
```

Exit code is 0 if all sections pass, 1 if any section fails.

### Environment Requirements

- POSIX-compliant `/bin/sh`
- `diff` for binary comparison
- `dd` for test data generation
- `wc` for size checks
- `pcompress` binary built and accessible (via wrapper script or PATH)
- `LD_LIBRARY_PATH` set to find `libpcompress.so`

### CI/CD Integration

The test suite is designed for use in CI pipelines (GitHub Actions,
GitLab CI, etc.). Recommended CI steps:

```yaml
# Example GitHub Actions workflow snippet
steps:
  - name: Build
    run: |
      git submodule update --init --recursive
      ./config
      make -j$(nproc)

  - name: Unit Tests
    run: |
      make test-unit-build
      cd test && sh run_all_tests.sh unit

  - name: Integration Tests
    run: cd test && sh run_all_tests.sh integration

  - name: Fuzz Smoke Test
    run: |
      make fuzz-build CC=clang FUZZ_FLAGS="-fsanitize=fuzzer,address"
      FUZZ_SECONDS=120 cd test && sh run_all_tests.sh fuzz
```

### Adding Tests to CI

When adding new test categories:

1. Add a new function to `run_all_tests.sh` following the existing pattern.
2. Add the category to the `case` dispatch and `all`/`full` targets as
   appropriate.
3. Ensure the test produces a clear PASS/FAIL exit code.

## Debugging Test Failures

### Common Issues

1. **"not compiled in"**: The `algo_available()` check in integration
   scripts parses help text. If an algorithm works but is not listed in
   help, it will be skipped. Verify manually:
   ```sh
   echo "test" | LD_LIBRARY_PATH=. ./buildtmp/pcompress -c zstd -l 1 -p - > /dev/null 2>&1
   echo $?  # 0 = available
   ```

2. **Decompression failure with no error**: Check `LD_LIBRARY_PATH` is
   set. If `libpcompress.so` is not found, the binary may exit silently.

3. **Stale test artifacts**: Remove `*.pz`, `*.out`, and `*.1` files from
   test directories before re-running.

4. **Permission errors**: The legacy test suite writes to `/tmp`. In
   sandboxed environments, set `TMPDIR` to a writable location.

### Verbose Mode

Run individual integration tests with `set -x` for shell tracing:

```sh
sh -x integration/test_compress.sh
```

For the pcompress binary, add `-v` (verbose) and `-C` (compression stats).

### Valgrind

Run unit tests under Valgrind to detect memory errors:

```sh
LD_LIBRARY_PATH=. valgrind --leak-check=full ./buildtmp/test_compress
```

### AddressSanitizer

Build with ASan for runtime memory error detection:

```sh
make clean
./config
make CFLAGS="-fsanitize=address -g" LDFLAGS="-fsanitize=address"
cd test && sh run_all_tests.sh all
```

## Test Matrix

For comprehensive testing, exercise these combinations:

| Dimension           | Values                                          |
|---------------------|-------------------------------------------------|
| Algorithm           | lz4, lzfx, zlib, bzip2, lzma, ppmd, zstd, libbsc, none |
| Level               | 1, 6, 14                                       |
| Chunk size          | 64k, 1m, 8m                                    |
| Data type           | text, random, zeros, mixed                      |
| Mode                | single-file, archive (`-a`), pipe (`-p`)        |
| Dedup               | none, `-D`, `-F`, `-G`                          |
| Encryption          | none, AES, SALSA20                              |
| Checksum            | BLAKE256, SHA256, CRC64                         |
| Threads             | 1, 2, system cores                              |

The integration test suite covers a subset of this matrix. Extend coverage
by adding new test scripts for untested combinations.
