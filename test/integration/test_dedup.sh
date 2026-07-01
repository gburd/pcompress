#!/bin/sh
#
# Integration test: Deduplication round-trip.
# Tests Rabin (variable-block) and fixed-block deduplication modes
# with various compression algorithms to verify that deduplicated
# data round-trips correctly.
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
echo " Integration: Deduplication Round-Trip"
echo "========================================="

if [ ! -d datafiles ]; then
	mkdir datafiles
fi

# Generate test data with duplicate content (essential for dedup testing)
if [ ! -f datafiles/dedup_test.dat ]; then
	# Create a base block
	dd if=/dev/urandom of=datafiles/dedup_base.tmp bs=4096 count=16 2>/dev/null
	# Replicate it multiple times with some unique sections
	cp datafiles/dedup_base.tmp datafiles/dedup_test.dat
	cat datafiles/dedup_base.tmp >> datafiles/dedup_test.dat
	dd if=/dev/urandom bs=4096 count=4 2>/dev/null >> datafiles/dedup_test.dat
	cat datafiles/dedup_base.tmp >> datafiles/dedup_test.dat
	cat datafiles/dedup_base.tmp >> datafiles/dedup_test.dat
	dd if=/dev/urandom bs=4096 count=8 2>/dev/null >> datafiles/dedup_test.dat
	cat datafiles/dedup_base.tmp >> datafiles/dedup_test.dat
	rm -f datafiles/dedup_base.tmp
fi

tf="datafiles/dedup_test.dat"

for algo in lz4 zlib lzma zstd; do
	if ! algo_available "$algo"; then
		log_skip "${algo} (not compiled in)"
		continue
	fi

	echo ""
	echo "--- ${algo} + Deduplication ---"

	# Test with Rabin (variable-block) dedup
	for dedup_flag in "-D" "-F"; do
		if [ "$dedup_flag" = "-D" ]; then
			dedup_name="rabin"
		else
			dedup_name="fixed"
		fi
		label="${algo}/${dedup_name}"
		rm -f ${tf}.pz ${tf}.out

		$PCOMPRESS -c ${algo} -l 3 -s 1m ${dedup_flag} ${tf} 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: dedup compression failed"
			rm -f ${tf}.pz
			continue
		fi

		$PCOMPRESS -d ${tf}.pz ${tf}.out 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: dedup decompression failed"
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

# Test dedup with encryption
echo ""
echo "--- Dedup + Encryption ---"
PWFILE="$TMPDIR/pcompress_dedup_pw"
for algo in lz4 zlib; do
	if ! algo_available "$algo"; then
		continue
	fi
	label="${algo}/rabin/AES"
	rm -f ${tf}.pz ${tf}.out

	echo "deduptestpassword" > "$PWFILE"
	$PCOMPRESS -c ${algo} -l 3 -s 1m -D -e AES -w "$PWFILE" ${tf} 2>/dev/null
	if [ $? -ne 0 ]; then
		log_fail "${label}: dedup+crypto compression failed"
		rm -f ${tf}.pz
		continue
	fi

	echo "deduptestpassword" > "$PWFILE"
	$PCOMPRESS -d -w "$PWFILE" ${tf}.pz ${tf}.out 2>/dev/null
	if [ $? -ne 0 ]; then
		log_fail "${label}: dedup+crypto decompression failed"
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
rm -f "$PWFILE"

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
exit 0
