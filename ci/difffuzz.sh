#!/bin/sh
# Option lists ($extra, $dx, $MODEL, the cases) split into words on
# purpose; none holds a path with spaces.
# shellcheck disable=SC2086,SC2046
# ci/difffuzz.sh: differential, cross-version fuzzing (nightly).
#
#   ci/difffuzz.sh <new-build> <old-build> [seeds] [first-seed]
#
# <new-build> and <old-build> are built source trees (./config && make),
# normally this one and the previous release (4.4.0). For each seed it
# makes a random input (test/tools/xver_model.py) and a random option set
# that the old release understands, then:
#   1. compresses with new and decompresses with old, and the reverse,
#      and new with new; every output must equal the input. A file the
#      old release cannot read (version 12, encrypted) must be refused
#      with "Cannot handle newer archive version 12"; an option set both
#      builds refuse to compress with is a skip;
#   2. builds test/tools/xver_drv.c against each library and checks the
#      gear chunk boundaries, chunk ids (plain and keyed), a random PCMF2
#      manifest and the PCIDX1 index file against the Python model, and
#      that both builds produce the same bytes;
#   3. decodes each build's manifest with the other and re-encodes it,
#      and seals chunk objects with each build and opens them with the
#      other (keyed objects of the new build must be refused by the old).
# Seeds are deterministic: a failure prints its seed, and the same seed
# repeats the same case. A case only the old release fails (it crashed
# compressing 1-byte inputs with -e, and could not read back some small
# LZP chunks of its own, both fixed in 4.5) is printed as OLDBUG and not
# counted as a failure, as long as the new build handles it. Prints "difffuzz pass=N fail=M skip=K
# oldbug=L"; exits 1 on any failure.
set -u
NEWB=$(cd "${1:?usage: ci/difffuzz.sh <new-build> <old-build> [seeds] [first-seed]}" && pwd)
OLDB=$(cd "${2:?usage: ci/difffuzz.sh <new-build> <old-build> [seeds] [first-seed]}" && pwd)
SEEDS=${3:-50}
FIRST=${4:-1}
src=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/pc-difffuzz.XXXXXX") || exit 2
MODEL="python3 $src/test/tools/xver_model.py"
pass=0 fail=0 skip=0 oldbug=0

ok() { pass=$((pass + 1)); }
no() {
	fail=$((fail + 1)); echo "FAIL seed=$seed $1"
	[ -s "$W/err" ] && head -n 3 "$W/err" | sed 's/^/     /'
}

# Each build's driver, linked to its own library.
for b in new old; do
	case $b in new) d=$NEWB ;; old) d=$OLDB ;; esac
	${CC:-cc} -O1 -I"$d/include" -o "$W/drv-$b" "$src/test/tools/xver_drv.c" \
	    -L"$d" -lpcompress -Wl,-rpath,"$d" || { echo "difffuzz: cannot build the $b driver"; exit 2; }
done

pw() { printf '%s\n' 'a pass phrase for the differential fuzzer' > "$W/pw"; echo "$W/pw"; }

# compress <bin> <opts...>: in -> c.pz (pipe mode with -p).
compress() {
	bin=$1; shift
	rm -f "$W/c.pz"
	extra=""
	case " $* " in *" -e "*) extra="-w $(pw)" ;; esac
	case " $* " in
	*" -p "*) "$bin" "$@" $extra < "$W/in" > "$W/c.pz" 2>> "$W/err" ;;
	*) "$bin" "$@" $extra "$W/in" "$W/c.pz" > /dev/null 2>> "$W/err" ;;
	esac
}

# old_selfbad <opts...>: the old build cannot round trip its own file.
old_selfbad() {
	cp "$W/c.pz" "$W/c.keep"
	compress "$OLDB/pcompress" "$@" 2> /dev/null || { mv "$W/c.keep" "$W/c.pz"; return 0; }
	rm -f "$W/out2"
	dx2=""
	case " $* " in *" -e "*) dx2="-w $(pw)" ;; esac
	"$OLDB/pcompress" -d $dx2 "$W/c.pz" "$W/out2" > /dev/null 2>&1 && cmp -s "$W/in" "$W/out2"
	r=$?
	mv "$W/c.keep" "$W/c.pz"
	[ $r -ne 0 ]
}

# roundtrip <writer> <reader> <label> <opts...>
roundtrip() {
	wb=$1 rb=$2 label=$3; shift 3
	: > "$W/err"
	if ! compress "$wb" "$@"; then
		# Refused: fine only if the other build refuses it too.
		other=$NEWB/pcompress; [ "$wb" = "$other" ] && other=$OLDB/pcompress
		: > "$W/err2"
		if ! compress "$other" "$@" 2> "$W/err2"; then
			skip=$((skip + 1))
		elif [ "$wb" = "$OLDB/pcompress" ]; then
			# Only the old release fails: its bug, fixed since. Report it.
			oldbug=$((oldbug + 1)); echo "OLDBUG seed=$seed $label: $*"
		else
			no "$label: refused by the new build only: $*"
		fi
		return
	fi
	rm -f "$W/out"
	dx=""
	case " $* " in *" -e "*) dx="-w $(pw)" ;; esac
	if "$rb" -d $dx "$W/c.pz" "$W/out" > /dev/null 2>> "$W/err" && cmp -s "$W/in" "$W/out"; then
		ok; return
	fi
	if [ "$rb" = "$OLDB/pcompress" ] && [ -n "$dx" ] &&
	    grep -q 'Cannot handle newer archive version 12' "$W/err"; then
		ok; return
	fi
	# The old reader failing on a file the old writer also cannot round
	# trip is the old release's bug, not an incompatibility.
	if [ "$rb" = "$OLDB/pcompress" ] && [ "$wb" = "$NEWB/pcompress" ] && old_selfbad "$@"; then
		oldbug=$((oldbug + 1)); echo "OLDBUG seed=$seed $label (4.4.0 fails on its own file too): $*"
		return
	fi
	no "$label: $*"
}

# The library-level checks for one seed (steps 2 and 3).
library() {
	: > "$W/err"
	$MODEL spec "$seed" "$W/spec"
	for b in new old; do
		mkdir -p "$W/m-$b"
		cp "$W/in" "$W/m-$b/in"
		if ! "$W/drv-$b" model "$W/spec" "$W/in" "$W/m-$b" 2>> "$W/err"; then
			no "driver ($b) model"; return
		fi
		if $MODEL check "$seed" "$W/m-$b" > "$W/err" 2>&1; then ok; else no "model check ($b)"; fi
	done
	for f in chunks.txt manifest.bin index.bin; do
		if cmp -s "$W/m-new/$f" "$W/m-old/$f"; then ok; else no "new and old differ in $f"; fi
	done
	for d in new:old old:new; do
		w=${d%:*} r=${d#*:}
		if "$W/drv-$r" reman "$W/m-$w/manifest.bin" "$W/reman" 2>> "$W/err" &&
		    cmp -s "$W/reman" "$W/m-$w/manifest.bin"; then ok; else no "manifest $w -> $r"; fi
	done
	alg=$(echo "zstd lz4 lzma zlib bzip2 libbsc none" | tr ' ' '\n' | sed -n "$((seed % 7 + 1))p")
	for keyed in 0 1; do
		for d in new:old old:new new:new; do
			w=${d%:*} r=${d#*:}
			: > "$W/err"
			"$W/drv-$w" seal "$W/in" "$W/objs" "$alg" $((seed % 9 + 1)) $keyed 2>> "$W/err" ||
			    { no "seal $w $alg keyed=$keyed"; continue; }
			if "$W/drv-$r" open "$W/objs" "$W/opened" $keyed 2>> "$W/err"; then
				if cmp -s "$W/in" "$W/opened"; then ok; else no "objects $w -> $r $alg keyed=$keyed"; fi
			elif [ "$d" = new:old ] && [ $keyed = 1 ]; then
				ok	# PCK2 (4.5): 4.4.0 must refuse it, and did
			else
				no "objects $w -> $r $alg keyed=$keyed"
			fi
		done
	done
}

seed=$FIRST
last=$((FIRST + SEEDS - 1))
while [ "$seed" -le "$last" ]; do
	$MODEL input "$seed" "$W/in"
	set -- $($MODEL cases "$seed" 1)
	roundtrip "$NEWB/pcompress" "$OLDB/pcompress" new-old "$@"
	roundtrip "$OLDB/pcompress" "$NEWB/pcompress" old-new "$@"
	roundtrip "$NEWB/pcompress" "$NEWB/pcompress" new-new "$@"
	library
	seed=$((seed + 1))
done

find "$W" -depth -delete 2>/dev/null
echo "difffuzz pass=$pass fail=$fail skip=$skip oldbug=$oldbug (seeds $FIRST-$last)"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
