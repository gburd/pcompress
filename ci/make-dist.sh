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
# Ship libarchive's generated configure (as upstream's own tarballs do), so
# building a release needs no autotools: libarchive 3.8 wants autoconf
# 2.71, newer than RHEL 9 and other long-lived distributions have.
mkdir "$tmp/la" && tar xf "$tmp/src.tar" -C "$tmp/la" "$d/archive/libarchive"
(cd "$tmp/la/$d/archive/libarchive" && autoreconf -fi >/dev/null 2>&1 &&
    find autom4te.cache -depth -delete) ||
	{ echo "make-dist: autoreconf in libarchive failed" >&2; exit 1; }
tar --delete -f "$tmp/src.tar" "$d/archive/libarchive"
tar rf "$tmp/src.tar" -C "$tmp/la" --owner=0 --group=0 "$d/archive/libarchive"
find "$tmp/la" -depth -delete
gzip -9n < "$tmp/src.tar" > "$out/$d.tar.gz"
rm -f "$tmp/src.tar" "$tmp/sub.tar"; rmdir "$tmp"
tar tzf "$out/$d.tar.gz" | grep -q "^$d/zstd/lib/zstd.h$"
tar tzf "$out/$d.tar.gz" | grep -q "^$d/archive/libarchive/libarchive/archive.h$"
tar tzf "$out/$d.tar.gz" | grep -q "^$d/archive/libarchive/configure$"
echo "$out/$d.tar.gz"
