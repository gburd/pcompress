#!/bin/sh
# ci/xcompat.sh: files written by one pcompress build must read back with
# another, in both directions (release gate, docs/PLATFORMS.md).
#
#   ci/xcompat.sh <new-pcompress> <old-pcompress> [workdir]
#
# <old> is normally the previous release built from its tarball. Cases cover
# every algorithm, dedupe, both ciphers, pipe mode and archives. Prints one
# PASS/FAIL line per case and "xcompat pass=N fail=M"; exits 1 on any FAIL.
set -u
NEW=${1:?usage: ci/xcompat.sh <new-pcompress> <old-pcompress> [workdir]}
OLD=${2:?usage: ci/xcompat.sh <new-pcompress> <old-pcompress> [workdir]}
W=${3:-$(mktemp -d "${TMPDIR:-/tmp}/pc-xcompat.XXXXXX")}
mkdir -p "$W" || exit 2
src=$(cd "$(dirname "$0")/.." && pwd)
pass=0 fail=0

# Input: compressible text, random bytes and a run of zeros, so every codec
# path (including stored chunks) is used.
( cd "$src" && find . -name '*.c' -size -200k | LC_ALL=C sort | head -60 | xargs cat ) > "$W/in" 2>/dev/null
head -c 600000 /dev/urandom >> "$W/in"
dd if=/dev/zero bs=65536 count=8 2>/dev/null >> "$W/in"
printf '%s\n' 'a pass phrase longer than sixteen bytes' > "$W/pw.src"

ok() { pass=$((pass + 1)); echo "PASS $1"; }
no() { fail=$((fail + 1)); echo "FAIL $1"; [ -s "$W/err" ] && head -n 3 "$W/err" | sed 's/^/     /'; }

# -w wipes the password file after reading it: hand each run a fresh copy.
pw() { cp "$W/pw.src" "$W/pw"; echo "$W/pw"; }

# One case: compress with $1, decompress with $2, compare. $3 = label,
# rest = compression options.
one() {
	w=$1 r=$2 label=$3; shift 3
	rm -f "$W/in.pz" "$W/out" "$W/err"
	extra=""
	case " $* " in *" -e "*) extra="-w $(pw)" ;; esac
	if "$w" "$@" $extra "$W/in" "$W/in.pz" > /dev/null 2> "$W/err"; then
		dextra=""
		case " $* " in *" -e "*) dextra="-w $(pw)" ;; esac
		if "$r" -d $dextra "$W/in.pz" "$W/out" > /dev/null 2>> "$W/err" &&
		    cmp -s "$W/in" "$W/out"; then ok "$label"; return; fi
	fi
	# Releases before 4.3 cannot read encrypted files from 4.3 on (format
	# version 11), nor releases before 4.5 those from 4.5 on (version 12):
	# from those, only a clean refusal counts as a pass.
	if [ "$w" = "$NEW" ] && [ -n "$extra" ] &&
	    grep -Eq 'Cannot handle newer archive version (11|12)' "$W/err"; then
		ok "$label (refused: older release)"; return
	fi
	no "$label"
}

for dir in new-old old-new; do
	case $dir in new-old) w=$NEW r=$OLD ;; old-new) w=$OLD r=$NEW ;; esac
	for a in zstd lz4 lzma zlib bzip2 ppmd lzfx adapt adapt2 none; do
		one "$w" "$r" "$dir $a -l6" -c "$a" -l6 -s 1m
	done
	one "$w" "$r" "$dir zstd -l14" -c zstd -l14 -s 4m
	one "$w" "$r" "$dir zstd -D" -c zstd -l6 -s 1m -D
	one "$w" "$r" "$dir zstd -F" -c zstd -l6 -s 1m -F
	one "$w" "$r" "$dir lzma -G" -c lzma -l6 -s 2m -G
	one "$w" "$r" "$dir zstd AES" -c zstd -l6 -s 1m -e AES
	one "$w" "$r" "$dir zstd SALSA20" -c zstd -l6 -s 1m -e SALSA20
	# A seek index (4.5 on): older readers stop before it.
	if [ "$w" = "$NEW" ]; then
		one "$w" "$r" "$dir zstd -X" -c zstd -l6 -s 256k -X
		one "$w" "$r" "$dir zstd -X AES" -c zstd -l6 -s 256k -X -e AES
	fi

	# Pipe mode.
	rm -f "$W/p.pz" "$W/out" "$W/err"
	if "$w" -p -c zstd -l6 < "$W/in" > "$W/p.pz" 2> "$W/err" &&
	    "$r" -d - < "$W/p.pz" > "$W/out" 2>> "$W/err" && cmp -s "$W/in" "$W/out"; then
		ok "$dir pipe zstd"; else no "$dir pipe zstd"; fi

	# Archive of a small tree.
	t="$W/tree"; x="$W/x"
	find "$t" "$x" -depth -delete 2>/dev/null
	mkdir -p "$t/sub/deep" "$x"
	cp "$W/in" "$t/big"; printf 'x' > "$t/one"; : > "$t/empty"
	head -c 70000 "$W/in" > "$t/sub/deep/part"; ln -s ../one "$t/sub/link"
	for xo in "" -X; do
		[ -n "$xo" ] && [ "$w" != "$NEW" ] && continue
		find "$x" -mindepth 1 -delete 2>/dev/null
		rm -f "$W/a.pz" "$W/err"
		if "$w" -a -c zstd -l6 $xo "$t" "$W/a" > /dev/null 2> "$W/err" &&
		    "$r" -d "$W/a.pz" "$x" > /dev/null 2>> "$W/err" &&
		    diff -r "$t" "$x/${t#/}" > /dev/null 2>&1; then
			ok "$dir archive $xo"; else no "$dir archive $xo"; fi
	done
done

find "$W" -depth -delete 2>/dev/null
echo "xcompat pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
