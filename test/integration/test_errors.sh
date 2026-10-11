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
expect() {
	want=$1; label=$2; shift 2
	timeout 120 $PCOMPRESS "$@" >"$T/out.log" 2>&1 </dev/null
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
		timeout 120 $PCOMPRESS -d -w "$T/pw" "$T/c.pz" "$T/c.out" >/dev/null 2>&1
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
		timeout 120 $PCOMPRESS -d -w "$T/pw" "$T/c.pz" "$T/c.out" >/dev/null 2>&1
		st=$?
		if [ $st -ge 124 ] || [ $st -eq 0 ]; then
			log_fail "$a $o: truncated to $len: status $st"; ok=0
		fi
	done
	[ $ok -eq 1 ] && log_pass "$a $o: corruption/truncation rejected"
done

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

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="
[ $FAIL -gt 0 ] && exit 1
[ $PASS -eq 0 ] && { echo "ERROR: no test cases ran"; exit 1; }
exit 0
