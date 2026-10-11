{
  description = "Pcompress - Parallel compression and archiving utility with deduplication";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        # Same expression as the nixpkgs package, built from this tree.
        # Bundled sources (lz4, libbsc, zstd, libarchive) are pinned in
        # nix/package.nix to the git submodule revisions.
        pcompress = pkgs.callPackage ./nix/package.nix {
          src = pkgs.lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              let b = baseNameOf path; in
              !(pkgs.lib.hasSuffix ".o" b || pkgs.lib.hasSuffix ".so" b
                || pkgs.lib.hasSuffix ".a" b || b == "buildtmp" || b == "result");
          };
        };
      in {
        packages.default = pcompress;
        packages.pcompress = pcompress;
        packages.pcompress-clang = pcompress.override { stdenv = pkgs.clangStdenv; };

        checks.default = pcompress;

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
            llvmPackages.openmp  # libomp for clang -fopenmp
            llvmPackages.bintools  # llvm-ar + lld for clang LTO builds
            lcov  # coverage reports (make coverage)
            mandoc  # man page lint (mandoc -Tlint pcompress.1)

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

          # Quiet: shell is also used non-interactively (nix develop --command).
          shellHook = ''
            [ -t 1 ] && echo "pcompress dev shell: ./config && make && make test-unit"
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
