#!/bin/sh
# ci/build-test.sh: the one build+test recipe every CI system runs.
#
#   ci/build-test.sh [quick|full] [config options...]
#
# Environment: CC, CXX, CFLAGS, LDFLAGS (honoured by ./config), MAKE, JOBS,
# OPENSSL_PREFIX (passed as --with-openssl), PCOMPRESS_SANITIZE=1 (ASan+UBSan).
# quick: build, unit, integration. full: also the feature matrix and the
# cross-architecture interop corpus. Exits non-zero on the first failure.
set -eu

mode=${1:-quick}
[ $# -gt 0 ] && shift
MAKE=${MAKE:-make}
if [ -z "${JOBS:-}" ]; then
	JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
fi

say() { printf '\n=== %s (%s)\n' "$*" "$(date -u +%H:%M:%S)"; }

cd "$(dirname "$0")/.."
root=$(pwd)

if [ ! -f lz4/lib/lz4.c ] || [ ! -f zstd/lib/zstd.h ] || \
   [ ! -f archive/libarchive/libarchive/archive.h ] || [ ! -f bsc/libbsc/libbsc.h ]; then
	echo "Bundled sources missing: git submodule update --init --recursive" >&2
	exit 1
fi

say "toolchain"
uname -a
${CC:-cc} --version 2>&1 | head -1
$MAKE --version 2>&1 | head -1

set -- --disable-wavpack "$@"
[ -n "${OPENSSL_PREFIX:-}" ] && set -- "$@" --with-openssl="$OPENSSL_PREFIX"
if [ "${PCOMPRESS_SANITIZE:-0}" = 1 ]; then
	# -fno-sanitize=function: the bundled LZMA SDK (lzma/) calls through
	# function pointers whose declared type differs from the target's, a
	# C idiom clang's UBSan "function" check flags but gcc's does not. It
	# is vendored and not a real bug, and halt_on_error would abort on it.
	CFLAGS="${CFLAGS:-} -fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer -g"
	LDFLAGS="${LDFLAGS:-} -fsanitize=address,undefined"
	export CFLAGS LDFLAGS
	# Configure-time probes (libarchive's autoconf) also link ASan and
	# must not trip leak checks: options are exported after the build.
	export ASAN_OPTIONS=detect_leaks=0
fi

say "configure $*"
./config "$@"

say "build (-j$JOBS)"
case "$(uname -s)" in
MINGW*)
	# Windows: list every compile error in one run before failing.
	$MAKE -k -j"$JOBS" || { $MAKE -k 2>&1 | grep -E 'error|Error' | head -200; exit 1; }
	;;
*)	$MAKE -j"$JOBS" ;;
esac
./pcompress -V

# zstd, lz4 and libbsc are bundled; a direct dependency on a system copy
# means the bundled libarchive picked one up from the build host. Only the
# library's own NEEDED entries count: ldd also lists what its dependencies
# need (Debian's libcrypto pulls in libzstd, illumos's libnsl libmd).
say "bundled libraries stay bundled"
lib=$(ls libpcompress.so.1 libpcompress.1.dylib libpcompress.dylib buildtmp/libpcompress-1.dll \
    2>/dev/null | head -1 || true)
if [ -n "$lib" ]; then
	if [ "${lib%.dll}" != "$lib" ]; then deps=$(objdump -p "$lib" | grep 'DLL Name')
	elif command -v otool >/dev/null 2>&1; then deps=$(otool -L "$lib")
	elif command -v elfdump >/dev/null 2>&1; then deps=$(elfdump -d "$lib" | grep NEEDED)
	elif command -v readelf >/dev/null 2>&1; then deps=$(readelf -d "$lib" | grep NEEDED)
	else deps=$(objdump -p "$lib" 2>/dev/null | grep NEEDED || true); fi
	bad=$(echo "$deps" | grep -E 'lib(zstd|lz4|bsc|b2|lzo2|nettle|mbedcrypto|md)\.' || true)
	if [ -n "$bad" ]; then
		echo "$lib links a system copy of a bundled or unused library:" >&2
		echo "$bad" >&2
		exit 1
	fi
	echo "ok: $lib"
fi

if [ "${PCOMPRESS_SANITIZE:-0}" = 1 ]; then
	export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1
	export LSAN_OPTIONS=suppressions=$root/ci/lsan.supp:print_suppressions=0
	export UBSAN_OPTIONS=print_stacktrace=1
fi

say "unit tests"
$MAKE test-unit TEST_WRAPPER="sh $root/ci/watchdog.sh 600"

say "integration tests"
(cd test && sh run_all_tests.sh integration)

if [ "$mode" = full ]; then
	say "feature matrix"
	CORPUS="$root" sh test/feature_matrix.sh "$root/pcompress" "${TMPDIR:-/tmp}/pc-matrix.$$" quick
	find "${TMPDIR:-/tmp}/pc-matrix.$$" -depth -delete 2>/dev/null || true
fi

say "install smoke test"
dest=${TMPDIR:-/tmp}/pc-install.$$
$MAKE install DESTDIR="$dest" PREFIX=/usr/local >/dev/null
test -x "$dest/usr/local/bin/pcompress"
test -f "$dest/usr/local/share/man/man1/pcompress.1"
find "$dest" -depth -delete

say "OK"
