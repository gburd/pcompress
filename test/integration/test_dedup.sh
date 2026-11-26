#!/bin/sh
#
# Integration test: Deduplication round-trip.
# Tests Rabin (variable-block) and fixed-block deduplication modes
# with various compression algorithms to verify that deduplicated
# data round-trips correctly.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
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
PWFILE="${TMPDIR:-/tmp}/pcompress_dedup_pw"
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

# Every content-defined mode, threads 1 and 16, plain and encrypted, on a
# file of 40 copies of a 64 KB random block with random gaps. With no
# compressor only dedupe can shrink it, and only if the block boundaries
# fall at the same places in each copy.
echo ""
echo "--- Dedupe modes, threads, encryption ---"
rf="datafiles/dedup_rep.dat"
if [ ! -f "$rf" ]; then
	dd if=/dev/urandom of=datafiles/dedup_rep.tmp bs=4096 count=16 2>/dev/null
	i=0
	while [ $i -lt 40 ]; do
		cat datafiles/dedup_rep.tmp
		dd if=/dev/urandom bs=1000 count=3 2>/dev/null
		i=$((i + 1))
	done > "$rf"
	rm -f datafiles/dedup_rep.tmp
fi
rsz=$(wc -c < "$rf")
for mode in "-D" "-G" "-D -E" "-D -EE"; do
	for t in 1 16; do
		for enc in "" "-e AES"; do
			label="${mode} -t${t} ${enc:-plain}"
			pw=""
			[ -n "$enc" ] && pw="-w $PWFILE"
			rm -f ${rf}.pz ${rf}.out
			# pcompress zeroes a password file once read.
			echo "deduptestpassword" > "$PWFILE"
			# shellcheck disable=SC2086
			if ! $PCOMPRESS -c none -l 1 -s 4m $mode -t $t $enc $pw \
			    ${rf} 2>/dev/null; then
				log_fail "${label}: compression failed"
				continue
			fi
			csz=$(wc -c < ${rf}.pz)
			echo "deduptestpassword" > "$PWFILE"
			# shellcheck disable=SC2086
			if ! $PCOMPRESS -d $pw ${rf}.pz ${rf}.out 2>/dev/null; then
				log_fail "${label}: decompression failed"
			elif ! cmp -s ${rf} ${rf}.out; then
				log_fail "${label}: data mismatch"
			elif [ $((csz * 2)) -gt "$rsz" ]; then
				log_fail "${label}: ${csz} of ${rsz} bytes: repeats not found"
			else
				log_pass "${label}: round-trip OK, ${csz} of ${rsz} bytes"
			fi
			rm -f ${rf}.pz ${rf}.out
		done
	done
done

# Delta encoding must pay: 40 copies of a 64 KB random block, each with 16
# scattered 2-byte edits, so no block repeats exactly but every block has
# a similar one. Without a compressor -D -EE must come out smaller than -D.
nf="datafiles/dedup_near.dat"
if [ ! -f "$nf" ]; then
	dd if=/dev/urandom of=datafiles/dedup_near.tmp bs=4096 count=16 2>/dev/null
	: > "$nf"
	c=0
	while [ $c -lt 40 ]; do
		cp datafiles/dedup_near.tmp datafiles/dedup_near.cp
		k=0
		while [ $k -lt 16 ]; do
			printf '%02x' $c | dd of=datafiles/dedup_near.cp bs=1 conv=notrunc \
			    seek=$((k * 4096 + (c * 997 + k * 131) % 4000)) 2>/dev/null
			k=$((k + 1))
		done
		cat datafiles/dedup_near.cp >> "$nf"
		c=$((c + 1))
	done
	rm -f datafiles/dedup_near.tmp datafiles/dedup_near.cp
fi
rm -f ${nf}.pz ${nf}.out
if $PCOMPRESS -c none -l 1 -s 1m -D ${nf} 2>/dev/null; then
	dsz=$(wc -c < ${nf}.pz)
	rm -f ${nf}.pz
	if $PCOMPRESS -c none -l 1 -s 1m -D -EE ${nf} 2>/dev/null &&
	    $PCOMPRESS -d ${nf}.pz ${nf}.out 2>/dev/null && cmp -s ${nf} ${nf}.out; then
		esz=$(wc -c < ${nf}.pz)
		if [ "$esz" -lt "$dsz" ]; then
			log_pass "-D -EE: ${esz} bytes, -D ${dsz}"
		else
			log_fail "-D -EE: ${esz} bytes, not below -D's ${dsz}"
		fi
	else
		log_fail "-D -EE: round-trip"
	fi
else
	log_fail "-D: compression of ${nf} failed"
fi
rm -f ${nf}.pz ${nf}.out

# Archive mode with dedupe: two copies of the file in one directory.
ADIR=datafiles/dedup_arc
rm -rf "$ADIR" "$ADIR.pz" datafiles/dedup_arc_out
mkdir -p "$ADIR" datafiles/dedup_arc_out && cp "$rf" "$ADIR/a" && cp "$rf" "$ADIR/b"
if $PCOMPRESS -a -c zstd -l 3 -s 4m -D -E "$ADIR" 2>/dev/null &&
    $PCOMPRESS -d "$ADIR.pz" datafiles/dedup_arc_out 2>/dev/null &&
    cmp -s "$rf" datafiles/dedup_arc_out/$ADIR/b; then
	log_pass "archive -D -E: round-trip OK"
else
	log_fail "archive -D -E: round-trip"
fi
rm -rf "$ADIR" "$ADIR.pz" datafiles/dedup_arc_out
rm -f "$PWFILE"

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
