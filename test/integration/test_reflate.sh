#!/bin/sh
#
# Integration test: the reflate filter (-Z). Archives of zip, jar, gzip
# and PNG members made by zlib extract bit for bit and are smaller than
# without -Z; members made by other deflaters, damaged containers and
# plain files are stored as they are; the file is format version 13
# with flag 0x40, and a reader that does not know the flag refuses it.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

T="${TMPDIR:-/tmp}/pc_reflate_$$"
mkdir -p "$T" || exit 1
trap 'find "$T" -depth -delete 2>/dev/null' EXIT
# $T as an archive member: pcompress drops a drive letter and leading "/".
TM=${T#?:}
TM=${TM#/}
# A native Windows python cannot run the ./pcompress shell wrapper: give
# it the program itself.
PCBIN=$PCOMPRESS
case "$(uname -s)" in
MINGW*) PCBIN="$(dirname "$PCOMPRESS")/buildtmp/pcompress.exe" ;;
esac

echo "========================================="
echo " Integration: Reflate (-Z)"
echo "========================================="

if ! command -v python3 >/dev/null 2>&1; then
	echo "  SKIP: needs python3 to build the containers"
	exit 0
fi

# Containers whose deflate streams come from the pinned deflate
# (buildtmp/rf_pdeflate), not the system zlib: on Fedora and Debian
# riscv64 that is zlib-ng, whose streams -Z cannot reproduce. A zip with
# a docx-like name, a jar, two gzip files at levels 6 and 9, a PNG, a
# gzip made with a strategy -Z does not search (Python's zlib, Huffman
# only: stored as is), a truncated zip, and plain text.
PDEFLATE="$(dirname "$PCOMPRESS")/buildtmp/rf_pdeflate"
case "$(uname -s)" in MINGW*) PDEFLATE="$PDEFLATE.exe" ;; esac
if [ ! -x "$PDEFLATE" ]; then
	echo "  SKIP: $PDEFLATE not built (make buildtmp/rf_pdeflate)"
	exit 0
fi
mkdir -p "$T/in/sub"
python3 - "$T/in" "$PDEFLATE" <<'PYEOF'
import os, struct, subprocess, sys, zlib, random
d, pdeflate = sys.argv[1], sys.argv[2]
random.seed(7)
words = [bytes(random.choice(b"etaoinshrdlu") for _ in range(random.randint(2, 9)))
         for _ in range(400)]
def text(n):
    return b" ".join(random.choice(words) for _ in range(n))
def deflate(data, level):
    # Raw deflate, window 15, memLevel 8, default strategy: zlib's defaults.
    return subprocess.run([pdeflate, str(level), "8", "15", "0"], input=data,
                          stdout=subprocess.PIPE, check=True).stdout
def zipfile_bytes(members):
    # members: (name, data, deflated?). Local headers, then the central
    # directory, as zipfile writes them (no data descriptors).
    out, cd = b"", b""
    for name, data, defl in members:
        body = deflate(data, 6) if defl else data
        crc, method = zlib.crc32(data), 8 if defl else 0
        fields = struct.pack("<HHHHHIIIHH", 20, 0, method, 0, 0x21, crc,
                             len(body), len(data), len(name), 0)
        cd += (b"PK\1\2" + struct.pack("<H", 20) + fields + struct.pack("<HHHII", 0, 0, 0, 0,
               len(out)) + name)
        out += b"PK\3\4" + fields + name + body
    end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, len(members), len(members),
                      len(cd), len(out), 0)
    return out + cd + end
for name in ("report.docx", "lib.jar"):
    m = [(b"part%d.xml" % i, text(4000 + 3000 * i), True) for i in range(6)]
    m.append((b"stored.bin", text(500), False))
    open(os.path.join(d, name), "wb").write(zipfile_bytes(m))
for lvl in (6, 9):
    body = text(60000)
    xfl = 2 if lvl == 9 else 0
    open(os.path.join(d, "sub", "t%d.gz" % lvl), "wb").write(
        b"\x1f\x8b\x08\0\0\0\0\0" + bytes([xfl, 255]) + deflate(body, lvl)
        + struct.pack("<II", zlib.crc32(body), len(body)))
raw = b"".join(b"\0" + bytes((x * y) & 255 for x in range(300)) for y in range(200))
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
idat = b"\x78\x9c" + deflate(raw, 6) + struct.pack(">I", zlib.adler32(raw))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 300, 200, 8, 0, 0, 0, 0))
       + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
open(os.path.join(d, "img.png"), "wb").write(png)
c = zlib.compressobj(6, zlib.DEFLATED, -15, 8, zlib.Z_HUFFMAN_ONLY)
body = text(20000)
open(os.path.join(d, "huff.gz"), "wb").write(
    b"\x1f\x8b\x08\0\0\0\0\0\0\x03" + c.compress(body) + c.flush()
    + struct.pack("<II", zlib.crc32(body), len(body)))
z = open(os.path.join(d, "report.docx"), "rb").read()
open(os.path.join(d, "cut.zip"), "wb").write(z[:len(z) // 2])
open(os.path.join(d, "plain.txt"), "wb").write(text(30000))
PYEOF

for opts in "-l6" "-l9" "-l6 -c lzma -e AES" "-l6 -X" "-l6 -T"; do
	rm -f "$T/a.pz" "$T/b.pz"
	find "$T/x" -depth -delete 2>/dev/null
	extra=""
	case "$opts" in *-e*) echo "reflate test password" > "$T/pw"; extra="-w $T/pw" ;; esac
	# shellcheck disable=SC2086
	if ! $PCOMPRESS -a $opts $extra -Z "$T/in" "$T/a" >/dev/null 2>&1; then
		log_fail "$opts: archive with -Z"
		continue
	fi
	case "$opts" in *-e*) echo "reflate test password" > "$T/pw" ;; esac
	# shellcheck disable=SC2086
	if $PCOMPRESS -d $extra "$T/a.pz" "$T/x" >/dev/null 2>&1 &&
	    diff -r "$T/in" "$T/x/$TM/in" >/dev/null 2>&1; then
		log_pass "$opts: extracts bit for bit"
	else
		log_fail "$opts: extraction differs"
	fi
	case "$opts" in *-e*) echo "reflate test password" > "$T/pw" ;; esac
	# shellcheck disable=SC2086
	$PCOMPRESS -a $opts $extra "$T/in" "$T/b" >/dev/null 2>&1
	a=$(wc -c < "$T/a.pz" | tr -d ' ')
	b=$(wc -c < "$T/b.pz" | tr -d ' ')
	if [ "$a" -lt "$b" ]; then
		log_pass "$opts: smaller with -Z ($a < $b)"
	else
		log_fail "$opts: not smaller with -Z ($a >= $b)"
	fi
done

echo ""
echo "--- Format ---"
rm -f "$T/a.pz" "$T/p.pz"
$PCOMPRESS -a -l6 -Z "$T/in" "$T/a" >/dev/null 2>&1
$PCOMPRESS -a -l6 "$T/in" "$T/p" >/dev/null 2>&1
# One byte as a decimal number: od zero-pads on illumos ("013"), which
# the shell would read as octal.
byte_at() { echo $(( $(od -An -tu1 -j "$2" -N 1 "$1" | tr -d ' ' | sed 's/^0*\(.\)/\1/') )); }
v=$(byte_at "$T/a.pz" 9)
f=$(byte_at "$T/a.pz" 11)
vp=$(byte_at "$T/p.pz" 9)
if [ "$v" = 13 ] && [ $((f & 64)) = 64 ] && [ "$vp" = 10 ]; then
	log_pass "-Z writes version 13 with flag 0x40; without it, version 10"
else
	log_fail "versions: -Z $v flags $f, plain $vp"
fi
if $PCOMPRESS -i "$T/a.pz" 2>/dev/null | grep "report.docx" |
    grep -q " $(wc -c < "$T/in/report.docx" | tr -d ' ') "; then
	log_pass "listing shows the original size"
else
	log_fail "listing shows the stored size"
fi
if ! $PCOMPRESS -c zstd -Z "$T/in/plain.txt" "$T/y" >/dev/null 2>&1; then
	log_pass "-Z refused without -a"
else
	log_fail "-Z accepted without -a"
fi

# A record that does not rebuild: the member is written as stored, listed
# in filter_failures.txt, and extraction exits non-zero. The archive is
# made by hand (as test/tools/craft_hostile_archive.py does): a PAX
# member tagged for the reflate filter whose record is not one,
# compressed with -c none as a file, then flagged as an archive.
rm -f "$T/n.pz"
find "$T/x" -depth -delete 2>/dev/null
mkdir -p "$T/x"
if python3 - "$PCBIN" "$T/n" <<'PYEOF'
import base64, io, os, struct, subprocess, sys, tarfile, zlib
pc, out = sys.argv[1], sys.argv[2]
data = struct.pack("<Q", 40) + b"PCRF\x01\x00" + b"\x07" * 90
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as t:
    ti = tarfile.TarInfo("bad.gz")
    ti.size = len(data)
    ti.mode = 0o644
    ti.pax_headers = {"LIBARCHIVE.xattr._._pc_filter_xattr":
                      base64.b64encode(b"Reflate").decode()}
    t.addfile(ti, io.BytesIO(data))
open(out, "wb").write(buf.getvalue())
subprocess.run([pc, "-c", "none", "-l1", out], check=True, stderr=subprocess.DEVNULL)
d = bytearray(open(out + ".pz", "rb").read())
d[10:12] = struct.pack(">H", struct.unpack(">H", d[10:12])[0] | 2048)
d[24:28] = struct.pack(">I", zlib.crc32(bytes(d[0:24])))
open(out + ".pz", "wb").write(d)
PYEOF
then
	if ! $PCOMPRESS -d "$T/n.pz" "$T/x" > "$T/err" 2>&1 &&
	    grep -q "1 pathnames failed filter decoding" "$T/err" &&
	    [ -f "$T/x/filter_failures.txt" ]; then
		log_pass "a record that does not rebuild fails the extraction"
	else
		log_fail "a record that does not rebuild: $(tail -n 2 "$T/err")"
	fi
else
	log_fail "making the damaged record"
fi

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="
[ $FAIL -gt 0 ] && exit 1
[ $PASS -eq 0 ] && { echo "ERROR: no test cases ran"; exit 1; }
exit 0
