# Building Pcompress with Nix

This document explains how to build and develop pcompress using Nix flakes.

## Prerequisites

- [Nix package manager](https://nixos.org/download.html) with flakes enabled
- Git (for submodules)

### Enable Flakes

Add to `~/.config/nix/nix.conf` or `/etc/nix/nix.conf`:

```
experimental-features = nix-command flakes
```

Or use Nix 2.4+ with `--experimental-features 'nix-command flakes'`

## Quick Start

### Build Pcompress

```bash
# Clone repository
git clone https://github.com/moinakg/pcompress.git
cd pcompress

# Build with Nix (submodules are automatically fetched as flake inputs)
nix build

# Run the built binary
./result/bin/pcompress --help
```

**Note:** The Nix flake automatically fetches all submodule dependencies (lz4, libbsc, zstd, wavpack, libarchive) as flake inputs. You do **not** need to manually initialize git submodules for Nix builds.

### Run Without Installing

```bash
nix run github:moinakg/pcompress -- --help
```

### Enter Development Shell

```bash
nix develop

# Now you have all dependencies available
./config
make
make test
```

## Development Environment

The development shell includes:

**Build Tools:**
- GCC and Clang compilers
- pkg-config, CMake, Make
- Yasm assembler
- autotools (autoconf, automake)

**Libraries:**
- OpenSSL (1.1.1+)
- zlib
- bzip2

**Development Tools:**
- GDB debugger
- Valgrind memory checker
- cppcheck static analyzer
- clang-tools (clang-format, clang-tidy)

**Testing Tools:**
- Standard Unix utilities

### Using direnv (Optional)

For automatic environment activation when entering the directory:

```bash
# Install direnv
nix profile install nixpkgs#direnv

# Hook into your shell (add to ~/.bashrc or ~/.zshrc)
eval "$(direnv hook bash)"    # For bash
eval "$(direnv hook zsh)"     # For zsh

# Allow direnv in the pcompress directory
cd pcompress
direnv allow
```

Now the development environment activates automatically!

## Dependency Management

### Git Submodules

**For Nix Builds:** Submodules are automatically fetched as flake inputs. You do **not** need to manually initialize submodules.

**For Non-Nix Builds:** If building with `./config && make`, initialize submodules first:

```bash
# Initialize all submodules
git submodule update --init --recursive

# Update submodules to latest versions
git submodule update --remote
```

**Submodule Dependencies (automatically handled by Nix):**
- `lz4/` - LZ4 compression (github:lz4/lz4)
- `bsc/` - libbsc block-sorting compression (github:IlyaGrebnov/libbsc)
- `zstd/` - Zstandard compression (github:facebook/zstd)
- `wavpack/` - WavPack audio filter (github:dbry/WavPack)
- `archive/libarchive/` - Archive format support (github:libarchive/libarchive v3.7.9)

### LZMA SDK (Vendored)

**IMPORTANT:** LZMA SDK is **NOT** fetched as a tarball in the Nix build.

The LZMA SDK in `lzma/` is vendored with pcompress-specific modifications:

1. **Custom struct fields** in `CLzmaEncProps` (LzmaEnc.h)
2. **Pervasive CRC usage** - 20+ call sites throughout pcompress
3. **No official git repository** - 7-Zip distributes as tarballs only

See `docs/DEPENDENCIES.md` for the full explanation of why LZMA cannot be
managed as a submodule or fetched dynamically.

The Nix flake verifies the vendored LZMA SDK is present but does not attempt
to fetch or replace it.

## Build Configuration

### Standard Build

```bash
nix build
```

This uses the default configuration:
- Prefix: `$out`
- All submodule dependencies enabled
- System allocator (not jemalloc)
- Zstandard support enabled

### Custom Build Options

To customize the build, modify `flake.nix` in the `preConfigure` section:

```nix
./config \
  --prefix=$out \
  --disable-allocator \
  --enable-zstd \
  --disable-wavpack    # Optional: disable WavPack audio filter
```

Available config options:

```bash
./config --help
```

## Testing

### In Development Shell

```bash
nix develop

# Run all tests
make test-all

# Run specific test categories
make test-unit          # Unit tests
make test-integration   # Integration tests
make bench              # Benchmarks

# Run with sanitizers
make clean
./config --enable-debug
make CFLAGS="-fsanitize=address" test
```

### CI Testing

The flake build runs basic validation. For full CI testing, see
`.github/workflows/ci.yml`.

## Cross-Compilation

### Build for ARM64

```bash
nix build .#packages.aarch64-linux.default --system aarch64-linux
```

### Build for Multiple Architectures

```bash
# Build for current system and ARM64
nix build .# --system x86_64-linux
nix build .# --system aarch64-linux
```

## Packaging for NixOS

To add pcompress to your NixOS configuration:

### In configuration.nix

```nix
{
  environment.systemPackages = [
    (pkgs.callPackage /path/to/pcompress {})
  ];
}
```

### As an Overlay

```nix
# overlays/pcompress.nix
final: prev: {
  pcompress = prev.callPackage /path/to/pcompress/flake.nix {};
}
```

### Submit to nixpkgs

To add pcompress to the official nixpkgs repository:

1. Fork https://github.com/NixOS/nixpkgs
2. Add package to `pkgs/tools/compression/pcompress/default.nix`
3. Submit pull request following nixpkgs guidelines

## Troubleshooting

### Submodule Errors

```
ERROR: Submodule lz4 is not present or empty
```

**Solution:**
```bash
git submodule update --init --recursive
```

### LZMA SDK Missing

```
ERROR: lzma/ directory is missing required vendored files
```

**Solution:** The LZMA SDK must be committed to the repository with its custom
modifications. You cannot build pcompress without the vendored lzma/ directory.

If you're packaging for a distribution, include the lzma/ directory as-is.

### Build Failures

```bash
# Clean build
nix build --rebuild

# Verbose output
nix build -L

# Enter build environment for debugging
nix develop
./config
make clean && make
```

### Flakes Not Enabled

```
error: experimental Nix feature 'flakes' is disabled
```

**Solution:**
```bash
# Temporary
nix build --experimental-features 'nix-command flakes'

# Permanent
echo "experimental-features = nix-command flakes" >> ~/.config/nix/nix.conf
```

## Performance Optimization

### Profile-Guided Optimization (PGO)

```bash
nix develop

# Build with profiling
./config --enable-profiling
make clean && make
./pcompress -c lzma test.dat -o test.pz  # Generate profile data
make pgo                                   # Rebuild with PGO

# Or manually
make clean
./config CFLAGS="-fprofile-generate"
make
./pcompress -c lzma large_file.dat -o /dev/null  # Profile workload
./config CFLAGS="-fprofile-use"
make clean && make
```

### Link-Time Optimization (LTO)

```bash
./config CFLAGS="-flto" LDFLAGS="-flto"
make
```

## Related Documentation

- `docs/DEPENDENCIES.md` - Dependency management and LZMA SDK explanation
- `docs/ARCHITECTURE.md` - System design and component overview
- `docs/PERFORMANCE.md` - Performance tuning guide
- `README.md` - General usage and features
- `.github/workflows/ci.yml` - CI/CD pipeline configuration

## Contributing

When modifying the flake:

1. Test the build: `nix build`
2. Test the dev shell: `nix develop`
3. Check flake: `nix flake check`
4. Format: `nixfmt flake.nix` (optional)

## License

The Nix flake configuration is released under the same license as pcompress
(dual LGPL v3+ / MPL 2.0).
