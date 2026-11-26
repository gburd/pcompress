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
echo "--- Sparse file ---"
# A sparse file's map is archived as part of its data; with the metadata
# stream (the default) extraction once failed with "Line too long".
SDIR="datafiles/sparsetest"
swipe() { [ -e "$1" ] && find "$1" -depth -delete; :; }
swipe "$SDIR"; rm -f "$SDIR.pz"
mkdir -p "$SDIR"
dd if=/dev/zero of="$SDIR/holes.bin" bs=1 count=0 seek=8388608 2>/dev/null
printf 'end' >> "$SDIR/holes.bin"
for t in "" "-T"; do
	label="sparse${t:+/$t}"
	rm -f "$SDIR.pz"; swipe datafiles/sout; mkdir -p datafiles/sout
	if $PCOMPRESS -a -c lz4 -l1 $t "$SDIR" 2>/dev/null &&
	    $PCOMPRESS -d "$SDIR.pz" datafiles/sout 2>/dev/null &&
	    cmp -s "$SDIR/holes.bin" "datafiles/sout/$SDIR/holes.bin"; then
		log_pass "$label"
	else
		log_fail "$label: sparse file not restored"
	fi
done
swipe "$SDIR"; swipe datafiles/sout; rm -f "$SDIR.pz"

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
echo "--- -x: ARM64 and RISC-V branch filters ---"
# A member whose ELF header says ARM64 (e_machine 183) or RISC-V (243)
# goes through the BCJ filter, and the archive becomes version 13; one
# with no such member stays version 10. Header bytes 8-9 hold the version.
BDIR="datafiles/bcjtest"
# One byte at a time: BSD od (macOS) prints -N2 output differently, and
# zero-padded ("011") values must not be read as octal.
hdr_byte() { echo $(( $(dd if="$1" bs=1 skip="$2" count=1 2>/dev/null | od -An -tu1 | tr -d ' \n' | sed 's/^0*\(.\)/\1/') )); }
hdr_version() { echo $(( $(hdr_byte "$1" 8) * 256 + $(hdr_byte "$1" 9) )); }
mk_elf() { # file e_machine: an ELF header, then calls to a few functions
	python3 - "$1" "$2" <<'PYEOF'
import random, struct, sys
# Calls from all over the file to 16 targets: relative offsets differ at
# every site, absolute ones repeat, which is what the filter is for.
r = random.Random(1)
hdr = b"\x7fELF\x02\x01\x01" + bytes(9) + struct.pack("<HH", 3, int(sys.argv[2]))
body = bytearray(hdr + bytes(512))
fns = [r.randrange(0, 1 << 17) * 4 for i in range(16)]
while len(body) < 200000:
	pc, t = len(body), r.choice(fns)
	if sys.argv[2] == "183":	# ARM64 BL
		body += struct.pack("<I", 0x94000000 | ((t - pc) >> 2) & 0x3ffffff)
	else:				# RISC-V JAL ra
		d = (t - pc) & 0x1fffff
		body += struct.pack("<I", 0xef | 1 << 7 | (d >> 12 & 0xff) << 12 |
		    (d >> 11 & 1) << 20 | (d >> 1 & 0x3ff) << 21 | (d >> 20 & 1) << 31)
	body += struct.pack("<I", r.randrange(1 << 32) & 0x0000ffff)
open(sys.argv[1], "wb").write(body)
PYEOF
}
wipe "$BDIR"; wipe datafiles/bout; rm -f "$BDIR.pz"
mkdir -p "$BDIR" datafiles/bout
if ! command -v python3 >/dev/null 2>&1; then
	log_skip "-x BCJ (no python3)"
else
mk_elf "$BDIR/arm.so" 183
mk_elf "$BDIR/rv.so" 243
if $PCOMPRESS -a -c zstd -l 6 -x "$BDIR" 2>/dev/null &&
    [ "$(hdr_version "$BDIR.pz")" = 13 ] &&
    $PCOMPRESS -d "$BDIR.pz" datafiles/bout 2>/dev/null &&
    cmp -s "$BDIR/arm.so" "datafiles/bout/$BDIR/arm.so" &&
    cmp -s "$BDIR/rv.so" "datafiles/bout/$BDIR/rv.so"; then
	log_pass "-x on ARM64/RISC-V members: version 13, restored exactly"
else
	log_fail "-x on ARM64/RISC-V members"
fi
filt=$(wc -c < "$BDIR.pz"); rm -f "$BDIR.pz"
$PCOMPRESS -a -c zstd -l 6 "$BDIR" 2>/dev/null
if [ "$filt" -lt "$(wc -c < "$BDIR.pz")" ]; then
	log_pass "-x BCJ: archive smaller ($filt bytes)"
else
	log_fail "-x BCJ: archive not smaller ($filt vs $(wc -c < "$BDIR.pz"))"
fi
rm -f "$BDIR.pz" "$BDIR/arm.so" "$BDIR/rv.so"
mk_elf "$BDIR/x86.so" 62
if $PCOMPRESS -a -c zstd -l 6 -x "$BDIR" 2>/dev/null &&
    [ "$(hdr_version "$BDIR.pz")" = 10 ]; then
	log_pass "-x with no ARM64/RISC-V member: still version 10"
else
	log_fail "-x with no ARM64/RISC-V member: version $(hdr_version "$BDIR.pz")"
fi
fi
wipe "$BDIR"; wipe datafiles/bout; rm -f "$BDIR.pz"

# The tar stream's zero padding after the end-of-archive blocks can land in
# a chunk of its own; extraction stops at the end blocks and must accept it.
PDIR="datafiles/padtest"
wipe "$PDIR"; wipe datafiles/pout; rm -f "$PDIR.pz"
mkdir -p "$PDIR" datafiles/pout
seq 1 100000 > "$PDIR/a"; echo hi > "$PDIR/b"
if $PCOMPRESS -a -c lz4 -s 64k "$PDIR" 2>/dev/null &&
    $PCOMPRESS -d "$PDIR.pz" datafiles/pout 2>/dev/null &&
    cmp -s "$PDIR/a" "datafiles/pout/$PDIR/a" && cmp -s "$PDIR/b" "datafiles/pout/$PDIR/b"; then
	log_pass "archive whose last chunk is tar padding extracts with status 0"
else
	log_fail "archive whose last chunk is tar padding"
fi
wipe "$PDIR"; wipe datafiles/pout; rm -f "$PDIR.pz"

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
