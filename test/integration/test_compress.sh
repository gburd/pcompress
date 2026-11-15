#!/bin/sh
#
# Integration test: Compress and decompress with all algorithms including zstd.
# Exercises round-trip correctness across multiple levels, chunk sizes, and
# data types.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0

log_pass() {
	PASS=$((PASS + 1))
	echo "  PASS: $1"
}

log_fail() {
	FAIL=$((FAIL + 1))
	echo "  FAIL: $1"
}

log_skip() {
	SKIP=$((SKIP + 1))
	echo "  SKIP: $1"
}

algo_available() {
	# Try a real compression to detect if the algorithm is compiled in.
	# The help text does not list algorithm names, so grep-based detection fails.
	_aa_tmp="${TMPDIR:-/tmp}/pcompress_algo_probe_$$"
	printf 'probe' > "$_aa_tmp"
	$PCOMPRESS -c "$1" -l 1 -s 64k "$_aa_tmp" 2>/dev/null
	_aa_rv=$?
	rm -f "$_aa_tmp" "$_aa_tmp.pz"
	return $_aa_rv
}

echo "========================================="
echo " Integration: Compression Round-Trip"
echo "========================================="

# Create test data if not present
if [ ! -d datafiles ]; then
	mkdir datafiles
fi

# Generate synthetic test data files
if [ ! -f datafiles/text.dat ]; then
	# Repeating text (highly compressible)
	dd if=/dev/zero bs=1024 count=512 2>/dev/null | tr '\0' 'A' > datafiles/text.dat
	i=0
	while [ $i -lt 20 ]; do
		echo "The quick brown fox jumps over the lazy dog. Line $i of test data." >> datafiles/text.dat
		i=$((i + 1))
	done
fi

if [ ! -f datafiles/random.dat ]; then
	dd if=/dev/urandom of=datafiles/random.dat bs=1024 count=256 2>/dev/null
fi

if [ ! -f datafiles/zeros.dat ]; then
	dd if=/dev/zero of=datafiles/zeros.dat bs=1024 count=256 2>/dev/null
fi

if [ ! -f datafiles/mixed.dat ]; then
	cat datafiles/text.dat datafiles/random.dat datafiles/zeros.dat > datafiles/mixed.dat
fi

# Test each algorithm at representative levels
for algo in lzfx lz4 zlib bzip2 lzma ppmd libbsc zstd none; do
	if ! algo_available "$algo"; then
		log_skip "$algo (not compiled in)"
		continue
	fi

	echo ""
	echo "--- Algorithm: $algo ---"

	for tf in datafiles/text.dat datafiles/random.dat datafiles/zeros.dat datafiles/mixed.dat; do
		tname=$(basename $tf .dat)

		for level in 1 6 14; do
			for seg in 64k 1m; do
				label="${algo}/${tname}/l${level}/s${seg}"
				rm -f ${tf}.pz ${tf}.out

				$PCOMPRESS -c ${algo} -l ${level} -s ${seg} ${tf} 2>/dev/null
				if [ $? -ne 0 ]; then
					log_fail "${label}: compression failed"
					rm -f ${tf}.pz
					continue
				fi

				$PCOMPRESS -d ${tf}.pz ${tf}.out 2>/dev/null
				if [ $? -ne 0 ]; then
					log_fail "${label}: decompression failed"
					rm -f ${tf}.pz ${tf}.out
					continue
				fi

				if diff -q ${tf} ${tf}.out > /dev/null 2>&1; then
					log_pass "${label}"
				else
					log_fail "${label}: data mismatch"
				fi
				rm -f ${tf}.pz ${tf}.out
			done
		done
	done
done

# Real text through libbsc's LZP stage at the levels that use its small hash
# tables (-l1 and -l3). Repetitive synthetic text never reaches the match
# extension code; C source does. GCC 16 miscompiled that code (aligned
# vector loads on unaligned data) until 4.2.1, and this segfaulted.
if algo_available libbsc; then
	src=$(cd "$(dirname "$0")/../.." && pwd)
	srctxt=datafiles/source.dat
	find "$src" -maxdepth 1 -name '*.c' -size -400k | LC_ALL=C sort | xargs cat > "$srctxt" 2>/dev/null
	while [ "$(wc -c < "$srctxt")" -lt 4000000 ]; do cat "$srctxt" "$srctxt" > "$srctxt.2" && mv "$srctxt.2" "$srctxt"; done
	for level in 1 3; do
		label="libbsc/source-text/l${level}"
		rm -f "$srctxt.pz" "$srctxt.out"
		if $PCOMPRESS -c libbsc -l ${level} -t 1 -s 256k "$srctxt" 2>/dev/null &&
		    $PCOMPRESS -d "$srctxt.pz" "$srctxt.out" 2>/dev/null && cmp -s "$srctxt" "$srctxt.out"; then
			log_pass "$label"
		else
			log_fail "$label: round trip failed (status $?)"
		fi
		rm -f "$srctxt.pz" "$srctxt.out"
	done
	rm -f "$srctxt"
fi

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
# Nothing ran: treat as failure so a broken harness cannot pass silently.
if [ $PASS -eq 0 ]; then
	echo "ERROR: no test cases ran"
	exit 1
fi
exit 0
