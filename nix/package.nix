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
    rev = "5c4c1fb2354133e1f3b087a341576985f8114bd5";
    hash = "sha256-GaQdKGyySUUDqF30jYIbVKugF7Ui/LaCzUNKt7DHUi4=";
  };
  libbsc-src = fetchFromGitHub {
    owner = "IlyaGrebnov";
    repo = "libbsc";
    rev = "baffa62c70b6ebbecc9af14ce550e965ea247680";
    hash = "sha256-WKSNSWvN7oDnteSEVnPoXAvNOOvrLU4OOQhKULxgxvU=";
  };
  zstd-src = fetchFromGitHub {
    owner = "facebook";
    repo = "zstd";
    rev = "6e1e545916eff6457e4b264f8813a7417510a57e";
    hash = "sha256-AQRxHHSeWa4BNmNn+LnIcYVnmC62y6XtHmv68ENK0ig=";
  };
  libarchive-src = fetchFromGitHub {
    owner = "libarchive";
    repo = "libarchive";
    rev = "8a7a9cc527fd1d6d8664315d3bed47c4259479cc"; # v3.7.9
    hash = "sha256-N1PzkkM6k35GuteKijsJZw5Og0DiM70VihN6qlLqGfU=";
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "pcompress";
  version = "4.0.0";

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

  # ./config is a hand-written script, not autoconf. --portable: no host
  # CPU probing (SIMD paths are still chosen at run time).
  configurePhase = ''
    runHook preConfigure
    patchShebangs config test
    ./config --prefix=$out --portable --disable-wavpack
    runHook postConfigure
  '';

  enableParallelBuilding = true;

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    make test-unit
    (cd test && sh run_all_tests.sh integration)
    runHook postCheck
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
