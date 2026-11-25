#!/bin/sh
# ci/gate.sh: the release gate for one host (docs/PLATFORMS.md).
#
#   ci/gate.sh <release-tarball> [previous-release-tarball]
#
# Run it on each host in docs/PLATFORMS.md from the candidate tarball made by
# ci/make-dist.sh. In a fresh temporary directory it:
#   1. builds the candidate and runs ci/build-test.sh full (unit,
#      integration, backward-compat references, examples, feature matrix);
#   2. checks the installed library and headers with the examples;
#   3. where clang and the sanitizer runtimes exist: ASan+UBSan make check,
#      and TSan on the concurrency tests;
#   4. where libabigail exists: make abi-check;
#   5. with a previous release: builds it and runs ci/xcompat.sh and
#      ci/pwcompat.sh against it.
# Every step prints "gate <step>: PASS|FAIL|SKIP <why>"; the last line is
# "gate: PASS" or "gate: FAIL (<steps>)". Exit status 0 only on PASS.
# Nothing outside the temporary directory is touched; it is removed at the
# end unless GATE_KEEP=1.
set -u
TB=${1:?usage: ci/gate.sh <release-tarball> [previous-release-tarball]}
PREV=${2:-}
case $TB in /*) ;; *) TB=$(pwd)/$TB ;; esac
case $PREV in ""|/*) ;; *) PREV=$(pwd)/$PREV ;; esac
W=$(mktemp -d "${GATE_DIR:-${TMPDIR:-/tmp}}/pc-gate.XXXXXX") || exit 2
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}
export JOBS
failed=""

case "$(uname -s)" in
SunOS)	PATH=/usr/bin:/usr/sbin:/sbin:/usr/gnu/bin:$PATH; MAKE=gmake
	CFG="--disable-wavpack --with-openssl=${OPENSSL_PREFIX:-/usr/openssl/3}" ;;
*)	MAKE=${MAKE:-make}; CFG="--disable-wavpack" ;;
esac
export MAKE PATH

step() {	# step <name> <log> <cmd...>
	name=$1 log=$2; shift 2
	if "$@" > "$W/$log" 2>&1; then
		echo "gate $name: PASS"
	else
		echo "gate $name: FAIL (last lines of $log follow)"
		tail -n 15 "$W/$log" | sed 's/^/    /'
		failed="$failed $name"
	fi
}
skip() { echo "gate $1: SKIP $2"; }

unpack() {	# unpack <tarball> <dir>: extract, print the source dir
	mkdir -p "$2" && (cd "$2" && gzip -dc "$1" | tar xf - 2>/dev/null) &&
	    ls -d "$2"/pcompress-*/ | head -1 | sed 's#/$##'
}

echo "gate: host $(uname -n) $(uname -srm), $JOBS jobs, in $W"
SRC=$(unpack "$TB" "$W/rel") || { echo "gate: FAIL (cannot unpack $TB)"; exit 1; }
echo "gate: candidate $(basename "$SRC")"

# 1. Build and test.
step full full.log sh "$SRC/ci/build-test.sh" full $CFG
NEWBIN=$SRC/pcompress

# 2. Install, then build the examples against the installed copy.
# shellcheck disable=SC2046 # pkg-config output must split into separate flags
inst() {
	cd "$SRC" && $MAKE install DESTDIR="$W/inst" PREFIX=/usr/local &&
	PKG_CONFIG_SYSROOT_DIR="$W/inst" PKG_CONFIG_LIBDIR="$W/inst/usr/local/lib/pkgconfig" &&
	export PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR &&
	cc=${CC:-$(command -v cc || command -v gcc || command -v clang)} && rp="-Wl,-rpath,$W/inst/usr/local/lib" &&
	{ [ "$(uname -s)" != SunOS ] || rp="-Wl,-R,$W/inst/usr/local/lib"; } &&
	$cc $(pkg-config --cflags libpcompress) examples/pzcat.c -o "$W/pzcat" $(pkg-config --libs libpcompress) $rp &&
	"$W/pzcat" -c < README.md > "$W/r.pz" && "$W/pzcat" < "$W/r.pz" | cmp - README.md &&
	"$W/inst/usr/local/bin/pcompress" -d "$W/r.pz" "$W/r.out" && cmp "$W/r.out" README.md &&
	$cc $(pkg-config --cflags libpcompress) examples/pzstore.c -o "$W/pzstore" $(pkg-config --libs libpcompress) $rp &&
	head -c 32 /dev/urandom > "$W/key" && cat ./*.c > "$W/in" &&
	"$W/pzstore" put "$W/st" "$W/key" "$W/in" > "$W/m1" &&
	"$W/pzstore" put "$W/st" "$W/key" "$W/in" > "$W/m2" 2> "$W/second" &&
	grep -q '^pzstore: 0 chunks stored' "$W/second" &&
	"$W/pzstore" get "$W/st" "$W/key" < "$W/m1" | cmp - "$W/in" &&
	$cc -Wall $(pkg-config --cflags libpcompress) examples/backup/pcbackup.c -o "$W/pcbackup" $(pkg-config --libs libpcompress) $rp &&
	TMPDIR="$W" sh examples/backup/test.sh "$W/pcbackup"
}
step installed install.log inst

# 3. Sanitizers, in their own copies of the tree.
san_ok() {	# san_ok <flag>: can clang build an OpenMP program with this sanitizer?
	command -v clang > /dev/null 2>&1 || return 1
	# pcompress needs OpenMP; clang without libomp cannot build it at all.
	printf '#include <omp.h>\nint main(void){return omp_get_max_threads() < 1;}\n' > "$W/t.c"
	clang -fopenmp -fsanitize="$1" "$W/t.c" -o "$W/t" > /dev/null 2>&1 && "$W/t"
}
if san_ok address,undefined; then
	asan() {
		d=$(unpack "$TB" "$W/asan") && cd "$d" &&
		CC=clang CXX=clang++ PCOMPRESS_SANITIZE=1 sh ci/build-test.sh quick $CFG
	}
	step asan asan.log asan
else
	skip asan "no clang with ASan/UBSan and OpenMP here"
fi
# OpenMP needs libarcher (LLVM's TSan annotations for libomp): without it
# every parallel region reports races that are not there.
archer=$(clang -print-file-name=libarcher.so 2>/dev/null || true)
case $archer in /*) ;; *) archer="" ;; esac
if [ -z "$archer" ]; then
	skip tsan "no libarcher.so for clang (TSan cannot see OpenMP's synchronisation without it)"
elif san_ok thread; then
	tsan() {
		d=$(unpack "$TB" "$W/tsan") && cd "$d" &&
		CC=clang CXX=clang++ CFLAGS="-fsanitize=thread -g -O1" LDFLAGS="-fsanitize=thread" \
		    ./config $CFG && $MAKE -j"$JOBS" && $MAKE test-unit-build buildtmp/test_public buildtmp/test_chunk || return 1
		OMP_TOOL_LIBRARIES=$archer; export OMP_TOOL_LIBRARIES
		TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$d/ci/tsan.supp"
		export TSAN_OPTIONS
		for t in test_reentrant test_api test_public test_chunk test_sem; do
			echo "== $t"; buildtmp/$t || return 1
		done
	}
	step tsan tsan.log tsan
else
	skip tsan "no clang with TSan and OpenMP here"
fi

# 4. ABI. abi/libpcompress.abi is in libabigail's own format, which a
# newer libabigail may not read ("incompatible format version"); then
# compare with the previous release's library, built with -g, instead.
if command -v abidw > /dev/null 2>&1 && command -v abidiff > /dev/null 2>&1; then
	abi() {
		d=$(unpack "$TB" "$W/abi") && cd "$d" &&
		CFLAGS=-g ./config $CFG && $MAKE -j"$JOBS" || return 1
		$MAKE abi-check > abi-check.out 2>&1 && { cat abi-check.out; return 0; }
		cat abi-check.out
		grep -q 'incompatible format version' abi-check.out && [ -n "$PREV" ] || return 1
		echo "abi: the stored corpus is unreadable here: comparing with $(basename "$PREV")"
		p=$(unpack "$PREV" "$W/abiprev") && (cd "$p" && CFLAGS=-g ./config $CFG && $MAKE -j"$JOBS") &&
		abidiff --no-added-syms --drop-private-types --suppressions abi/suppressions.ini \
		    --hd1 "$p/include/pcompress" --hd2 include/pcompress \
		    "$p/libpcompress.so.1" libpcompress.so.1
	}
	step abi abi.log abi
else
	skip abi "libabigail (abidw/abidiff) not installed"
fi

# 5. Cross-version.
if [ -n "$PREV" ]; then
	prev() {
		d=$(unpack "$PREV" "$W/prev") && cd "$d" && ./config $CFG && $MAKE -j"$JOBS" &&
		echo "previous: $(./pcompress -V)"
	}
	step previous-build prev.log prev
	OLDBIN=$(ls -d "$W"/prev/pcompress-*/ 2>/dev/null | head -1)pcompress
	if [ -x "$OLDBIN" ] && [ -x "$NEWBIN" ]; then
		step xcompat xcompat.log sh "$SRC/ci/xcompat.sh" "$NEWBIN" "$OLDBIN" "$W/xc"
		step pwcompat pwcompat.log sh "$SRC/ci/pwcompat.sh" "$NEWBIN" "$OLDBIN" "$W/pw"
	else
		skip xcompat "a build is missing"; skip pwcompat "a build is missing"
	fi
else
	skip xcompat "no previous release given"; skip pwcompat "no previous release given"
fi

for l in full xcompat pwcompat; do
	[ -f "$W/$l.log" ] && grep -E '^(TOTAL|xcompat|pwcompat) pass=|Sections (passed|failed)|^pcbackup:' "$W/$l.log" | sed "s/^/  $l: /"
done
if [ "${GATE_KEEP:-0}" = 1 ]; then echo "gate: kept $W"; else find "$W" -depth -delete 2>/dev/null; fi
if [ -z "$failed" ]; then echo "gate: PASS"; else echo "gate: FAIL ($failed )"; exit 1; fi
