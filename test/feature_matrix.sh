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
	# Archive filter inputs: JPEG (packJPG), PNM (packPNM), x86 ELF (dispack).
	cp "$SRC/test/res/jpg/screen.jpg" tree/ 2>/dev/null
	awk 'BEGIN{printf "P6\n256 256\n255\n"; for(i=0;i<65536;i++) printf "%c%c%c", i%251, (i/256)%251, (i*7)%251}' > tree/img.ppm
	awk 'BEGIN{printf "P5\n256 256\n255\n"; for(i=0;i<65536;i++) printf "%c", (i%256+i/256)%256}' > tree/img.pgm
	for b in /bin/sh /usr/bin/env "$(command -v sh)"; do [ -f "$b" ] && { cp -L "$b" tree/prog.elf; break; }; done
	# Near-duplicate blocks for delta (bsdiff) encoding: same data, sparse edits.
	# Copies must be > deltac_min_distance (4MB) apart within one chunk.
	head -c 6000000 mixed.dat > d1; cp d1 d2
	awk 'BEGIN{srand(7); for(i=0;i<2000;i++) print int(rand()*5999990)}' | while read o; do
		printf 'Q' | dd of=d2 bs=1 seek=$o conv=notrunc 2>/dev/null; done
	cat d1 d2 > delta.dat; rm -f d1 d2
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
for s in CRC64 SHA256 SHA512 KECCAK256 KECCAK512 BLAKE256 BLAKE512; do
	ck "cksum-$s" -c zlib -l3 -S $s
	# Whole file in one chunk: checksums run in parallel (OpenMP) mode.
	ck "cksum-$s-single" -c lz4 -l1 -s 64m -S $s
done
for e in AES SALSA20; do for k in 16 32; do
	PWF=1 DARGS="-w pw"
	cp pw.master pw; ck "enc-$e-k$k" -c zstd -l6 -e $e -k $k -w pw
	cp pw.master pw; ck "enc-$e-k$k-dedup" -c lzma -l9 -D -E -e $e -k $k -w pw
	PWF= DARGS=
done; done
for t in 1 3 16; do ck "threads-$t" -c lzma -l6 -s 1m -t $t; done
# Delta (bsdiff) on near-duplicate blocks, all similarity levels/block sizes.
IN=delta.dat
for opt in "-E" "-EE" "-E -B0" "-E -B3" "-EE -B1 -L -P"; do
	ck "delta $opt" -c zstd -l3 -s 16m $opt
	ck "delta-lzma $opt" -c lzma -l9 -s 16m $opt
done
# Global dedupe: index-memory limit forces the segmented (on-disk) index;
# alternate block hashes; cache dir.
IN=dup.dat
mkdir -p cache
for h in SHA256 SHA512 KECCAK256 KECCAK512 BLAKE256 BLAKE512 SKEIN256 SKEIN512; do
	PCOMPRESS_CHUNK_HASH_GLOBAL=$h ck "gdedup-hash-$h" -c lz4 -l3 -s 2m -G
done
PCOMPRESS_INDEX_MEM=1 PCOMPRESS_CACHE_DIR=$W/cache ck "gdedup-segmented" -c zstd -l6 -s 2m -G
PCOMPRESS_INDEX_MEM=1 ck "gdedup-segmented-fixed" -c zstd -l6 -s 2m -G -F -B1
ALLOCATOR_BYPASS=1 ck "allocator-bypass" -c zstd -l9 -s 2m
ck "stats" -c zstd -l6 -s 2m -D -C -C -M
for l in 4 5 10; do ck "level-$l-autofeatures" -c adapt2 -l$l -s 2m; done
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
# Filters (packJPG/packPNM/dispack) must restore files exactly; also -T/-n/-v/-m/-K.
for opt in "-c adapt2 -l14" "-c lzma -l6 -j -x" "-c zstd -l6 -j -x -T" "-c zstd -l6 -n -v" "-c libbsc -l9 -j" "-c adapt -l11 -G"; do
	rm -f a.pz; find ax -delete 2>/dev/null; mkdir ax
	if timeout 600 "$PC" -a $opt tree a >c.log 2>&1 && timeout 600 "$PC" -d -m a.pz ax >d.log 2>&1 && diff -r tree ax/tree >/dev/null 2>&1; then
		pass=$((pass+1)); echo "PASS archive $opt"; else fail=$((fail+1)); echo "FAIL archive $opt"; tail -n 3 c.log d.log; diff -r tree ax/tree 2>&1 | head -3; fi
done
# -K: do not overwrite newer files on extraction.
rm -f a.pz; find ax -delete 2>/dev/null; mkdir -p ax/tree
# Skip a large entry (tree/sub/mixed.dat): following entries must still extract.
mkdir -p ax/tree/sub
"$PC" -a -c lz4 -l1 tree a >/dev/null 2>&1; echo newer > ax/tree/sub/mixed.dat; touch -d '+1 hour' ax/tree/sub/mixed.dat 2>/dev/null || touch ax/tree/sub/mixed.dat
"$PC" -d -K a.pz ax >d.log 2>&1
if [ "$(cat ax/tree/sub/mixed.dat)" = newer ] && cmp -s tree/num.dat ax/tree/num.dat && cmp -s tree/img.ppm ax/tree/img.ppm; then
	pass=$((pass+1)); echo "PASS archive-K"; else fail=$((fail+1)); echo "FAIL archive-K"; tail -n 3 d.log; fi
# Multiple paths on the command line; archive to stdout.
rm -f a.pz; find ax -delete 2>/dev/null; mkdir ax
if "$PC" -a -c zstd -l3 tree/num.dat tree/sub a >c.log 2>&1 && "$PC" -d a.pz ax >d.log 2>&1 && cmp -s tree/num.dat ax/tree/num.dat && cmp -s tree/sub/mixed.dat ax/tree/sub/mixed.dat; then
	pass=$((pass+1)); echo "PASS archive-multi"; else fail=$((fail+1)); echo "FAIL archive-multi"; tail -n 3 c.log d.log; fi
rm -f a.pz; find ax -delete 2>/dev/null; mkdir ax
if "$PC" -a -c zstd -l3 -T tree - > a.pz 2>c.log && "$PC" -d - ax < a.pz >d.log 2>&1 && diff -r tree ax/tree >/dev/null 2>&1; then
	pass=$((pass+1)); echo "PASS archive-stdout"; else fail=$((fail+1)); echo "FAIL archive-stdout"; tail -n 3 c.log d.log; fi
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
# Argument validation: each must be rejected without crashing.
neg bad-B "$PC" -c zstd -B 9 mixed.dat x
neg bad-t "$PC" -c zstd -t 0 mixed.dat x
neg bad-k "$PC" -c zstd -e AES -k 7 -w pw mixed.dat x
neg bad-e "$PC" -c zstd -e ROT13 mixed.dat x
neg bad-S "$PC" -c zstd -S MD5 mixed.dat x
neg bad-s "$PC" -c zstd -s 1k mixed.dat x
neg bad-s-num "$PC" -c zstd -s 12q mixed.dat x
neg F-and-D "$PC" -c zstd -F -D mixed.dat x
neg G-and-E "$PC" -c zstd -G -E mixed.dat x
neg crc64-dedup "$PC" -c zstd -D -S CRC64 mixed.dat x
neg x-not-archive "$PC" -c zstd -x mixed.dat x
neg a-and-d "$PC" -a -d mixed.dat
neg a-and-p "$PC" -a -p
neg pipe-G "$PC" -p -c zstd -G
neg pipe-enc-nopw "$PC" -p -c zstd -e AES
neg enc-on-d "$PC" -d -e AES e1.pz
neg D-on-d "$PC" -d -D e1.pz
neg too-many "$PC" -c zstd a b c
: > exists.pz; neg exists "$PC" -c zstd mixed.dat exists; rm -f exists.pz
neg dir-input "$PC" -c zstd tree x
neg no-args "$PC" -c zstd
neg i-not-archive "$PC" -i e1.pz
neg version-newer sh -c "printf 'zstd\\0\\0\\0\\0\\377\\377' > vn.pz; head -c 100 rnd.dat >> vn.pz; \"$PC\" -d vn.pz vn.out"
rm -f c1.pz e1.pz
echo "TOTAL pass=$pass fail=$fail"
[ -n "$KEEP" ] || { cd /; rm -f "$W"/*.dat "$W"/*.pz "$W"/*.out "$W"/*.log "$W"/pw "$W"/pw.master; }
[ $fail -gt 255 ] && fail=255
exit $fail
