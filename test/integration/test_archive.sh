#!/bin/sh
#
# Integration test: Archive mode compression and decompression.
# Tests the -a (archive) mode with various algorithm and feature combinations.
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
echo " Integration: Archive Mode"
echo "========================================="

# Create a test directory tree for archiving
ARCDIR="datafiles/arctest"
OUTDIR="datafiles/arcout"

if [ ! -d "$ARCDIR" ]; then
	mkdir -p "$ARCDIR/subdir"
	echo "File one contents" > "$ARCDIR/file1.txt"
	echo "File two contents with more data" > "$ARCDIR/file2.txt"
	dd if=/dev/urandom of="$ARCDIR/binary.bin" bs=1024 count=64 2>/dev/null
	echo "Nested file" > "$ARCDIR/subdir/nested.txt"
	dd if=/dev/zero of="$ARCDIR/subdir/zeros.bin" bs=1024 count=32 2>/dev/null
fi

for algo in lz4 zlib lzma zstd; do
	if ! algo_available "$algo"; then
		log_skip "archive/$algo (not compiled in)"
		continue
	fi

	echo ""
	echo "--- Archive with $algo ---"

	for level in 1 6; do
		label="archive/${algo}/l${level}"
		rm -f "${ARCDIR}.pz"
		rm -rf "$OUTDIR"

		$PCOMPRESS -a -c ${algo} -l ${level} -s 1m "$ARCDIR" 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: archive compression failed"
			rm -f "${ARCDIR}.pz"
			continue
		fi

		mkdir -p "$OUTDIR"
		$PCOMPRESS -d "${ARCDIR}.pz" "$OUTDIR" 2>/dev/null
		if [ $? -ne 0 ]; then
			log_fail "${label}: archive decompression failed"
			rm -f "${ARCDIR}.pz"
			rm -rf "$OUTDIR"
			continue
		fi

		# Verify extracted files match originals
		all_ok=1
		for f in file1.txt file2.txt binary.bin subdir/nested.txt subdir/zeros.bin; do
			if [ ! -f "$OUTDIR/arctest/$f" ]; then
				log_fail "${label}: missing $f"
				all_ok=0
				break
			fi
			if ! diff -q "$ARCDIR/$f" "$OUTDIR/arctest/$f" > /dev/null 2>&1; then
				log_fail "${label}: $f content mismatch"
				all_ok=0
				break
			fi
		done

		if [ $all_ok -eq 1 ]; then
			log_pass "${label}"
		fi
		rm -f "${ARCDIR}.pz"
		rm -rf "$OUTDIR"
	done
done

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
exit 0
