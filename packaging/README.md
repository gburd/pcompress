# Packaging

What a release ships, how each piece is built, and how to check it. The
release workflow (`.github/workflows/release.yml`) builds everything from
the tag; CI (`ci.yml`, "Packages") builds the packages and the static
binary from the release tarball on every push, so the recipes do not rot.

| Artefact | Built from | Where |
|---|---|---|
| `pcompress-<ver>.tar.gz` | `ci/make-dist.sh` | source with the bundled submodules and libarchive's generated `configure` |
| `SHA256SUMS` | release workflow | checksums of every file below |
| `pcompress-<ver>-<os>-<arch>.tar.gz` | release workflow | binary, library, header, man pages; runs from wherever it is unpacked |
| `pcompress-<ver>-linux-<arch>-static` | `make static` on Alpine | one fully static (musl) executable, x86_64 and arm64 |
| `pcompress-<ver>-1.<dist>.<arch>.rpm` | `packaging/pcompress.spec.in` | Fedora, x86_64 and aarch64; `-devel` subpackage |
| `pcompress_<ver>-1_<arch>.deb` | `packaging/debian/` | Debian stable, amd64 and arm64; `libpcompress1`, `libpcompress-dev` |

Homebrew (`gburd/homebrew-pcompress`) and nixpkgs (`pkgs/by-name/pc/pcompress`)
are updated by hand after a release; `nix/package.nix` in this repository is
the same recipe.

Not built yet: Alpine `.apk` and Arch `PKGBUILD`.

## Verifying a download

Every release file has a signed build-provenance attestation, made by the
release workflow with GitHub's Sigstore integration (no long-lived key):

```sh
gh attestation verify pcompress-4.3.0.tar.gz -R gburd/pcompress
sha256sum -c --ignore-missing SHA256SUMS
```

The attestation says which workflow run, at which commit of
`gburd/pcompress`, produced the file. A maintainer signature may also be
attached (`<file>.sig`, see below); check it with the public key in
`packaging/allowed_signers`:

```sh
ssh-keygen -Y verify -f packaging/allowed_signers -I greg@burd.me \
    -n file -s pcompress-4.3.0.tar.gz.sig < pcompress-4.3.0.tar.gz
```

## Building the packages yourself

`packaging/prepare.sh <srcdir>` writes the version and date into the RPM
spec and a `debian/` directory, in an unpacked release tarball:

```sh
tar xzf pcompress-4.3.0.tar.gz && sh pcompress-4.3.0/packaging/prepare.sh pcompress-4.3.0
# RPM
cp pcompress-4.3.0.tar.gz ~/rpmbuild/SOURCES/ && rpmbuild -bb pcompress-4.3.0/packaging/pcompress.spec
# Debian
cp pcompress-4.3.0.tar.gz pcompress_4.3.0.orig.tar.gz && (cd pcompress-4.3.0 && dpkg-buildpackage -us -uc -b)
```

Both build with `./config --portable --disable-rpath`, run `make check`,
and install with `LIBDIR` set to the distribution's library directory.

## Maintainer signature

`packaging/sign-release.sh <file>...` signs files with the maintainer's
SSH signing key (`ssh-keygen -Y sign`, namespace `file`), writing
`<file>.sig` next to each. It runs on the maintainer's machine only; no
CI job has the key.
