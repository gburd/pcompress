#!/bin/sh
#
# Feature round-trip matrix: every algorithm, level, dedupe/delta/filter
# option, checksum, cipher, pipe mode, archive mode, and a set of negative
# cases (corrupt/truncated input, bad args). Each round trip is verified
# byte-for-byte with cmp(1).
#
# Usage: feature_matrix.sh [pcompress] [workdir] [quick]
#   quick  - only level 6 for the per-algorithm sweep.
# Env:     CORPUS=<dir> source of mixed test data (default: source tree)
#          TMO=<sec>    per-command timeout (default 600)
# Exit status is the number of failures (capped at 255).
#
SRC=$(cd "$(dirname "$0")/.." && pwd)
PC=${1:-$SRC/pcompress}; W=${2:-${TMPDIR:-/tmp}/pcompress-matrix.$$}; Q=$3
case "$PC" in /*) ;; *) PC="$(pwd)/$PC" ;; esac
CORPUS=${CORPUS:-$SRC}
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }
mkdir -p "$W" && cd "$W" || exit 99
fail=0; pass=0
if [ ! -f mixed.dat ]; then
	# Deterministic, portable corpus: text, binary, repetitive, random.
	# CORPUS env: dir of mixed text+binary files (default: the pcompress tree)
	( cd "$CORPUS" && find . -type f -size -2M 2>/dev/null | LC_ALL=C sort | head -3000 | xargs cat 2>/dev/null ) | head -c 24000000 > mixed.dat
	[ $(wc -c < mixed.dat) -gt 1000000 ] || { echo "corpus too small"; exit 99; }
	head -c 4000000 /dev/urandom > rnd.dat
	cat mixed.dat rnd.dat mixed.dat > dup.dat      # dedupe-friendly
	awk 'BEGIN{for(i=0;i<300000;i++) printf "%d,%d,%d\n", i, i*3, i*7}' > num.dat
	: > empty.dat; printf x > one.dat; head -c 5000 mixed.dat > small.dat
	mkdir -p tree/sub/deep; cp num.dat one.dat empty.dat tree/; cp mixed.dat tree/sub/;
	ln -sf ../num.dat tree/sub/link 2>/dev/null; mkdir -p tree/emptydir
	printf 'pw%s\n' secret > pw.master
fi
ck() { # name, compress-args...; input $IN; DARGS = extra decompress args
	n=$1; shift
	rm -f o.pz o.out
	if timeout ${TMO:-600} "$PC" "$@" "$IN" o >c.log 2>&1 && [ -f o.pz ] && \
	   { [ -z "$PWF" ] || cp pw.master pw; } && \
	   timeout ${TMO:-600} "$PC" -d $DARGS o.pz o.out >d.log 2>&1 && cmp -s "$IN" o.out; then
		pass=$((pass+1)); echo "PASS $n"
	else
		fail=$((fail+1)); echo "FAIL $n"; tail -n 3 c.log d.log | sed 's/^/    /'
	fi
}
algos="lzfx lz4 zlib bzip2 lzma lzmaMt zstd ppmd libbsc adapt adapt2 none"
levels="1 6 9 14"; [ -n "$Q" ] && levels="6"
for IN in mixed.dat num.dat one.dat small.dat; do
	for a in $algos; do for l in $levels; do
		ck "$a-l$l-$IN" -c $a -l$l -s 2m
	done; done
done
IN=dup.dat
for opt in "-D" "-D -E" "-D -EE" "-F" "-G" "-G -F" "-D -B0" "-D -B5" "-L" "-P" "-L -P" "-G -L -P" "-D -T"; do
	ck "lzma $opt" -c lzma -l6 -s 4m $opt
	ck "zstd $opt" -c zstd -l6 -s 4m $opt
done
for s in CRC64 SHA256 SHA512 KECCAK256 KECCAK512 BLAKE256 BLAKE512; do ck "cksum-$s" -c zlib -l3 -S $s; done
for e in AES SALSA20; do for k in 16 32; do
	PWF=1 DARGS="-w pw"
	cp pw.master pw; ck "enc-$e-k$k" -c zstd -l6 -e $e -k $k -w pw
	cp pw.master pw; ck "enc-$e-k$k-dedup" -c lzma -l9 -D -E -e $e -k $k -w pw
	PWF= DARGS=
done; done
for t in 1 3 16; do ck "threads-$t" -c lzma -l6 -s 1m -t $t; done
# Pipe mode
for a in lz4 zstd adapt2; do
	rm -f p.pz p.out
	if "$PC" -p -c $a -l6 < dup.dat > p.pz 2>c.log && "$PC" -d - < p.pz 2>d.log | cmp -s dup.dat - && \
	   "$PC" -d - p.out < p.pz 2>>d.log && cmp -s dup.dat p.out; then
		pass=$((pass+1)); echo "PASS pipe-$a"; else fail=$((fail+1)); echo "FAIL pipe-$a"; tail -n 3 c.log d.log; fi
done
# Archive mode
for l in 1 6 9 14; do
	rm -f a.pz; find ax -delete 2>/dev/null; mkdir ax
	if timeout 600 "$PC" -a -l$l tree a >c.log 2>&1 && timeout 600 "$PC" -d a.pz ax >d.log 2>&1 && diff -r tree ax/tree >/dev/null 2>&1; then
		pass=$((pass+1)); echo "PASS archive-l$l"; else fail=$((fail+1)); echo "FAIL archive-l$l"; tail -n 3 c.log d.log; fi
done
rm -f a.pz; find ax -delete 2>/dev/null; mkdir ax
if timeout 600 "$PC" -a -c zstd -l6 -j -x tree a >c.log 2>&1 && "$PC" -i a.pz >d.log 2>&1 && grep -q num.dat d.log; then
	pass=$((pass+1)); echo "PASS archive-list"; else fail=$((fail+1)); echo "FAIL archive-list"; tail -n 3 c.log d.log; fi
# Negative tests: must fail cleanly (exit !=0, no signal)
neg() { n=$1; shift; timeout 120 "$@" >n.log 2>&1; rc=$?
	if [ $rc -ne 0 ] && [ $rc -lt 128 ]; then pass=$((pass+1)); echo "PASS neg-$n"; else fail=$((fail+1)); echo "FAIL neg-$n rc=$rc"; tail -n 2 n.log; fi; }
"$PC" -c zstd -l6 mixed.dat c1 >/dev/null 2>&1
head -c 100000 c1.pz > trunc.pz; neg truncated "$PC" -d trunc.pz t.out
cp c1.pz corrupt.pz; printf 'XXXXXXXX' | dd of=corrupt.pz bs=1 seek=5000 conv=notrunc 2>/dev/null; neg corrupt "$PC" -d corrupt.pz t2.out
neg badalgo "$PC" -c nosuch mixed.dat x
neg empty-refused "$PC" -c zstd -l6 empty.dat e0
neg badlevel "$PC" -c zstd -l 99 mixed.dat x
neg nofile "$PC" -c zstd /nonexistent x
neg garbage "$PC" -d rnd.dat g.out
cp pw.master pw; "$PC" -c zstd -e AES -w pw mixed.dat e1 >/dev/null 2>&1
printf 'wrongpw\n' > pw; neg wrongpw "$PC" -d -w pw e1.pz e1.out
rm -f c1.pz e1.pz
echo "TOTAL pass=$pass fail=$fail"
[ -n "$KEEP" ] || { cd /; rm -f "$W"/*.dat "$W"/*.pz "$W"/*.out "$W"/*.log "$W"/pw "$W"/pw.master; }
[ $fail -gt 255 ] && fail=255
exit $fail
