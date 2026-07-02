{
  description = "Pcompress - Parallel compression and archiving utility with deduplication";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Submodule dependencies as flake inputs
    # This allows Nix to fetch them directly without requiring
    # git submodule initialization in the source tree
    lz4-src = {
      url = "github:lz4/lz4";
      flake = false;
    };
    libbsc-src = {
      url = "github:IlyaGrebnov/libbsc";
      flake = false;
    };
    zstd-src = {
      url = "github:facebook/zstd";
      flake = false;
    };
    wavpack-src = {
      url = "github:dbry/WavPack";
      flake = false;
    };
    libarchive-src = {
      url = "github:libarchive/libarchive/v3.7.9";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, lz4-src, libbsc-src, zstd-src, wavpack-src, libarchive-src }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

      in {
        packages.default = pkgs.stdenv.mkDerivation rec {
          pname = "pcompress";
          version = "4.0.0";

          # NOTE: This flake requires git submodules to be initialized
          # Run: git submodule update --init --recursive
          # before building with: nix build
          src = ./.;

          nativeBuildInputs = with pkgs; [
            pkg-config
            yasm
            which
            git
            patchelf
            # For libarchive submodule build
            autoconf
            automake
            libtool
            perl
          ];

          buildInputs = with pkgs; [
            openssl
            zlib
            bzip2
          ];

          # Verify vendored LZMA SDK and set up submodules from flake inputs
          preConfigure = ''
            # Verify LZMA SDK is present (vendored with custom modifications)
            # See docs/DEPENDENCIES.md for why LZMA cannot be fetched as a tarball
            if [ ! -f lzma/LzmaEnc.h ]; then
              echo "ERROR: lzma/ directory is missing required vendored files"
              echo "The LZMA SDK in pcompress has custom modifications (CLzmaEncProps"
              echo "struct extensions and pervasive CRC usage) and must be vendored."
              echo "See docs/DEPENDENCIES.md for details."
              exit 1
            fi
            echo "Using vendored LZMA SDK with pcompress-specific modifications"

            # Copy submodules from flake inputs
            echo "Setting up submodules from flake inputs..."

            # Remove any empty submodule directories and copy from inputs
            rm -rf lz4 bsc zstd wavpack archive/libarchive

            mkdir -p archive
            cp -r ${lz4-src} lz4
            cp -r ${libbsc-src} bsc
            cp -r ${zstd-src} zstd
            cp -r ${wavpack-src} wavpack
            cp -r ${libarchive-src} archive/libarchive

            # Make directories writable
            chmod -R +w lz4 bsc zstd wavpack archive/libarchive

            echo "Submodules successfully populated from flake inputs"

            # Build libarchive from submodule
            echo "Configuring libarchive submodule..."
            pushd archive/libarchive

            # Run autogen if configure doesn't exist
            if [ ! -f configure ]; then
              echo "Running autogen.sh for libarchive..."
              if [ -f build/autogen.sh ]; then
                ./build/autogen.sh
              elif [ -f autogen.sh ]; then
                ./autogen.sh
              else
                echo "Running autoreconf directly..."
                autoreconf -fiv
              fi
            fi

            # Apply pcompress patches if needed
            if [ -f ../patches/pcompress_libarchive.patch ]; then
              if ! grep -q "cb_is_metadata" libarchive/archive_private.h 2>/dev/null; then
                echo "Applying pcompress patches to libarchive..."
                patch -p1 < ../patches/pcompress_libarchive.patch || true
              else
                echo "Patches already applied to libarchive"
              fi
            fi
            popd

            # Build WavPack from submodule
            echo "Configuring WavPack submodule..."
            pushd wavpack

            # Run autogen if configure doesn't exist
            if [ ! -f configure ]; then
              echo "Running autogen.sh for WavPack..."
              if [ -f autogen.sh ]; then
                ./autogen.sh
              else
                echo "Running autoreconf directly..."
                autoreconf -fiv
              fi
            fi
            popd

            # Run pcompress configuration script
            # The config script now checks PATH first, so yasm will be found automatically
            echo "Running pcompress configuration..."
            bash ./config \
              --prefix=$out \
              --disable-allocator
            # Note: Zstandard is enabled by default, no flag needed
          '';

          buildPhase = ''
            make -j$NIX_BUILD_CORES
          '';

          installPhase = ''
            runHook preInstall

            # 'make install' installs the real ELF binary (buildtmp/pcompress),
            # the shared library, and README to $PREFIX (set to $out via config).
            make install

            # The binary links against libpcompress.so.1 with no rpath to the
            # install location; add $out/lib to its rpath (preserving the
            # existing entries for bzip2/zlib/openssl/etc.) so it resolves the
            # library without needing LD_LIBRARY_PATH or the dev wrapper.
            # 'make install' sets mode 0555, so make it writable for patchelf.
            chmod u+w "$out/bin/pcompress"
            patchelf --add-rpath "$out/lib" "$out/bin/pcompress"
            chmod 0555 "$out/bin/pcompress"

            # Additional documentation (license + docs/) beyond the README that
            # 'make install' already places in share/doc/pcompress.
            install -Dm644 COPYING $out/share/doc/pcompress/COPYING
            install -Dm644 COPYING.LESSER $out/share/doc/pcompress/COPYING.LESSER
            for f in docs/*.md; do
              install -Dm644 "$f" "$out/share/doc/pcompress/$(basename "$f")"
            done

            # Install man page if it exists
            if [ -f pcompress.1 ]; then
              install -Dm644 pcompress.1 $out/share/man/man1/pcompress.1
            fi

            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "Parallel compression utility with multiple algorithms, deduplication, and encryption";
            longDescription = ''
              Pcompress is an advanced archiver that performs compression and decompression
              in parallel by splitting input data into chunks. Features include:

              - Multiple compression algorithms: LZMA, Bzip2, PPMD, LZ4, Zstandard, and more
              - Variable block deduplication with polynomial fingerprinting
              - Delta compression via bsdiff
              - AES and Salsa20 encryption with Scrypt key derivation
              - SIMD optimizations (SSE/AVX on x86-64, NEON on ARM64)
              - Archive mode with PAX format support
              - Adaptive compression based on file type analysis
            '';
            homepage = "https://github.com/moinakg/pcompress";
            license = with licenses; [ lgpl3Plus mpl20 ];
            maintainers = [ ];
            platforms = platforms.unix;
            mainProgram = "pcompress";
          };
        };

        # Development shell with all dependencies
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            # Build tools
            pkg-config
            yasm
            autoreconfHook
            cmake
            gnumake
            gcc
            clang

            # Libraries
            openssl
            zlib
            bzip2

            # Development tools
            git
            gdb
            valgrind
            cppcheck
            clang-tools

            # Testing tools
            which
            file
          ];

          shellHook = ''
            echo "Pcompress development environment"
            echo ""
            echo "Available commands:"
            echo "  ./config           - Configure build"
            echo "  make              - Build pcompress"
            echo "  make test         - Run tests"
            echo "  make bench        - Run benchmarks"
            echo ""
            echo "Git submodules:"
            echo "  git submodule update --init --recursive"
            echo ""
            echo "LZMA SDK: Using vendored version with custom modifications"
            echo "See docs/DEPENDENCIES.md for details on why LZMA is vendored."
          '';
        };

        # Apps
        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/pcompress";
        };
      }
    );
}
