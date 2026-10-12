#!/bin/sh
# ci/make-dist.sh <tag> [outdir]: source tarball of <tag> including the
# bundled submodules (forge-generated archives omit them and cannot build).
set -eu
tag=$1; out=${2:-.}
cd "$(dirname "$0")/.."
v=${tag#v}; d=pcompress-$v; tmp=$(mktemp -d)
git archive --format=tar --prefix="$d/" "$tag" > "$tmp/src.tar"
# Submodule commits as recorded in the tagged tree.
git ls-tree -r "$tag" | awk -F'\t' '{split($1, m, " "); if (m[2] == "commit") print m[3], $2}' > "$tmp/subs"
while read -r sha path; do
	git -C "$path" archive --format=tar --prefix="$d/$path/" "$sha" > "$tmp/sub.tar"
	tar --concatenate -f "$tmp/src.tar" "$tmp/sub.tar"
done < "$tmp/subs"
rm -f "$tmp/subs"
gzip -9n < "$tmp/src.tar" > "$out/$d.tar.gz"
rm -f "$tmp/src.tar" "$tmp/sub.tar"; rmdir "$tmp"
tar tzf "$out/$d.tar.gz" | grep -q "^$d/zstd/lib/zstd.h$"
tar tzf "$out/$d.tar.gz" | grep -q "^$d/archive/libarchive/libarchive/archive.h$"
echo "$out/$d.tar.gz"
