#!/usr/bin/env python3
"""Benchmark tracking: measure pcompress on a fixed corpus, compare runs.

  ci/bench.py run PCOMPRESS CORPUS BENCH_CHUNK OUT.json
  ci/bench.py compare OLD.json NEW.json

"run" compresses and decompresses CORPUS (enwik8 in the nightly job)
with each algorithm in CASES, three times, and keeps the best time of
each; it also runs bench_chunk on 64 MB and keeps its MB/s lines. The
output is one JSON object: per case the compressed size, the ratio and
compress and decompress MB/s, and per bench_chunk line its MB/s.

"compare" prints a table of NEW against OLD and exits 1 when a ratio
got worse by more than RATIO_TOLERANCE (sizes are deterministic for a
given build, so any growth is a real change), or when a case is gone.
A speed drop beyond SPEED_WARN is only reported (a GitHub Actions
warning annotation): shared runners vary by 10 to 20% from run to run.
"""

import json
import os
import subprocess
import sys
import tempfile
import time

CASES = ["zstd:3", "zstd:9", "lz4:1", "lzma:6", "libbsc:6", "adapt2:6"]
CHUNK = "8m"
REPEAT = 3
RATIO_TOLERANCE = 0.001
SPEED_WARN = 0.25


def timed(cmd):
    """Run cmd (output discarded), fail on a non-zero status; seconds taken."""
    t0 = time.monotonic()
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.monotonic() - t0


def run_case(pc, corpus, case, tmp):
    """Best-of-REPEAT compress and decompress of corpus with one algo:level."""
    algo, level = case.split(":")
    packed, out = os.path.join(tmp, "x.pz"), os.path.join(tmp, "x.out")
    size = os.path.getsize(corpus)
    best_c = best_d = None
    for _ in range(REPEAT):
        for p in (packed, out):
            if os.path.exists(p):
                os.unlink(p)
        tc = timed([pc, "-c", algo, "-l", level, "-s", CHUNK, corpus, packed])
        td = timed([pc, "-d", packed, out])
        best_c = tc if best_c is None else min(best_c, tc)
        best_d = td if best_d is None else min(best_d, td)
    with open(corpus, "rb") as a, open(out, "rb") as b:
        if a.read() != b.read():
            sys.exit(f"bench: {case}: decompressed output differs from the input")
    csize = os.path.getsize(packed)
    return {
        "size": csize,
        "ratio": size / csize,
        "compress_mbs": size / 1e6 / best_c,
        "decompress_mbs": size / 1e6 / best_d,
    }


def run_bench_chunk(path):
    """bench_chunk on 64 MB: {label: MB/s} for each of its MB/s lines."""
    text = subprocess.run([path, "64"], check=True, capture_output=True, text=True).stdout
    result = {}
    for line in text.splitlines():
        if line.rstrip().endswith("MB/s"):
            label, value, _unit = line.rsplit(None, 2)
            result[label.strip()] = float(value)
    return result


def cmd_run(pc, corpus, bench_chunk, out_json):
    """Measure every case and bench_chunk; write the JSON result."""
    with tempfile.TemporaryDirectory() as tmp:
        cases = {c: run_case(pc, corpus, c, tmp) for c in CASES}
    result = {
        "corpus": os.path.basename(corpus),
        "corpus_bytes": os.path.getsize(corpus),
        "chunk": CHUNK,
        "cases": cases,
        "bench_chunk": run_bench_chunk(bench_chunk),
    }
    with open(out_json, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    for c, r in cases.items():
        print(
            f"{c:10} ratio {r['ratio']:7.4f}  c {r['compress_mbs']:8.1f} MB/s"
            f"  d {r['decompress_mbs']:8.1f} MB/s"
        )


def cmd_compare(old_json, new_json):
    """Table of new against old; status 1 on a ratio regression or lost case."""
    with open(old_json) as f:
        old = json.load(f)
    with open(new_json) as f:
        new = json.load(f)
    failed = False
    print(
        f"{'case':10} {'ratio old':>10} {'new':>8} {'c MB/s old':>11} {'new':>8}"
        f" {'d MB/s old':>11} {'new':>8}"
    )
    for case, o in sorted(old["cases"].items()):
        n = new["cases"].get(case)
        if n is None:
            print(f"::error::bench: case {case} is missing from the new run")
            failed = True
            continue
        print(
            f"{case:10} {o['ratio']:10.4f} {n['ratio']:8.4f} {o['compress_mbs']:11.1f}"
            f" {n['compress_mbs']:8.1f} {o['decompress_mbs']:11.1f} {n['decompress_mbs']:8.1f}"
        )
        if n["ratio"] < o["ratio"] * (1 - RATIO_TOLERANCE):
            print(f"::error::bench: {case} ratio {o['ratio']:.4f} -> {n['ratio']:.4f}")
            failed = True
        for k in ("compress_mbs", "decompress_mbs"):
            if n[k] < o[k] * (1 - SPEED_WARN):
                print(f"::warning::bench: {case} {k} {o[k]:.1f} -> {n[k]:.1f}")
    for label, o in sorted(old.get("bench_chunk", {}).items()):
        n = new.get("bench_chunk", {}).get(label)
        if n is not None and n < o * (1 - SPEED_WARN):
            print(f"::warning::bench_chunk: {label} {o:.1f} -> {n:.1f} MB/s")
    return 1 if failed else 0


def main():
    """Dispatch to run or compare."""
    if len(sys.argv) == 6 and sys.argv[1] == "run":
        cmd_run(*sys.argv[2:])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "compare":
        return cmd_compare(sys.argv[2], sys.argv[3])
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
