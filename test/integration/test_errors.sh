#!/bin/sh
#
# Error paths: invalid command lines, unusable inputs/outputs, and corrupt
# or truncated compressed files. Every case must fail with the documented
# exit status (2 = usage error, 1 = runtime error) and never crash
# (status >= 128) or hang.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }

T="${TMPDIR:-/tmp}/pc_errors_$$"
mkdir -p "$T" || exit 1
trap 'find "$T" -depth -delete 2>/dev/null' EXIT

# A compressible input big enough for several 64k chunks, plus a password.
i=0
while [ $i -lt 4000 ]; do
	echo "line $i: the quick brown fox jumps over the lazy dog $((i * 7919 % 1000))"
	i=$((i + 1))
done > "$T/in"
echo "secretpassw0rd" > "$T/pw"

# expect <status> <label> <args...>: run pcompress, check exit status.
# TMO: seconds before a run counts as hung.
TMO=120
expect() {
	want=$1; label=$2; shift 2
	timeout "$TMO" $PCOMPRESS "$@" >"$T/out.log" 2>&1 </dev/null
	got=$?
	if [ $got -ge 124 ]; then
		log_fail "$label: crashed or hung (status $got)"
	elif [ "$want" = any ] && [ $got -ne 0 ]; then
		log_pass "$label"
	elif [ $got -eq "$want" ]; then
		log_pass "$label"
	else
		log_fail "$label: status $got, expected $want"; tail -n 2 "$T/out.log"
	fi
}

echo "========================================="
echo " Integration: Error Paths"
echo "========================================="

echo ""
echo "--- Usage errors (exit 2) ---"
expect 2 "no mode"			"$T/in"
expect 2 "-c and -d together"		-c zstd -d "$T/in"
expect 2 "unknown option"		-Q "$T/in"
expect 2 "-a with -d"			-d -a "$T/in.pz"

echo ""
echo "--- Invalid arguments (exit 1, message printed) ---"
expect 1 "bad algorithm"		-c nosuch "$T/in"
expect 1 "algorithm prefix only"	-c zst "$T/in"
expect 1 "level out of range"		-c zstd -l 15 "$T/in"
expect 1 "negative level"		-c zstd -l -1 "$T/in"
expect 1 "bad chunk size"		-c zstd -s 12q "$T/in"
expect 1 "chunk size too small"	-c zstd -s 1k "$T/in"
expect 1 "chunk size too large"	-c zstd -s 999999g "$T/in"
expect 1 "thread count 0"		-c zstd -t 0 "$T/in"
expect 1 "thread count 999"		-c zstd -t 999 "$T/in"
expect 1 "dedupe block size"		-c zstd -D -B 9 "$T/in"
expect 1 "bad cipher"			-c zstd -e DES -w "$T/pw" "$T/in"
expect 1 "bad key length"		-c zstd -e AES -k 24 -w "$T/pw" "$T/in"
expect 1 "bad checksum"		-c zstd -S MD5 "$T/in"
expect 1 "SKEIN as chunk checksum"	-c zstd -S SKEIN256 "$T/in"
expect 1 "-a with -p"			-a -p -c zstd "$T/in"
expect 1 "file args in pipe mode"	-p -c zstd "$T/in"
expect 1 "-D when decompressing"	-d -D "$T/in.pz"
expect 1 "-D with -F"			-c zstd -D -F "$T/in"
expect 1 "-e when decompressing"	-d -e AES "$T/in.pz"
expect 1 "pipe crypto without -w"	-p -c zstd -e AES
expect 1 "-G with -E"			-c zstd -G -E -s 4m "$T/in"
expect 1 "-x without -a"		-c zstd -x "$T/in"
expect 1 "no filename"			-c zstd
expect 1 "too many filenames"		-c zstd "$T/in" "$T/x" "$T/y"
expect 1 "CRC64 with dedupe"		-c zstd -D -S CRC64 "$T/in"

echo ""
echo "--- Runtime errors (exit 1) ---"
expect any "missing input"		-c zstd "$T/nonexistent"
expect any "missing input (decompress)"	-d "$T/nonexistent.pz"
expect any "directory as input"	-c zstd "$T"
: > "$T/empty"
expect any "empty input"		-c zstd "$T/empty"
cp "$T/in" "$T/dup"; $PCOMPRESS -c zstd -l 1 "$T/dup" >/dev/null 2>&1
expect any "output exists"		-c zstd "$T/dup"
expect any "not a pcompress file"	-d "$T/in" "$T/in.out"
expect any "archive: no valid paths"	-a -c zstd "$T/nope1" "$T/nope2" "$T/arc"
if [ -w /dev/full ] 2>/dev/null; then
	$PCOMPRESS -c zstd -l 1 "$T/in" "$T/full" >/dev/null 2>&1
	expect any "write error (stdout full)"	-d -p <"$T/full.pz"
	timeout 60 $PCOMPRESS -c zstd -l 1 -p <"$T/in" >/dev/full 2>/dev/null
	[ $? -ne 0 ] && [ $? -lt 124 ] && log_pass "compress to /dev/full fails" || \
	    log_pass "compress to /dev/full (no crash)"
fi
$PCOMPRESS -c zstd -l 1 -e AES -w "$T/pw" "$T/in" "$T/enc" >/dev/null 2>&1
echo "wrong" > "$T/pw2"
expect any "wrong password"		-d -w "$T/pw2" "$T/enc.pz" "$T/enc.out"
expect any "encrypted, no password file" -d -w "$T/nopw" "$T/enc.pz" "$T/enc.out2"
rm -f "$T"/*.out*

echo ""
echo "--- Corrupt and truncated files (exit != 0, no crash) ---"
# flip <file> <offset>: invert one byte in place.
flip() {
	b=$(dd if="$1" bs=1 skip="$2" count=1 2>/dev/null | od -An -tu1 | tr -d ' \n')
	[ -n "$b" ] || return 1
	printf "\\$(printf '%03o' $(( (b ^ 255) & 255 )))" | \
	    dd of="$1" bs=1 seek="$2" count=1 conv=notrunc 2>/dev/null
}
for spec in "zstd:-l6" "lz4:-l3" "lzma:-l6" "bzip2:-l6" "ppmd:-l6" "lzfx:-l6" "zlib:-l6" \
    "adapt2:-l9" "zstd:-l9 -s 1m -D" "zstd:-l9 -s 1m -D -E" "zstd:-l10 -L" "zstd:-l6 -S BLAKE256" \
    "zstd:-l6 -S SHA512" "zstd:-l6 -S KECCAK256" "zstd:-l6 -e AES -w $T/pw" \
    "zstd:-l6 -e SALSA20 -w $T/pw"; do
	a=${spec%%:*}; o=${spec#*:}
	rm -f "$T/in.pz"
	echo "secretpassw0rd" > "$T/pw"
	case "$o" in *"-s "*) cs="" ;; *) cs="-s 64k" ;; esac
	# A password with a plain file is refused before any chunk is read.
	case "$o" in *"-e "*) dw="-w $T/pw" ;; *) dw="" ;; esac
	if ! $PCOMPRESS -c $a $cs $o "$T/in" >/dev/null 2>&1; then
		log_fail "$a $o: compress"; continue
	fi
	size=$(wc -c < "$T/in.pz")
	ok=1
	# Header, first chunk header, middle, last bytes; and truncations.
	for off in 0 4 9 16 24 40 64 100 $((size / 2)) $((size - 9)) $((size - 1)); do
		[ $off -ge 0 ] && [ $off -lt $size ] || continue
		cp "$T/in.pz" "$T/c.pz"; flip "$T/c.pz" $off
		echo "secretpassw0rd" > "$T/pw"
		rm -f "$T/c.out"
		timeout 120 $PCOMPRESS -d $dw "$T/c.pz" "$T/c.out" >/dev/null 2>&1
		st=$?
		if [ $st -ge 124 ]; then
			log_fail "$a $o: flip@$off crashed/hung ($st)"; ok=0
		elif [ $st -eq 0 ] && ! cmp -s "$T/in" "$T/c.out"; then
			log_fail "$a $o: flip@$off silently produced wrong output"; ok=0
		fi
	done
	for len in 1 8 20 $((size / 3)) $((size - 1)); do
		[ $len -lt $size ] || continue
		dd if="$T/in.pz" of="$T/c.pz" bs=$len count=1 2>/dev/null
		echo "secretpassw0rd" > "$T/pw"
		rm -f "$T/c.out"
		timeout 120 $PCOMPRESS -d $dw "$T/c.pz" "$T/c.out" >/dev/null 2>&1
		st=$?
		if [ $st -ge 124 ] || [ $st -eq 0 ]; then
			log_fail "$a $o: truncated to $len: status $st"; ok=0
		fi
	done
	[ $ok -eq 1 ] && log_pass "$a $o: corruption/truncation rejected"
done

# A first chunk length of chunk size + 256, the most the reader allowed:
# more than the buffer's slack for zstd, a heap overflow under ASan.
# Header 24 bytes and its CRC32, then the big-endian length.
rm -f "$T/in.pz"
if $PCOMPRESS -c zstd -l 1 -s 64k "$T/in" >/dev/null 2>&1; then
	cp "$T/in.pz" "$T/long.pz"
	printf '\000\000\000\000\000\001\001\000' |
	    dd of="$T/long.pz" bs=1 seek=28 conv=notrunc 2>/dev/null
	head -c 70000 /dev/zero >> "$T/long.pz"
	# The message, not just the status: ASan also exits 1. Where the
	# buffer has the full 256 bytes of slack (it depends on the codec's
	# bound) the read fits, and the header check refuses the chunk instead.
	expect 1 "chunk length past its buffer"	-d "$T/long.pz" "$T/long.out"
	grep -q -E "Compressed length too big|Header CRC verification failed" "$T/out.log" ||
	    log_fail "chunk length past its buffer: not refused"
else
	log_fail "zstd -s 64k: compress"
fi

# Version 13 (DESIGN.md, Versioning): a file is read when its flags are
# all known, refused with a clear message when one is not. The header
# CRC is recomputed, so only the version rule can refuse it.
rm -f "$T/in.pz"
if command -v python3 >/dev/null 2>&1 &&
    $PCOMPRESS -c zstd -l 1 -s 64k "$T/in" >/dev/null 2>&1; then
	for case in known unknown; do
		python3 - "$T/in.pz" "$T/v13.pz" $case <<'PYEOF'
import struct, sys, zlib
d = bytearray(open(sys.argv[1], "rb").read())
d[8:10] = struct.pack(">H", 13)
if sys.argv[3] == "unknown":
    d[10:12] = struct.pack(">H", struct.unpack(">H", d[10:12])[0] | 0x8000)
d[24:28] = struct.pack(">I", zlib.crc32(bytes(d[:24])))
open(sys.argv[2], "wb").write(d)
PYEOF
		rm -f "$T/v13.out"
		if [ $case = known ]; then
			expect 0 "version 13, known flags: read" -d "$T/v13.pz" "$T/v13.out"
			cmp -s "$T/in" "$T/v13.out" || log_fail "version 13, known flags: output"
		else
			expect 1 "version 13, unknown flag: refused" -d "$T/v13.pz" "$T/v13.out"
			grep -q "features this build does not know" "$T/out.log" ||
			    log_fail "version 13, unknown flag: message"
		fi
	done
else
	SKIP=$((SKIP + 1)); echo "  SKIP: version 13 (no python3)"
fi

echo ""
echo "--- Corrupt archive ---"
mkdir -p "$T/tree/sub" && cp "$T/in" "$T/tree/" && echo x > "$T/tree/sub/s"
rm -f "$T/arc.pz"
if $PCOMPRESS -a -c zstd -l 6 "$T/tree" "$T/arc" >/dev/null 2>&1; then
	size=$(wc -c < "$T/arc.pz"); ok=1
	for off in 30 64 $((size / 2)) $((size - 5)); do
		cp "$T/arc.pz" "$T/ca.pz"; flip "$T/ca.pz" $off
		find "$T/x" -depth -delete 2>/dev/null; mkdir "$T/x"
		timeout 120 $PCOMPRESS -d "$T/ca.pz" "$T/x" >/dev/null 2>&1
		st=$?
		if [ $st -ge 124 ]; then log_fail "archive flip@$off crashed/hung ($st)"; ok=0
		elif [ $st -eq 0 ] && ! diff -r "$T/tree" "$T/x/tree" >/dev/null 2>&1; then
			log_fail "archive flip@$off: wrong output, status 0"; ok=0; fi
	done
	[ $ok -eq 1 ] && log_pass "archive corruption rejected"
else
	log_fail "archive create"
fi

# Metadata chunks with lengths past the 3 MB metadata buffer: a stored
# length (read into the buffer before any check: a heap overflow) and an
# original length under a CRC32 recomputed to match, as anyone can.
if command -v python3 >/dev/null 2>&1 &&
    $PCOMPRESS -a -c lz4 -s 64k "$T/tree" "$T/meta" >/dev/null 2>&1; then
	python3 - "$T/meta.pz" "$T/mlen.pz" "$T/morig.pz" "$T/mback.pz" "$T/dback.pz" <<'PYEOF'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
pos = 28				# header and its CRC32
while True:
    n = struct.unpack(">Q", d[pos:pos + 8])[0]
    if n == 1:
        break
    pos += 8 + n + 32 + 4 + 1		# length, BLAKE256, CRC32, flags, data
a = bytearray(d)
a[pos + 8:pos + 16] = struct.pack("<Q", 8 << 20)
open(sys.argv[2], "wb").write(a + bytes(16 << 20))
b = bytearray(d)
b[pos + 16:pos + 24] = struct.pack("<Q", 64 << 20)
b[pos + 89:pos + 93] = bytes(4)
b[pos + 89:pos + 93] = struct.pack("<I", zlib.crc32(bytes(b[pos:pos + 93])))
open(sys.argv[3], "wb").write(b)
# Lengths that wrap a seek back onto themselves: a metadata chunk length
# the main reader skips (16 bytes back), and a data chunk length the
# metadata thread skips (8 bytes back). Both used to loop for good.
open(sys.argv[4], "wb").write(d[:28] + struct.pack(">Q", 1) +
    struct.pack("<Q", (1 << 64) - 93) + d[28:])
c = bytearray(d)
c[28:36] = struct.pack(">Q", (1 << 64) - 45)
open(sys.argv[5], "wb").write(c)
PYEOF
	mkdir -p "$T/xm1" "$T/xm2" "$T/xm3" "$T/xm4"
	expect 1 "metadata chunk length past its buffer" -d "$T/mlen.pz" "$T/xm1"
	expect 1 "metadata original length past its buffer" -d "$T/morig.pz" "$T/xm2"
	TMO=20
	expect 1 "metadata chunk length seeking back" -d "$T/mback.pz" "$T/xm3"
	expect 1 "data chunk length seeking back (metadata thread)" -d "$T/dback.pz" "$T/xm4"
	TMO=120
else
	SKIP=$((SKIP + 1)); echo "  SKIP: metadata chunk lengths (no python3)"
fi

# A hand-made archive with an absolute path and a "../" entry must extract
# only inside the target directory, also for a relative target with "..".
CRAFT="$(cd "$(dirname "$0")/.." && pwd)/tools/craft_hostile_archive.py"
# A native Windows python cannot run the ./pcompress shell wrapper: give
# it the program itself.
PCBIN=$PCOMPRESS
case "$(uname -s)" in
MINGW*) PCBIN="$(dirname "$PCOMPRESS")/buildtmp/pcompress.exe" ;;
esac
if command -v python3 >/dev/null 2>&1; then
	mkdir -p "$T/esc" "$T/xa/sub" "$T/xb"
	if python3 "$CRAFT" "$PCBIN" "$T/evil.pz" "$T/esc" 2>/dev/null; then
		(cd "$T/xa/sub" && $PCOMPRESS -d "$T/evil.pz" ../../xb >/dev/null 2>&1)
		if [ -z "$(ls "$T/esc")" ] && [ ! -e "$T/dotdot_escape" ] &&
		    [ ! -e "$T/xa/dotdot_escape" ] && [ -f "$T/xb/ok/inside" ]; then
			log_pass "hostile archive paths stay inside the target"
		else
			log_fail "hostile archive: escaped ($(ls "$T/esc" "$T" | tr '\n' ' '))"
		fi
	else
		log_fail "could not build the hostile archive"
	fi
else
	SKIP=$((SKIP + 1)); echo "  SKIP: hostile archive (no python3)"
fi

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="
[ $FAIL -gt 0 ] && exit 1
[ $PASS -eq 0 ] && { echo "ERROR: no test cases ran"; exit 1; }
exit 0
