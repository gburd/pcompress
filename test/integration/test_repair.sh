#!/bin/sh
#
# Integration test: repair data (-Y) and repair (-y). A file with repair
# data decompresses as any other, a run of damaged bytes within the
# bound is repaired in place (and to a second name) and the file then
# decompresses to the input, damage beyond the bound is refused with the
# file unchanged, and a damaged version 12 file's repair region cannot
# be swapped or extended without the reader noticing.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

T="${TMPDIR:-/tmp}/pc_repair_$$"
mkdir -p "$T" || exit 1
trap 'find "$T" -depth -delete 2>/dev/null' EXIT

echo "========================================="
echo " Integration: Repair data"
echo "========================================="

# About 3 MB of text and 1 MB of random bytes: compressed, still 1.5 MB.
i=0
while [ $i -lt 40000 ]; do
	echo "row $i: $((i * 7919 % 10007)) the quick brown fox $((i % 97))"
	i=$((i + 1))
done > "$T/in"
head -c 1048576 /dev/urandom >> "$T/in"
echo "repair test password" > "$T/pw.src"

# zap <file> <offset> <count>: overwrite count 4 KB blocks from offset.
zap() {
	dd if=/dev/zero of="$1" bs=4096 seek="$2" count="$3" conv=notrunc 2>/dev/null
}

# pw <opts>: a fresh password file option when opts encrypt (-w wipes it).
pw() {
	case " $* " in *" -e "*) cp "$T/pw.src" "$T/pw"; echo "-w $T/pw" ;; esac
}

for opts in "-c zstd -l6" "-c zstd -l6 -X" "-c lz4 -l1 -e AES" "-c zstd -l6 -e SALSA20 -X"; do
	echo ""
	echo "--- $opts ---"
	rm -f "$T/x.pz" "$T/out"
	# shellcheck disable=SC2046,SC2086
	if ! $PCOMPRESS $opts $(pw $opts) -Y 10 -s 256k "$T/in" "$T/x" >/dev/null 2>&1; then
		log_fail "$opts: compress with -Y"
		continue
	fi
	cp "$T/x.pz" "$T/good.pz"
	# shellcheck disable=SC2046
	if $PCOMPRESS -d $(pw $opts) "$T/x.pz" "$T/out" >/dev/null 2>&1 && cmp -s "$T/in" "$T/out"; then
		log_pass "$opts: decompress"
	else
		log_fail "$opts: decompress"
	fi
	if $PCOMPRESS -y "$T/x.pz" >/dev/null 2>&1 && cmp -s "$T/x.pz" "$T/good.pz"; then
		log_pass "$opts: intact file checks clean"
	else
		log_fail "$opts: intact file"
	fi

	# 64 KB of zeros in the chunks: decompression fails and points at -y.
	zap "$T/x.pz" 100 16
	rm -f "$T/out"
	# shellcheck disable=SC2046
	if ! $PCOMPRESS -d $(pw $opts) "$T/x.pz" "$T/out" > "$T/err" 2>&1 &&
	    grep -q "pcompress -y" "$T/err"; then
		log_pass "$opts: damaged file refused, repair suggested"
	else
		log_fail "$opts: damaged file not refused"
	fi
	if $PCOMPRESS -y "$T/x.pz" >/dev/null 2>&1 && cmp -s "$T/x.pz" "$T/good.pz"; then
		log_pass "$opts: repaired in place"
	else
		log_fail "$opts: not repaired"
	fi

	# Damage at the end (the trailer and the repair header copy there).
	size=$(wc -c < "$T/x.pz" | tr -d ' ')
	dd if=/dev/zero of="$T/x.pz" bs=1 seek=$((size - 100)) count=100 conv=notrunc 2>/dev/null
	zap "$T/x.pz" 10 1
	rm -f "$T/out"
	# shellcheck disable=SC2046
	if ! $PCOMPRESS -d $(pw $opts) "$T/x.pz" "$T/out" > "$T/err" 2>&1 &&
	    grep -q "pcompress -y" "$T/err"; then
		log_pass "$opts: damaged end refused, repair suggested"
	else
		log_fail "$opts: damaged end"
	fi
	rm -f "$T/fixed.pz"
	if $PCOMPRESS -y "$T/x.pz" "$T/fixed.pz" >/dev/null 2>&1 &&
	    cmp -s "$T/fixed.pz" "$T/good.pz" && ! cmp -s "$T/x.pz" "$T/good.pz"; then
		log_pass "$opts: repaired copy, original left alone"
	else
		log_fail "$opts: repaired copy"
	fi
	rm -f "$T/out"
	# shellcheck disable=SC2046
	if $PCOMPRESS -d $(pw $opts) "$T/fixed.pz" "$T/out" >/dev/null 2>&1 &&
	    cmp -s "$T/in" "$T/out"; then
		log_pass "$opts: repaired copy decompresses"
	else
		log_fail "$opts: repaired copy does not decompress"
	fi
done

echo ""
echo "--- Limits ---"
# 12 MB of random bytes at 10%: stripes of 40 data and 4 parity blocks,
# 64 stripes interleaved, so a run of 256 blocks (1 MB) costs each
# stripe 4 and is repaired; a run of 512 costs 8 and is not.
head -c 12582912 /dev/urandom > "$T/big"
rm -f "$T/x.pz"
$PCOMPRESS -c zstd -l6 -Y 10 -s 1m "$T/big" "$T/x" >/dev/null 2>&1
cp "$T/x.pz" "$T/good.pz"
zap "$T/x.pz" 0 256
if $PCOMPRESS -y "$T/x.pz" >/dev/null 2>&1 && cmp -s "$T/x.pz" "$T/good.pz"; then
	log_pass "a 1 MB burst is repaired"
else
	log_fail "a 1 MB burst is not repaired"
fi
zap "$T/x.pz" 0 512
cp "$T/x.pz" "$T/bad.pz"
if ! $PCOMPRESS -y "$T/x.pz" > "$T/err" 2>&1 && grep -q "too many" "$T/err" &&
    cmp -s "$T/x.pz" "$T/bad.pz"; then
	log_pass "a 2 MB burst is refused, file unchanged"
else
	log_fail "a 2 MB burst"
fi

# An archive (with its metadata stream): repaired and extracted.
mkdir -p "$T/tree/sub" "$T/xt"
cp "$T/in" "$T/tree/one"
head -c 300000 "$T/big" > "$T/tree/sub/two"
rm -f "$T/a.pz"
if $PCOMPRESS -a -c zstd -l6 -Y 20 -s 256k "$T/tree" "$T/a" >/dev/null 2>&1; then
	zap "$T/a.pz" 50 8
	if $PCOMPRESS -y "$T/a.pz" >/dev/null 2>&1 &&
	    $PCOMPRESS -d "$T/a.pz" "$T/xt" >/dev/null 2>&1 &&
	    diff -r "$T/tree" "$T/xt/${T#/}/tree" >/dev/null 2>&1; then
		log_pass "archive repaired and extracted"
	else
		log_fail "archive repaired and extracted"
	fi
else
	log_fail "archive with -Y"
fi

rm -f "$T/x.pz"
$PCOMPRESS -c zstd -l6 -s 256k "$T/in" "$T/x" >/dev/null 2>&1
if ! $PCOMPRESS -y "$T/x.pz" > "$T/err" 2>&1 && grep -q "No usable repair data" "$T/err"; then
	log_pass "a file without repair data is refused"
else
	log_fail "a file without repair data"
fi
if ! $PCOMPRESS -c zstd -Y 10 -p < "$T/in" > /dev/null 2>&1 &&
    ! $PCOMPRESS -c zstd -Y 0 "$T/in" "$T/y" > /dev/null 2>&1 &&
    ! $PCOMPRESS -c zstd -Y 51 "$T/in" "$T/y" > /dev/null 2>&1 &&
    ! $PCOMPRESS -d -Y 10 "$T/x.pz" "$T/y" > /dev/null 2>&1; then
	log_pass "-Y refused with a stream, out of range, or when decompressing"
else
	log_fail "-Y option checks"
fi

# Version 12: the region is sealed with the file's end record. Another
# file's region (same password, different key) is refused as trailing
# bytes, and so is anything appended after a region.
echo ""
echo "--- Sealed region ---"
if command -v python3 >/dev/null 2>&1; then
	for n in a b; do
		rm -f "$T/$n.pz"
		cp "$T/pw.src" "$T/pw"
		$PCOMPRESS -c zstd -l6 -e AES -w "$T/pw" -Y 10 -s 256k "$T/in" "$T/$n" >/dev/null 2>&1
	done
	python3 - "$T/a.pz" "$T/b.pz" "$T/swap.pz" <<'PYEOF'
import struct, sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
def plen(d):
    return struct.unpack("<Q", d[-72 + 24:-72 + 32])[0]
open(sys.argv[3], "wb").write(a[:plen(a)] + b[plen(b):])
PYEOF
	rm -f "$T/out"
	cp "$T/pw.src" "$T/pw"
	if ! $PCOMPRESS -d -w "$T/pw" "$T/swap.pz" "$T/out" >/dev/null 2>&1; then
		log_pass "another file's repair region refused"
	else
		log_fail "another file's repair region accepted"
	fi
	cp "$T/a.pz" "$T/app.pz"
	printf 'appended' >> "$T/app.pz"
	rm -f "$T/out"
	cp "$T/pw.src" "$T/pw"
	if ! $PCOMPRESS -d -w "$T/pw" "$T/app.pz" "$T/out" >/dev/null 2>&1; then
		log_pass "bytes after the repair region refused"
	else
		log_fail "bytes after the repair region accepted"
	fi
	# The trailer copy's AEAD tag damaged: repair has no key, but the
	# header hash covers the tag, so it rebuilds the trailer from the
	# first copy and never the other way round.
	cp "$T/a.pz" "$T/tag.pz"
	size=$(wc -c < "$T/tag.pz" | tr -d ' ')
	printf 'X' | dd of="$T/tag.pz" bs=1 seek=$((size - 88 + 70)) conv=notrunc 2>/dev/null
	rm -f "$T/out"
	cp "$T/pw.src" "$T/pw"
	if $PCOMPRESS -y "$T/tag.pz" >/dev/null 2>&1 && cmp -s "$T/tag.pz" "$T/a.pz" &&
	    $PCOMPRESS -d -w "$T/pw" "$T/tag.pz" "$T/out" >/dev/null 2>&1 &&
	    cmp -s "$T/in" "$T/out"; then
		log_pass "damaged tag of the last header copy repaired from the first"
	else
		log_fail "damaged tag of the last header copy"
	fi
else
	SKIP=$((SKIP + 1)); echo "  SKIP: sealed region (no python3)"
fi

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="
[ $FAIL -gt 0 ] && exit 1
[ $PASS -eq 0 ] && { echo "ERROR: no test cases ran"; exit 1; }
exit 0
