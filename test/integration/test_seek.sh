#!/bin/sh
#
# Integration test: the seek index (-X) and random reads (-r, -R).
# A range or a member read with the index must equal the same bytes of a
# full decode; files without an index, stream input and -G are refused;
# a damaged index or a chunk that does not match it fails the read.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

T="${TMPDIR:-/tmp}/pc_seek_$$"
mkdir -p "$T" || exit 1
trap 'find "$T" -depth -delete 2>/dev/null' EXIT

echo "========================================="
echo " Integration: Seek index"
echo "========================================="

# About 3 MB of varied text: several 256k chunks.
i=0
while [ $i -lt 40000 ]; do
	echo "row $i: $((i * 7919 % 10007)) the quick brown fox $((i % 97))"
	i=$((i + 1))
done > "$T/in"
size=$(wc -c < "$T/in" | tr -d ' ')

# expect_range <label> <file.pz> <offset> <length> [pcompress options]:
# the bytes -r returns must be those of the input.
expect_range() {
	label=$1; pz=$2; off=$3; len=$4; shift 4
	rm -f "$T/part"
	if $PCOMPRESS -d "$@" -r "$off:$len" "$pz" "$T/part" >/dev/null 2>&1 &&
	    dd if="$T/in" bs=1 skip="$off" count="$len" 2>/dev/null | cmp -s - "$T/part"; then
		log_pass "$label"
	else
		log_fail "$label"
	fi
}

echo ""
echo "--- File ranges ---"
for algo in zstd lz4; do
	rm -f "$T/x.pz"
	if ! $PCOMPRESS -c $algo -l6 -X -s 256k "$T/in" "$T/x" >/dev/null 2>&1; then
		log_fail "$algo: compress with -X"
		continue
	fi
	expect_range "$algo: first bytes" "$T/x.pz" 0 10
	expect_range "$algo: across chunks" "$T/x.pz" 250000 300000
	expect_range "$algo: last byte" "$T/x.pz" $((size - 1)) 1
	expect_range "$algo: one chunk exactly" "$T/x.pz" 262144 262144
	rm -f "$T/full"
	if $PCOMPRESS -d "$T/x.pz" "$T/full" >/dev/null 2>&1 && cmp -s "$T/in" "$T/full"; then
		log_pass "$algo: full decode of an indexed file"
	else
		log_fail "$algo: full decode of an indexed file"
	fi
done

# A range past the end is refused, one that runs past it is cut.
rm -f "$T/part"
if ! $PCOMPRESS -d -r "$size:1" "$T/x.pz" "$T/part" >/dev/null 2>&1; then
	log_pass "range starting past the end refused"
else
	log_fail "range starting past the end accepted"
fi
rm -f "$T/part"
$PCOMPRESS -d -r "$((size - 5)):100" "$T/x.pz" "$T/part" >/dev/null 2>&1
if [ "$(wc -c < "$T/part" | tr -d ' ')" = 5 ]; then
	log_pass "range running past the end cut"
else
	log_fail "range running past the end not cut"
fi

echo ""
echo "--- Encrypted ---"
echo "seek-test-password" > "$T/pw"
cp "$T/pw" "$T/pw1"
rm -f "$T/e.pz"
if $PCOMPRESS -c zstd -X -e AES -w "$T/pw1" -s 256k "$T/in" "$T/e" >/dev/null 2>&1; then
	cp "$T/pw" "$T/pw2"
	expect_range "AES: across chunks" "$T/e.pz" 600000 400000 -w "$T/pw2"
	echo "not-the-password" > "$T/pw3"
	rm -f "$T/part"
	if ! $PCOMPRESS -d -w "$T/pw3" -r 0:1 "$T/e.pz" "$T/part" >/dev/null 2>&1; then
		log_pass "AES: wrong password refused"
	else
		log_fail "AES: wrong password accepted"
	fi
else
	log_fail "AES: compress with -X"
fi

echo ""
echo "--- Archive members ---"
mkdir -p "$T/tree/sub"
cp "$T/in" "$T/tree/big.txt"
echo "small file" > "$T/tree/sub/small.txt"
dd if=/dev/urandom of="$T/tree/sub/rand.bin" bs=1024 count=700 2>/dev/null
rm -f "$T/a.pz"
if (cd "$T" && $PCOMPRESS -a -X -c zstd -l6 -s 256k tree a >/dev/null 2>&1); then
	for m in tree/sub/small.txt tree/sub/rand.bin tree/big.txt; do
		find "$T/o" -depth -delete 2>/dev/null; mkdir -p "$T/o"
		if $PCOMPRESS -d -R "$m" "$T/a.pz" "$T/o" >/dev/null 2>&1 &&
		    cmp -s "$T/$m" "$T/o/$m" &&
		    [ "$(find "$T/o" -type f | wc -l | tr -d ' ')" = 1 ]; then
			log_pass "member $m alone"
		else
			log_fail "member $m"
		fi
	done
	find "$T/o" -depth -delete 2>/dev/null; mkdir -p "$T/o"
	if ! $PCOMPRESS -d -R tree/none "$T/a.pz" "$T/o" >/dev/null 2>&1; then
		log_pass "unknown member refused"
	else
		log_fail "unknown member accepted"
	fi
	find "$T/o" -depth -delete 2>/dev/null; mkdir -p "$T/o"
	if $PCOMPRESS -d "$T/a.pz" "$T/o" >/dev/null 2>&1 && diff -r "$T/tree" "$T/o/tree" >/dev/null; then
		log_pass "full extraction of an indexed archive"
	else
		log_fail "full extraction of an indexed archive"
	fi
else
	log_fail "archive with -X"
fi

# The first member of an archive is indexed by the archiver thread as
# soon as it starts: the index must exist by then (TSan found it set up
# after the thread started). Several trees, the first member, repeated.
mkdir -p "$T/many"
for d in 1 2 3 4 5 6; do
	mkdir -p "$T/many/t$d"; echo "first $d" > "$T/many/t$d/a"
done
ok=1
for n in 1 2 3 4 5 6 7 8 9 10; do
	rm -f "$T/m.pz"; find "$T/o" -depth -delete 2>/dev/null; mkdir -p "$T/o"
	if ! (cd "$T" && $PCOMPRESS -a -X -c lz4 -l1 many/t1 many/t2 many/t3 \
	    many/t4 many/t5 many/t6 m >/dev/null 2>&1) ||
	    ! $PCOMPRESS -d -R many/t1/a "$T/m.pz" "$T/o" >/dev/null 2>&1 ||
	    ! cmp -s "$T/many/t1/a" "$T/o/many/t1/a"; then
		ok=0; break
	fi
done
if [ $ok = 1 ]; then log_pass "first member, repeated"; else log_fail "first member (run $n)"; fi

echo ""
echo "--- Refusals ---"
# expect_fail <label> <args...>: pcompress must exit 1.
expect_fail() {
	label=$1; shift
	"$@" >/dev/null 2>&1
	if [ $? -eq 1 ]; then log_pass "$label"; else log_fail "$label"; fi
}
rm -f "$T/n.pz" "$T/part"
$PCOMPRESS -c zstd -l6 -s 256k "$T/in" "$T/n" >/dev/null 2>&1
expect_fail "no index: -r refused" $PCOMPRESS -d -r 0:1 "$T/n.pz" "$T/part"
expect_fail "-X with -G refused" $PCOMPRESS -c zstd -X -G "$T/in" "$T/g"
expect_fail "-r on an archive refused" $PCOMPRESS -d -r 0:1 "$T/a.pz" "$T/o"
rm -f "$T/part"
expect_fail "-R on a file refused" $PCOMPRESS -d -R x "$T/x.pz" "$T/part"
if cat "$T/x.pz" | $PCOMPRESS -d -r 0:1 - >/dev/null 2>&1; then
	log_fail "stream input with -r accepted"
else
	log_pass "stream input with -r refused"
fi

echo ""
echo "--- Damage ---"
if command -v python3 >/dev/null 2>&1; then
	python3 - "$T/x.pz" "$T" <<'PYEOF'
import struct, sys
src, d = sys.argv[1], sys.argv[2]
b = open(src, "rb").read()
t = 32					# BLAKE256 tag
n = struct.unpack("<Q", b[-16:-8])[0]
body = len(b) - 16 - t - n
def put(name, x): open(d + "/" + name, "wb").write(x)
x = bytearray(b); x[body + 45] ^= 1; put("d_body.pz", x)
x = bytearray(b); x[-17] ^= 1; put("d_tag.pz", x)
x = bytearray(b); x[-16:-8] = struct.pack("<Q", 1 << 40); put("d_len.pz", x)
x = bytearray(b); x[body - 40] ^= 0x55; put("d_chunk.pz", x)
# Chunk 0's length in the index one byte short, under a recomputed tag
# (BLAKE256: BLAKE2b-256, stored byte-reversed), as anyone can forge an
# unencrypted index: its chunk then decodes to more than the index says.
import hashlib
x = bytearray(b)
u = body + 40 + 8
x[u:u + 8] = struct.pack("<Q", struct.unpack("<Q", b[u:u + 8])[0] - 1)
x[-16 - t:-16] = hashlib.blake2b(bytes(x[body:body + n]), digest_size=32).digest()[::-1]
put("d_ulen.pz", x)
PYEOF
	# Two equal-length encrypted chunks swapped: each still has a valid
	# HMAC, so only the index's copy of their tags tells them apart.
	dd if=/dev/urandom of="$T/r.bin" bs=262144 count=6 2>/dev/null
	cp "$T/pw" "$T/pw4"
	rm -f "$T/r.pz"
	$PCOMPRESS -c none -X -e AES -w "$T/pw4" -s 256k "$T/r.bin" "$T/r" >/dev/null 2>&1
	python3 - "$T/r.pz" "$T/d_swap.pz" <<'PYEOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
p = 24 + 4 + struct.unpack(">I", b[24:28])[0] + 8 + 4 + 16 + 32
ch = []
while struct.unpack(">Q", b[p:p + 8])[0]:
    n = 8 + 32 + 1 + struct.unpack(">Q", b[p:p + 8])[0]
    ch.append((p, n)); p += n
(a, n), (c, _) = ch[1], ch[3]
x = bytearray(b); x[a:a + n] = b[c:c + n]; x[c:c + n] = b[a:a + n]
open(sys.argv[2], "wb").write(x)
PYEOF
	cp "$T/pw" "$T/pw5"
	rm -f "$T/part"
	expect_fail "swapped encrypted chunks refused" $PCOMPRESS -d -w "$T/pw5" \
	    -r 300000:10 "$T/d_swap.pz" "$T/part"
	# A body length of 900 MB (a hole in a sparse file) under a wrong
	# tag: refused on its tag, before 900 MB is allocated for it. Linux
	# only, where ulimit -d bounds malloc() but not a file mapping.
	if [ "$(uname -s)" = Linux ]; then
		python3 - "$T/x.pz" "$T/d_big.pz" <<'PYEOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
t = 32
n = struct.unpack("<Q", b[-16:-8])[0]
head = b[:len(b) - 16 - t - n]
with open(sys.argv[2], "wb") as f:
    f.write(head)
    f.seek(len(head) + (900 << 20))
    f.write(bytes(t) + struct.pack("<Q", 900 << 20) + b"PCSEEK01")
PYEOF
		rm -f "$T/part"
		if (ulimit -d 400000; $PCOMPRESS -d -r 0:1 "$T/d_big.pz" "$T/part" 2>&1) |
		    grep -q "verification failed"; then
			log_pass "huge index refused on its tag, not allocated"
		else
			log_fail "huge index allocated before its tag was checked"
		fi
		rm -f "$T/d_big.pz"
	fi
	for f in d_body d_tag d_len d_chunk d_ulen; do
		rm -f "$T/part"
		expect_fail "damaged $f refused" $PCOMPRESS -d -r 0: "$T/$f.pz" "$T/part"
	done
else
	SKIP=$((SKIP + 1)); echo "  SKIP: damaged index (no python3)"
fi

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="
[ $FAIL -gt 0 ] && exit 1
[ $PASS -eq 0 ] && { echo "ERROR: no test cases ran"; exit 1; }
exit 0
