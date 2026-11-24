#!/usr/bin/env python3
# Deterministic inputs and an independent model for ci/difffuzz.sh.
#
#   xver_model.py input <seed> <file>       random input, 0 to 3 MB
#   xver_model.py cases <seed> <n>          n option sets 4.4.0 knows
#   xver_model.py spec <seed> <file>        a random manifest spec
#   xver_model.py check <seed> <dir>        check xver_drv's outputs in dir
#
# The model is written from DESIGN.md, not from the C code: the gear
# chunker (FastCDC with normalized chunking, splitmix64 table), chunk ids
# (keyed BLAKE2b-256, hashlib), the PCMF2 manifest encoding, and the
# PCIDX1 index file. Every run with one seed does the same thing.
import hashlib
import os
import random
import struct
import sys

M = (1 << 64) - 1
KEY = bytes(range(32))          # the driver's fixed chunk-id key
WORDS = ("backup chunk store object seal open manifest index key the of and "
         "zstd lzma pcompress dedupe archive\n").split(" ")


def gear_table():
    s, t = 0x70636F6D70726573, []   # "pcompres"
    for _ in range(256):
        s = (s + 0x9E3779B97F4A7C15) & M
        z = s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
        t.append(z ^ (z >> 31))
    return t


GEAR = gear_table()


def gear_chunks(buf, avg, mn, mx):
    """Boundaries hashing every byte; the C code skips what cannot matter."""
    bits = avg.bit_length() - 1
    ms = (M << (64 - (bits + 2))) & M
    ml = (M << (64 - (bits - 2))) & M
    h = cur = 0
    lens = []
    for b in buf:
        cur += 1
        h = ((h << 1) + GEAR[b]) & M
        if cur >= mn and ((h & (ms if cur < avg else ml)) == 0 or cur >= mx):
            lens.append(cur)
            h = cur = 0
    if cur:
        lens.append(cur)
    return lens


def gen_input(seed):
    """Text, random bytes, zeros and repeats, in random amounts."""
    r = random.Random(seed)
    size = r.choice([0, 1, 2, 3, 100, 4095, 65536, r.randrange(1, 3 << 20)])
    out = bytearray()
    while len(out) < size:
        k = r.randrange(4)
        n = r.randrange(1, 200000)
        if k == 0:
            while n > 0:
                w = r.choice(WORDS).encode() + b" "
                out += w
                n -= len(w)
        elif k == 1:
            out += r.randbytes(n)
        elif k == 2:
            out += bytes(n)
        elif out:
            a = r.randrange(len(out))
            out += out[a:a + n]
    return bytes(out[:size])


def gen_cases(seed, n):
    """Single-file option sets, each understood by 4.4.0."""
    r = random.Random(seed)
    algos = ["zstd", "lz4", "lzma", "zlib", "bzip2", "ppmd", "lzfx", "adapt",
             "adapt2", "none", "libbsc"]
    sums = ["CRC64", "SHA256", "SHA512", "KECCAK256", "KECCAK512", "BLAKE256",
            "BLAKE512"]
    cases = []
    for _ in range(n):
        o = ["-c", r.choice(algos), "-l", str(r.randint(1, 14)),
             "-s", r.choice(["64k", "256k", "1m", "4m"])]
        d = r.randrange(6)
        if d == 1:
            o.append("-D")
        elif d == 2:
            o += ["-E"] * r.randint(1, 2)
        elif d == 3:
            o.append("-F")
        elif d == 4:
            # Global dedupe needs chunks of 2 MB or more.
            o[5] = r.choice(["2m", "4m"])
            o += ["-G", r.choice(["-D", "-F"])]
        if d in (1, 2, 4) and r.random() < 0.5:
            o += ["-B", str(r.randint(0, 5))]
        if r.random() < 0.3:
            o.append("-L")
        if r.random() < 0.3:
            o.append("-P")
        if r.random() < 0.4:
            # pcompress refuses CRC64 with deduplication.
            o += ["-S", r.choice(sums[1:] if d else sums)]
        if r.random() < 0.25:
            o += ["-e", r.choice(["AES", "SALSA20"])]
        o += ["-t", str(r.choice([1, 2, 4]))]
        if r.random() < 0.15:
            o.append("-p")
        cases.append(o)
    return cases


# --- Manifest spec and the PCMF2 model ----------------------------------

def gen_spec(seed):
    """Entries as tuples; paths random but valid, no entry under a link."""
    r = random.Random(seed)
    names = ["a", "b", "dir", "z", "\u00e9t\u00e9", "x y", "e\u0301", "0", "~"]
    paths = set()
    ents = []
    for _ in range(r.randint(0, 40)):
        depth = r.randint(1, 3)
        p = "/".join(r.choice(names) + str(r.randrange(3)) for _ in range(depth))
        if p in paths:
            continue
        paths.add(p)
        t = r.choice([1, 1, 1, 2, 3, 4, 5, 6, 7])
        e = {"path": p, "type": t, "mode": r.randrange(1 << 16), "uid": r.randrange(1 << 32),
             "gid": r.randrange(1 << 32), "mtime": r.randrange(-(1 << 40), 1 << 40),
             "nsec": r.randrange(10 ** 9), "target": "", "chunks": [], "owner": None,
             "btime": None, "rdev": 0, "xattrs": []}
        if t == 1:
            e["chunks"] = [(r.randbytes(32), r.randint(1, 1 << 20))
                           for _ in range(r.randint(0, 5))]
        elif t == 3:
            e["target"] = "../" + r.choice(names)
        elif t == 4:
            e["target"] = r.choice(names) + "0"
        elif t in (5, 6):
            e["rdev"] = r.randrange(1 << 64)
        if r.random() < 0.3:
            e["owner"] = r.choice(["root", "gburd", "\u5c71\u7530"])
        if r.random() < 0.3:
            e["btime"] = (r.randrange(1 << 40), r.randrange(10 ** 9))
        if r.random() < 0.3:
            e["xattrs"] = sorted({"user." + r.choice(names): r.randbytes(r.randrange(8))
                                  for _ in range(r.randint(1, 3))}.items())
        ents.append(e)
    # Nothing may lie under a symlink entry: drop such entries.
    links = [e["path"] + "/" for e in ents if e["type"] == 3]
    return [e for e in ents if not any(e["path"].startswith(l) for l in links)]


def write_spec(ents, f):
    """One entry per line for the driver; strings hex-encoded UTF-8."""
    hx = lambda s: s.encode().hex() if s else "-"
    for e in ents:
        bt = "%d:%d" % e["btime"] if e["btime"] else "-"
        xs = ",".join("%s=%s" % (k.encode().hex(), v.hex() or "-") for k, v in e["xattrs"])
        ch = ",".join("%s:%d" % (i.hex(), n) for i, n in e["chunks"])
        f.write(" ".join([hx(e["path"]), str(e["type"]), str(e["mode"]), str(e["uid"]),
                          str(e["gid"]), str(e["mtime"]), str(e["nsec"]), hx(e["target"]),
                          str(e["rdev"]), hx(e["owner"]), bt, xs or "-", ch or "-"]) + "\n")


def encode_manifest(ents):
    """PCMF2 from DESIGN.md: sorted by path bytes, records in tag order."""
    out = bytearray(b"PCMF2\0\0\0") + struct.pack("<Q", len(ents))
    for e in sorted(ents, key=lambda e: e["path"].encode()):
        p, t = e["path"].encode(), e["target"].encode()
        size = sum(n for _, n in e["chunks"])
        out += struct.pack("<H", len(p)) + p
        out += struct.pack("<BIIIqIQH", e["type"], e["mode"], e["uid"], e["gid"],
                           e["mtime"], e["nsec"], size, len(t)) + t
        out += struct.pack("<I", len(e["chunks"]))
        for i, n in e["chunks"]:
            out += i + struct.pack("<I", n)
        if e["btime"]:
            out += struct.pack("<BIqI", 1, 12, *e["btime"])
        if e["type"] in (5, 6):
            out += struct.pack("<BIQ", 4, 8, e["rdev"])
        if e["owner"]:
            o = e["owner"].encode()
            out += struct.pack("<BI", 6, len(o)) + o
        for k, v in e["xattrs"]:
            k = k.encode()
            out += struct.pack("<BIB", 8, 1 + len(k) + len(v), len(k)) + k + v
        out += b"\0"
    return bytes(out)


# --- Checking the driver's outputs ---------------------------------------

def fail(msg):
    print("MODEL FAIL " + msg)
    sys.exit(1)


def check(seed, d):
    """Compare xver_drv's chunk, id, manifest and index output to the model."""
    data = open(os.path.join(d, "in"), "rb").read()
    want, ids = gear_chunks(data, 4096, 1024, 16384), []
    got = [line.split() for line in open(os.path.join(d, "chunks.txt"))]
    if [int(g[0]) for g in got] != want:
        fail("gear boundaries (seed %d)" % seed)
    pos = 0
    for n, (ln, plain, keyed) in zip(want, got):
        c = data[pos:pos + n]
        pos += n
        if hashlib.blake2b(c, digest_size=32).hexdigest() != plain:
            fail("unkeyed id (seed %d)" % seed)
        if hashlib.blake2b(c, digest_size=32, key=KEY).hexdigest() != keyed:
            fail("keyed id (seed %d)" % seed)
        ids.append(bytes.fromhex(plain))
    m = open(os.path.join(d, "manifest.bin"), "rb").read()
    if m != encode_manifest(gen_spec(seed)):
        fail("manifest bytes (seed %d)" % seed)
    # The index file: magic, then each distinct id once, in order added.
    seen, body = set(), bytearray(b"PCIDX1\0\0")
    for i in ids + ids[: len(ids) // 2]:
        if i not in seen:
            seen.add(i)
            body += i
    if open(os.path.join(d, "index.bin"), "rb").read() != bytes(body):
        fail("index file (seed %d)" % seed)
    print("model ok seed=%d chunks=%d entries=%d" % (seed, len(want), len(gen_spec(seed))))


def main():
    op, seed = sys.argv[1], int(sys.argv[2])
    if op == "input":
        open(sys.argv[3], "wb").write(gen_input(seed))
    elif op == "cases":
        for c in gen_cases(seed, int(sys.argv[3])):
            print(" ".join(c))
    elif op == "spec":
        with open(sys.argv[3], "w") as f:
            write_spec(gen_spec(seed), f)
    elif op == "check":
        check(seed, sys.argv[3])
    else:
        sys.exit("unknown op " + op)


if __name__ == "__main__":
    main()
