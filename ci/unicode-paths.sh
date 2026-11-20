#!/bin/sh
# ci/unicode-paths.sh <pcompress> <workdir>: non-ASCII and long file names
# round-trip through compress, decompress, archive, and extract.
#
# Names have Japanese, Greek, and emoji characters; one path is longer
# than 260 characters (Windows' MAX_PATH). Leaves archive.pz in <workdir>
# (made from the tree "uni") for a cross-platform extract check:
#   ci/unicode-paths.sh --extract <pcompress> <archive.pz> <workdir>
# extracts an archive made elsewhere and compares it with the same tree.
set -eu

# make_tree <dir>: the reference tree, the same on every platform.
make_tree() {
	mkdir -p "$1/日本語/Ελληνικά" "$1/emoji😀"
	printf 'one\n' > "$1/日本語/ファイル.txt"
	printf 'two\n' > "$1/Ελληνικά.txt"
	printf 'three\n' > "$1/emoji😀/a😀b.txt"
	printf 'four\n' > "$1/日本語/Ελληνικά/x.txt"
	# 13 levels of 20 characters: over 260 characters below the tree.
	long="$1"
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
		long="$long/dir_$i-ελληνικά_long"
	done
	mkdir -p "$long"
	printf 'deep\n' > "$long/深い.txt"
}

if [ "${1:-}" = --extract ]; then
	PC=$2; ARC=$3; W=$4
	mkdir -p "$W/ref" "$W/x"
	(cd "$W/ref" && make_tree uni)
	"$PC" -d "$ARC" "$W/x"
	diff -r "$W/ref/uni" "$W/x/uni"
	echo "unicode-paths: extract of $ARC OK"
	exit 0
fi

PC=$1; W=$2
mkdir -p "$W"
cd "$W"
make_tree uni

# A single file with such a name, in file mode.
cp "uni/日本語/ファイル.txt" "単一😀.txt"
"$PC" -c zstd -l3 "単一😀.txt"
"$PC" -d "単一😀.txt.pz" "単一😀.out"
cmp "単一😀.txt" "単一😀.out"

# The file below the long path, in file mode.
deep=$(find uni -name '深い.txt')
[ "${#deep}" -gt 260 ] || { echo "unicode-paths: long path is only ${#deep} characters" >&2; exit 1; }
"$PC" -c lz4 -l1 "$deep"
"$PC" -d "$deep.pz" "$deep.out"
cmp "$deep" "$deep.out"
rm -f "$deep.pz" "$deep.out"

# The tree, in archive mode, with and without the metadata stream.
for t in "" "-T"; do
	rm -f archive.pz
	[ -d x ] && find x -depth -delete
	mkdir x
	"$PC" -a -c zstd -l6 $t uni archive
	"$PC" -d archive.pz x
	diff -r uni x/uni
	"$PC" -i archive.pz | grep -q 'ファイル.txt'
done
echo "unicode-paths: OK"
