#!/usr/bin/env python3
# Seeds for fuzz_reflate (mode byte 1: a member rf_encode() turns into a
# record): a gzip file, a zip and a PNG, all made by zlib at settings the
# reflate filter searches, so the fuzzer starts inside the decoders.
#   reflate_seeds.py <dir>
import os, struct, sys, zlib, gzip, io, zipfile

d = sys.argv[1]
text = b"".join(b"line %d of the seed, some words repeat\n" % i for i in range(400))
open(os.path.join(d, "gzip"), "wb").write(b"\1" + gzip.compress(text, 6, mtime=0))
b = io.BytesIO()
with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("a.xml", text)
    z.writestr("b.xml", text[::-1])
open(os.path.join(d, "zip"), "wb").write(b"\1" + b.getvalue())
def chunk(t, p):
    return struct.pack(">I", len(p)) + t + p + struct.pack(">I", zlib.crc32(t + p))
raw = b"".join(b"\0" + bytes((x ^ y) & 255 for x in range(64)) for y in range(64))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 0, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
open(os.path.join(d, "png"), "wb").write(b"\1" + png)
