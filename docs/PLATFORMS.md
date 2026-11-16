# Platforms

The targets pcompress must support, what each one is tested on today, and
what has to pass before a release is tagged. A cell is only "gated" if a
failure there stops the release.

## Target matrix

| OS | Family | x86_64 | aarch64 | riscv64 | sparcv9 |
|----|--------|--------|---------|---------|---------|
| Linux | Debian/Ubuntu | gated: CI (Ubuntu 22.04, 24.04; gcc, clang), Codeberg CI (Debian trixie) | gated: CI (Ubuntu 24.04 arm; gcc, clang) | gated: lixr (Bianbu), rv (Ubuntu 24.04) | n/a |
| Linux | RPM (Fedora, RHEL) | gated: CI (Fedora, Rocky 9) | gated: CI (Fedora, Rocky 9) | not tested | n/a |
| Linux | NixOS / Nix | gated: CI `nix build` (gcc, clang), meh (NixOS) | gated: CI `nix build` | not tested | n/a |
| macOS | | gated: CI (macos-15-intel) | gated: CI (macos-15) | n/a | n/a |
| Windows 11 | MSYS2 / MinGW-w64 | gated: CI (windows-latest, CLANG64) | gated: CI (windows-11-arm, CLANGARM64) | n/a | n/a |
| illumos | OpenIndiana | not tested | n/a | n/a | gated: sun (big-endian; also reachable as icarus) |
| illumos | Solnix | not tested | not tested | not tested | not tested |

"n/a": the OS does not ship on that CPU.

## Release gate

A tag is cut only when all of these pass on the release commit, from the
release tarball (`ci/make-dist.sh`) where the host builds from source.
Everything needed is in `ci/`:

- `ci/gate.sh <tarball> [previous-release-tarball]` runs one host's gate:
  `ci/build-test.sh full` (unit, integration, backward-compatibility
  references, examples, feature matrix), the installed library with the
  examples, ASan+UBSan and TSan where clang has them, `make abi-check`
  where libabigail is installed, and `ci/xcompat.sh` and `ci/pwcompat.sh`
  against the previous release. Its last line is `gate: PASS` or
  `gate: FAIL (...)`.
- `ci/remote-gate.sh <tarball> [previous-tarball] host...` runs that on
  each host over ssh in a temporary `/scratch` directory, then removes it.

Steps:

1. GitHub CI and Codeberg CI green on the release commit (every row above
   marked CI, including ASan+UBSan and the ABI check).
2. `ci/remote-gate.sh` passes on rv and lixr (RISC-V), sun (big-endian
   SPARC) and meh (x86_64, NixOS), with the previous release's tarball so
   the cross-version checks run.
3. NixOS (meh): `nix build` of the flake with gcc and clang, and
   `nix flake check`.

No build or test runs on a developer workstation.

## Gaps

- **Windows 11 (x86_64, aarch64).** Builds with MSYS2 and the MinGW-w64
  clang toolchains (CLANG64, CLANGARM64): `./config && make && make
  check` produce `buildtmp/pcompress.exe` and `buildtmp/libpcompress-1.dll`
  (with the import library `libpcompress.dll.a`), and CI runs the unit,
  integration and backward-compatibility tests and binary round trips on
  both. Not covered: MSVC builds (the core compressors compile with it,
  the program and library do not); the feature matrix and
  `examples/backup` (POSIX-only test scripts and file APIs); `test_sem`
  (no signals to interrupt a wait). Archives do not store or restore
  symbolic links, owners or POSIX permissions on Windows: links are
  followed when archiving, and extraction keeps the default ACLs. The
  program uses the ANSI (code page) Windows file APIs, so names the
  current code page cannot represent are not supported. The x86
  assembler fast paths are not used (they are ELF-only), and the
  password prompt's echo control is not covered by CI (no console).
- **Solnix (illumos on x86_64, aarch64, riscv64, sparcv9).** No Solnix
  host is in the test fleet. OpenIndiana on SPARC is the nearest
  covered relative. Solnix packages go through the solnix.io cache
  workflow, which has its own build farm.
- **RPM on riscv64**, **NixOS on riscv64**. No hosts.
- **Only one SPARC host.** sun and icarus are the same machine (two
  names, one hostid), so big-endian coverage has a single point of
  failure.
- **Windows hosts** are wina (Windows 11 ARM64, formerly santorini) and
  wini (Windows 11 x64). `ci/gate.sh` has not been run on them; the gate
  for Windows is CI.
