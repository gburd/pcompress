#!/bin/sh
#
# Integration test: Multi-threaded compression.
# Tests that pcompress correctly handles parallel chunk processing
# with -t flag for various thread counts and algorithms.
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
echo " Integration: Multi-Threaded Compression"
echo "========================================="

if [ ! -d datafiles ]; then
	mkdir datafiles
fi

# Need a file large enough that multiple chunks are created
# With 64k chunks, 1 MiB file = 16 chunks
if [ ! -f datafiles/mt_test.dat ]; then
	dd if=/dev/urandom of=datafiles/mt_base.tmp bs=1024 count=64 2>/dev/null
	cp datafiles/mt_base.tmp datafiles/mt_test.dat
	i=0
	while [ $i -lt 15 ]; do
		cat datafiles/mt_base.tmp >> datafiles/mt_test.dat
		i=$((i + 1))
	done
	rm -f datafiles/mt_base.tmp
fi

tf="datafiles/mt_test.dat"

for algo in lz4 zlib lzma zstd; do
	if ! algo_available "$algo"; then
		log_skip "${algo} (not compiled in)"
		continue
	fi

	echo ""
	echo "--- ${algo}: thread counts ---"

	for threads in 1 2 4; do
		label="${algo}/t${threads}"
		rm -f ${tf}.pz ${tf}.out

		$PCOMPRESS -c ${algo} -l 3 -s 64k -t ${threads} ${tf} 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: compression failed"
			rm -f ${tf}.pz
			continue
		fi

		$PCOMPRESS -d -t ${threads} ${tf}.pz ${tf}.out 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: decompression failed"
			rm -f ${tf}.pz ${tf}.out
			continue
		fi

		if diff -q ${tf} ${tf}.out > /dev/null 2>&1; then
			log_pass "${label}: round-trip OK"
		else
			log_fail "${label}: data mismatch"
		fi
		rm -f ${tf}.pz ${tf}.out
	done
done

# Test that single-threaded and multi-threaded decompression
# produce identical results (cross-validate).
echo ""
echo "--- Cross-thread decompression validation ---"
for algo in lz4 zlib; do
	if ! algo_available "$algo"; then
		continue
	fi

	label="${algo}/cross-thread"
	rm -f ${tf}.pz ${tf}.out1 ${tf}.out4

	# Compress with 4 threads
	$PCOMPRESS -c ${algo} -l 3 -s 64k -t 4 ${tf} 2>/dev/null
	if [ $? -ne 0 ]; then
		log_fail "${label}: compression failed"
		rm -f ${tf}.pz
		continue
	fi

	# Decompress with 1 thread
	$PCOMPRESS -d -t 1 ${tf}.pz ${tf}.out1 2>/dev/null
	rc1=$?

	# Need to re-compress since .pz was consumed
	rm -f ${tf}.pz
	$PCOMPRESS -c ${algo} -l 3 -s 64k -t 4 ${tf} 2>/dev/null

	# Decompress with 4 threads
	$PCOMPRESS -d -t 4 ${tf}.pz ${tf}.out4 2>/dev/null
	rc4=$?

	if [ $rc1 -ne 0 ] || [ $rc4 -ne 0 ]; then
		log_fail "${label}: decompression failed (rc1=$rc1 rc4=$rc4)"
	elif diff -q ${tf}.out1 ${tf}.out4 > /dev/null 2>&1; then
		if diff -q ${tf} ${tf}.out1 > /dev/null 2>&1; then
			log_pass "${label}: 1-thread and 4-thread output match original"
		else
			log_fail "${label}: outputs match each other but not original"
		fi
	else
		log_fail "${label}: 1-thread and 4-thread outputs differ"
	fi
	rm -f ${tf}.pz ${tf}.out1 ${tf}.out4
done

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
exit 0
