#!/bin/sh
# ci/pgo-build.sh: a profile-guided (PGO) build of pcompress, in place.
#
#   CC=gcc|clang CXX=g++|clang++ sh ci/pgo-build.sh [config options...]
#
# NB: it runs "git clean -fdx" in the tree and its submodules first.
#
# Builds the tree (the bundled libraries too) with instrumentation, runs a
# training workload (zstd, lz4, lzma, libbsc and adapt2 at levels 3 and 9,
# with and without dedupe, on a tar of this source tree, then an archive
# round trip), and rebuilds with the profile. gcc uses -fprofile-generate
# and -fprofile-use; clang -fprofile-instr-generate and llvm-profdata
# ($LLVM_PROFDATA, default llvm-profdata). The profile lives in a temporary
# directory, since each build starts from a clean tree.
#
# Measured on c7i.4xlarge against a plain build (enwik8 and 100 MB of a
# /usr/lib tar, neither in the training set; one thread, best of three):
# with gcc 13, zstd -l 9 and adapt2 -l 6 on the tar were 7 to 11% faster,
# every other case within 3% either way (zstd -D 3% slower), and with 16
# threads the zstd gain was 1%. clang 18 was within 5% either way. Not
# the 5% general gain that would justify a two-pass release build, so
# release builds do not use it; it is here to repeat the measurement
# after a compiler or codec change.
set -eu

cd "$(dirname "$0")/.."
# Every build below runs git clean -fdx: refuse a tree that holds anything
# git would delete, so a checkout with work in it is never wiped.
if [ -n "$(git status --porcelain --ignored)" ] ||
    [ -n "$(git submodule foreach -q 'git status --porcelain --ignored')" ]; then
	echo "pgo-build: the tree has untracked, ignored or modified files, which" >&2
	echo "it would delete; run it in a fresh clone or git worktree." >&2
	exit 1
fi
CC=${CC:-gcc}
MAKE=${MAKE:-make}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
work=$(mktemp -d "${TMPDIR:-/tmp}/pcompress-pgo.XXXXXX")
trap 'find "$work" -depth -delete 2>/dev/null' EXIT
prof=$work/profile
train=$work/train
case "$CC" in
*clang*)
	gen="-fprofile-instr-generate=$prof/%m-%p.profraw"
	use="-fprofile-instr-use=$prof/merged.profdata"
	use="$use -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ;;
*)
	gen="-fprofile-generate=$prof -fprofile-update=atomic"
	use="-fprofile-use=$prof -fprofile-partial-training -Wno-missing-profile"
	# configure probes (conftest.c) were profiled too, as other programs.
	use="$use -Wno-coverage-mismatch" ;;
esac

# build <flags> [config options]: configure and build everything with flags.
# Each build starts from a clean checkout ("make distclean" removes parts
# of libarchive that ./config needs), so this needs a git tree and
# discards untracked files in it and in the submodules.
build() {
	flags=$1
	shift
	git clean -qfdx
	git submodule foreach -q 'git clean -qfdx && git checkout -q -f .'
	CFLAGS="${CFLAGS:-} $flags" LDFLAGS="${LDFLAGS:-} $flags" ./config "$@" > /dev/null
	$MAKE -j"$JOBS" > /dev/null
}

# Round-trip the training corpus through each main codec, then an archive.
run_training() {
	mkdir -p "$train/x"
	tar cf "$train/corpus" --exclude=.git --exclude=buildtmp . 2> /dev/null || true
	for a in zstd lz4 lzma libbsc adapt2; do
		for l in 3 9; do
			for d in "" -D; do
				rm -f "$train/c.pz" "$train/c.out"
				./pcompress -c $a -l $l $d -s 8m "$train/corpus" "$train/c.pz"
				./pcompress -d "$train/c.pz" "$train/c.out"
				cmp "$train/corpus" "$train/c.out"
			done
		done
	done
	./pcompress -a -c adapt2 -l 9 lzma filters "$train/a.pz"
	./pcompress -d "$train/a.pz" "$train/x"
}

build "$gen" "$@"
mkdir -p "$prof"
run_training > /dev/null 2>&1
case "$CC" in
*clang*) ${LLVM_PROFDATA:-llvm-profdata} merge -o "$prof/merged.profdata" "$prof"/*.profraw ;;
esac
build "$use" "$@"
find "$work" -depth -delete
echo "PGO build done ($CC)"
