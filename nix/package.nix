{
  lib,
  stdenv,
  fetchFromGitHub,
  autoconf,
  automake,
  libtool,
  perl,
  pkg-config,
  yasm,
  which,
  patchelf,
  openssl,
  zlib,
  bzip2,
  llvmPackages,
  src ? null,
}:

let
  # Bundled third-party sources, pinned to the revisions of the git submodules.
  lz4-src = fetchFromGitHub {
    owner = "lz4";
    repo = "lz4";
    rev = "e7c36223415219c0a18ea75e3fe2eb5d267ee667";
    hash = "sha256-ApkSJCBB7s1zm6AU4/LD7HvbivQEQEkEq5UXlV4Le9w=";
  };
  libbsc-src = fetchFromGitHub {
    owner = "IlyaGrebnov";
    repo = "libbsc";
    rev = "4178579ff11d878863ea0d2758c125a0987d7b9b";
    hash = "sha256-fcG7PStI9V4EhyW5USwIVn+FkHFTcuToKIFYXudDLHo=";
  };
  zstd-src = fetchFromGitHub {
    owner = "facebook";
    repo = "zstd";
    rev = "49cf51799ea051457f57243527aa2058f999f005";
    hash = "sha256-FuW9d91JdfbP+YtPBVyjmtlhkJDre/DKGhiVdU5UWKs=";
  };
  libarchive-src = fetchFromGitHub {
    owner = "libarchive";
    repo = "libarchive";
    rev = "0c48412be2d94afe1859a807ea7896a18d8900bd"; # v3.8.10
    hash = "sha256-B2leEyMwSy7YSEMKSTsgfeBbKo4kOQPU0ME0XWuZMik=";
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "pcompress";
  version = "4.1.0";

  src =
    if src != null then
      src
    else
      fetchFromGitHub {
        owner = "gburd";
        repo = "pcompress";
        rev = "v${finalAttrs.version}";
        hash = lib.fakeHash;
      };

  nativeBuildInputs = [
    autoconf
    automake
    libtool
    perl
    pkg-config
    which
    patchelf
  ]
  ++ lib.optional stdenv.hostPlatform.isx86_64 yasm;

  buildInputs = [
    openssl
    zlib
    bzip2
  ]
  ++ lib.optional stdenv.cc.isClang llvmPackages.openmp;

  strictDeps = true;
  __structuredAttrs = true;

  # ./config is a hand-written script, not autoconf. --portable: no host
  # CPU probing (SIMD paths are still chosen at run time).
  configureScript = "./config";
  dontDisableStatic = true;
  configureFlags = [
    "--portable"
    "--disable-wavpack"
  ];
  checkTarget = "check";
  enableParallelBuilding = true;

  doCheck = true;

  # The bundled libraries are built in-tree (pcompress patches libarchive).
  postUnpack = ''
    for d in lz4 bsc zstd archive/libarchive; do
      chmod -R u+w "$sourceRoot/$d" 2>/dev/null || true
      find "$sourceRoot/$d" -mindepth 1 -delete 2>/dev/null || true
      mkdir -p "$sourceRoot/$d"
    done
    cp -r ${lz4-src}/. "$sourceRoot/lz4"
    cp -r ${libbsc-src}/. "$sourceRoot/bsc"
    cp -r ${zstd-src}/. "$sourceRoot/zstd"
    cp -r ${libarchive-src}/. "$sourceRoot/archive/libarchive"
    chmod -R u+w "$sourceRoot/lz4" "$sourceRoot/bsc" "$sourceRoot/zstd" "$sourceRoot/archive/libarchive"
  '';

  preConfigure = ''
    patchShebangs config test
  '';

  # LLVM libomp's topology probe asserts in the build sandbox on aarch64
  # (kmp_affinity.cpp); pcompress does not rely on thread affinity.
  preCheck = ''
    export KMP_AFFINITY=disabled
  '';

  postInstall = ''
    # The binary links libpcompress.so.1 from $out/lib.
    chmod u+w $out/bin/pcompress
    patchelf --add-rpath $out/lib $out/bin/pcompress
    chmod 0555 $out/bin/pcompress
    install -Dm644 COPYING COPYING.LESSER -t $out/share/doc/pcompress
  '';

  meta = {
    description = "Parallel compressor and archiver with deduplication, delta compression and encryption";
    longDescription = ''
      Pcompress splits its input into chunks and compresses them in parallel
      with one of many algorithms (zstd, LZMA, PPMd, libbsc, LZ4, zlib, bzip2,
      or adaptive per-chunk selection). It can deduplicate within a chunk or
      across the whole input, delta-encode similar blocks, apply content
      filters (JPEG, PNM, x86 code, text) in archive mode, and encrypt and
      authenticate every chunk with AES or XSalsa20.
    '';
    homepage = "https://github.com/gburd/pcompress";
    license = lib.licenses.lgpl3Plus;
    maintainers = [ ];
    mainProgram = "pcompress";
    # POSIX threads/semaphores/mmap are required; 64-bit targets only.
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
    badPlatforms = [ lib.systems.inspect.patterns.is32bit ];
  };
})
