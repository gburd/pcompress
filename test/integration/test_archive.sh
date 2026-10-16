#!/bin/sh
#
# Integration test: Archive mode compression and decompression.
# Tests the -a (archive) mode with various algorithm and feature combinations.
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
		# The archive stores the path as given on the command line (like tar).
		all_ok=1
		for f in file1.txt file2.txt binary.bin subdir/nested.txt subdir/zeros.bin; do
			if [ ! -f "$OUTDIR/$ARCDIR/$f" ]; then
				log_fail "${label}: missing $f"
				all_ok=0
				break
			fi
			if ! diff -q "$ARCDIR/$f" "$OUTDIR/$ARCDIR/$f" > /dev/null 2>&1; then
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
echo "--- Listing (-i) ---"
# With a metadata stream (the default) -i reads only metadata, on a second
# fd; closing that fd too early once made the listing empty.
for t in "" "-T"; do
	label="list${t:+/$t}"
	rm -f "${ARCDIR}.pz"
	if $PCOMPRESS -a -c lz4 -l1 $t "$ARCDIR" 2>/dev/null &&
	    $PCOMPRESS -i "${ARCDIR}.pz" 2>/dev/null | grep -q 'subdir/nested.txt'; then
		log_pass "$label"
	else
		log_fail "$label: member missing from -i output"
	fi
done
rm -f "${ARCDIR}.pz"

echo ""
echo "--- -j applies packJPG ---"
# -j was ignored in 4.0.x (filters were set only for the level-based choice).
JPG="$(cd "$(dirname "$0")/.." && pwd)/res/jpg/screen.jpg"
JDIR="datafiles/jtest"
wipe() { [ -e "$1" ] && find "$1" -depth -delete; :; }
if [ -f "$JPG" ]; then
	wipe "$JDIR"; wipe datafiles/jout; rm -f "$JDIR.pz"
	mkdir -p "$JDIR" datafiles/jout && cp "$JPG" "$JDIR/"
	$PCOMPRESS -a -c lz4 -l1 "$JDIR" 2>/dev/null; plain=$(wc -c < "$JDIR.pz")
	rm -f "$JDIR.pz"
	$PCOMPRESS -a -c lz4 -l1 -j "$JDIR" 2>/dev/null; filt=$(wc -c < "$JDIR.pz")
	if [ "$filt" -lt "$plain" ] &&
	    $PCOMPRESS -d "$JDIR.pz" datafiles/jout 2>/dev/null &&
	    cmp -s "$JPG" "datafiles/jout/$JDIR/screen.jpg"; then
		log_pass "-j filter ($plain -> $filt bytes, restored exactly)"
	else
		log_fail "-j filter: plain=$plain filtered=$filt or restore differs"
	fi
	wipe "$JDIR"; wipe datafiles/jout; rm -f "$JDIR.pz"
else
	log_skip "-j filter (no test JPEG)"
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
