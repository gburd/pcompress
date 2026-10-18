# Platforms

The targets pcompress must support, what each one is tested on today, and
what has to pass before a release is tagged. A cell is only "gated" if a
failure there stops the release.

## Target matrix

| OS | Family | x86_64 | aarch64 | riscv64 | sparcv9 |
|----|--------|--------|---------|---------|---------|
| Linux | Debian/Ubuntu | gated: CI (Ubuntu 22.04, 24.04; gcc, clang), Codeberg CI (Debian trixie), EC2 | gated: CI (Ubuntu 24.04 arm; gcc, clang) | gated: lixr (Bianbu), rv (Ubuntu 24.04) | n/a |
| Linux | RPM (Fedora, RHEL) | gated: CI (Fedora, Rocky 9) | gated: CI (Fedora, Rocky 9) | not tested | n/a |
| Linux | NixOS / Nix | gated: CI `nix build` (gcc, clang), meh (NixOS) | gated: CI `nix build` | not tested | n/a |
| macOS | | gated: CI (macos-15-intel) | gated: CI (macos-15) | n/a | n/a |
| Windows 11 | | **not supported** (no port) | **not supported** (no port) | n/a | n/a |
| illumos | OpenIndiana | not tested | n/a | n/a | gated: sun (big-endian; also reachable as icarus) |
| illumos | Solnix | not tested | not tested | not tested | not tested |

"n/a": the OS does not ship on that CPU.

## Release gate

A tag is cut only when all of these pass on the release commit, from the
release tarball (`ci/make-dist.sh`) where the host builds from source:

1. GitHub CI and Codeberg CI green (every row above marked CI).
2. EC2 x86_64 gate: `make check`, the feature matrix (322 cases), ASan and
   UBSan `make check`, TSan on the concurrency tests, `make abi-check`,
   34 cross-version cases against the previous release, `nix build`.
3. RISC-V (lixr and rv) and big-endian SPARC (sun): `make check`,
   the feature matrix, and the installed-library examples
   (`/tmp/remote_qual.sh`, kept by the maintainer).
4. NixOS (meh): `nix build` of the flake with gcc and clang, and
   `nix flake check`.

## Gaps

- **Windows 11 (x86_64, aarch64).** Not supported. The core compressors
  build with MSVC, but the program and library use POSIX threads and
  semaphores, `mmap`, POSIX file and directory calls, signals and
  `nftw`. A port is real work (a platform layer, or a MinGW/MSYS2 build
  as a first step) and has not been started. Until then no Windows cell
  can be gated.
- **Solnix (illumos on x86_64, aarch64, riscv64, sparcv9).** No Solnix
  host is in the test fleet. OpenIndiana on SPARC is the nearest
  covered relative. Solnix packages go through the solnix.io cache
  workflow, which has its own build farm.
- **RPM on riscv64**, **NixOS on riscv64**. No hosts.
- **Only one SPARC host.** sun and icarus are the same machine (two
  names, one hostid), so big-endian coverage has a single point of
  failure.
- **Windows host santorini** exists in the fleet but is not reachable
  over SSH at the time of writing, and there is nothing to run on it
  until the port exists.
