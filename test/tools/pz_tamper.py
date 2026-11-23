#!/usr/bin/env python3
# Tamper with an encrypted (format version 12) .pz file, for the
# integration tests: every change must make decompression fail.
#   pz_tamper.py <in.pz> <out.pz> <op>
# ops: reorder (swap data chunks 0 and 1), dup (repeat data chunk 0 in
# place of chunk 1), drop-last (remove the last data chunk, keep the end
# marker and record), cut-end (remove the end record), header (flip a bit
# of the chunk size), payload (flip a bit inside data chunk 0), meta
# (flip a bit inside metadata chunk 0). Stream separation (a metadata
# chunk opened as a data chunk) is tested in test_crypto.
import struct, sys

src, dst, op = sys.argv[1], sys.argv[2], sys.argv[3]
d = bytearray(open(src, "rb").read())
if struct.unpack(">H", d[8:10])[0] != 12:
    sys.exit("not a version 12 file")
pos = 24 + 48 + 16                       # header, salt + KDF, tag
data, meta = [], []                      # (offset, length)
while True:
    (n,) = struct.unpack(">Q", d[pos:pos + 8])
    if n == 0:
        break
    if n == 1 and struct.unpack(">H", d[10:12])[0] & 4096:   # metadata chunk
        (cl,) = struct.unpack("<Q", d[pos + 8:pos + 16])
        meta.append((pos, 97 + cl))
        pos += 97 + cl
    else:
        data.append((pos, 8 + n + 16 + 1))
        pos += 8 + n + 16 + 1
end = pos                                # the 8 zero bytes, then the record


def cut(a):
    return bytes(d[a[0]:a[0] + a[1]])


if op == "reorder":
    a, b = data[0], data[1]
    out = d[:a[0]] + cut(b) + d[a[0] + a[1]:b[0]] + cut(a) + d[b[0] + b[1]:]
elif op == "dup":
    a, b = data[0], data[1]
    out = d[:b[0]] + cut(a) + d[b[0] + b[1]:]
elif op == "drop-last":
    a = data[-1]
    out = d[:a[0]] + d[a[0] + a[1]:]
elif op == "cut-end":
    out = d[:end + 8]
elif op == "header":
    d[20] ^= 0x01
    out = d
elif op == "meta":
    d[meta[0][0] + 97 + 1] ^= 0x04
    out = d
elif op == "payload":
    d[data[0][0] + 8 + 16 + 1 + 3] ^= 0x04
    out = d
else:
    sys.exit("unknown op " + op)
open(dst, "wb").write(bytes(out))
