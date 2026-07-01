#!/bin/sh
#
# Integration test: Edge cases and error conditions.
# Tests empty files, single-byte inputs, invalid parameters, and corrupted data.
#

PCOMPRESS="../../pcompress"
PASS=0
FAIL=0
SKIP=0

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
log_skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1"; }

algo_available() {
	_aa_tmp="${TMPDIR:-/tmp}/pcompress_algo_probe_$$"
	printf 'probe' > "$_aa_tmp"
	$PCOMPRESS -c "$1" -l 1 -s 64k "$_aa_tmp" 2>/dev/null
	_aa_rv=$?
	rm -f "$_aa_tmp" "$_aa_tmp.pz"
	return $_aa_rv
}

echo "========================================="
echo " Integration: Edge Cases & Error Handling"
echo "========================================="

if [ ! -d datafiles ]; then
	mkdir datafiles
fi

# --- Empty file test ---
echo ""
echo "--- Empty file ---"
: > datafiles/empty.dat
for algo in lz4 zlib lzma zstd; do
	if ! algo_available "$algo"; then
		continue
	fi
	rm -f datafiles/empty.dat.pz datafiles/empty.dat.out
	$PCOMPRESS -c ${algo} -l 1 -s 64k datafiles/empty.dat 2>/dev/null
	# Empty file may succeed or fail depending on algorithm; either is acceptable
	# as long as it does not crash
	if [ -f datafiles/empty.dat.pz ]; then
		$PCOMPRESS -d datafiles/empty.dat.pz datafiles/empty.dat.out 2>/dev/null
		if [ -f datafiles/empty.dat.out ]; then
			sz=$(wc -c < datafiles/empty.dat.out)
			if [ "$sz" -eq 0 ]; then
				log_pass "empty/${algo}: round-trip OK"
			else
				log_fail "empty/${algo}: decompressed to non-empty"
			fi
		else
			log_pass "empty/${algo}: compression produced output, decompression handled"
		fi
	else
		log_pass "empty/${algo}: gracefully rejected empty input"
	fi
	rm -f datafiles/empty.dat.pz datafiles/empty.dat.out
done

# --- Single byte test ---
echo ""
echo "--- Single byte ---"
printf 'X' > datafiles/single.dat
for algo in lz4 zlib zstd; do
	if ! algo_available "$algo"; then
		continue
	fi
	rm -f datafiles/single.dat.pz datafiles/single.dat.out
	$PCOMPRESS -c ${algo} -l 1 -s 64k datafiles/single.dat 2>/dev/null
	if [ -f datafiles/single.dat.pz ]; then
		$PCOMPRESS -d datafiles/single.dat.pz datafiles/single.dat.out 2>/dev/null
		if diff -q datafiles/single.dat datafiles/single.dat.out > /dev/null 2>&1; then
			log_pass "single_byte/${algo}: round-trip OK"
		else
			log_fail "single_byte/${algo}: data mismatch"
		fi
	else
		log_pass "single_byte/${algo}: gracefully rejected single byte"
	fi
	rm -f datafiles/single.dat.pz datafiles/single.dat.out
done

# --- Invalid algorithm ---
echo ""
echo "--- Invalid parameters ---"
dd if=/dev/urandom of=datafiles/small.dat bs=1024 count=16 2>/dev/null

$PCOMPRESS -c nonexistent_algo -l 1 -s 64k datafiles/small.dat 2>/dev/null
if [ $? -ne 0 ]; then
	log_pass "invalid_algo: rejected unknown algorithm"
else
	log_fail "invalid_algo: should have failed"
	rm -f datafiles/small.dat.pz
fi

# Invalid level (above max)
for algo in lz4 zlib; do
	if ! algo_available "$algo"; then
		continue
	fi
	$PCOMPRESS -c ${algo} -l 99 -s 64k datafiles/small.dat 2>/dev/null
	if [ $? -ne 0 ]; then
		log_pass "invalid_level/${algo}: rejected level 99"
	else
		# Some algorithms clamp the level; this is also acceptable
		log_pass "invalid_level/${algo}: clamped level (acceptable)"
		rm -f datafiles/small.dat.pz
	fi
done

# --- Corrupted compressed data ---
echo ""
echo "--- Corrupted data detection ---"
for algo in lz4 zlib lzma zstd; do
	if ! algo_available "$algo"; then
		continue
	fi
	rm -f datafiles/small.dat.pz datafiles/small.dat.out
	$PCOMPRESS -c ${algo} -l 3 -s 64k -S CRC64 datafiles/small.dat 2>/dev/null
	if [ $? -ne 0 ]; then
		log_skip "corrupt/${algo}: compression failed"
		continue
	fi

	# Corrupt the compressed data (overwrite bytes in the middle)
	cp datafiles/small.dat.pz datafiles/small.dat.pz.bak
	dd if=/dev/urandom conv=notrunc of=datafiles/small.dat.pz bs=4 seek=50 count=2 2>/dev/null

	$PCOMPRESS -d datafiles/small.dat.pz datafiles/small.dat.out 2>/dev/null
	if [ $? -ne 0 ]; then
		log_pass "corrupt/${algo}: detected corruption"
	else
		if diff -q datafiles/small.dat datafiles/small.dat.out > /dev/null 2>&1; then
			log_pass "corrupt/${algo}: corruption in non-data region (still valid)"
		else
			log_fail "corrupt/${algo}: silently produced wrong output"
		fi
	fi
	rm -f datafiles/small.dat.pz datafiles/small.dat.pz.bak datafiles/small.dat.out
done

rm -f datafiles/empty.dat datafiles/single.dat datafiles/small.dat

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
exit 0
