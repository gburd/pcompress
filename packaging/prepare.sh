#!/bin/sh
# packaging/prepare.sh <srcdir> [date]
#
# Fill in the version (from pcompress.h) and date for the distribution
# recipes, in place in an unpacked release tree:
#   <srcdir>/packaging/pcompress.spec   from pcompress.spec.in (rpmbuild)
#   <srcdir>/debian/                    from packaging/debian (dpkg-buildpackage)
# The release workflow runs it before building the .rpm and .deb.
set -eu

src=$1
when=${2:-$(date -u +%s)}
ver=$(sed -n 's/^#define[[:space:]]*UTILITY_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$src/pcompress.h")
[ -n "$ver" ] || { echo "prepare.sh: no UTILITY_VERSION in $src/pcompress.h" >&2; exit 1; }

rpmdate=$(LC_ALL=C date -u -d "@$when" '+%a %b %d %Y' 2>/dev/null ||
    LC_ALL=C date -u -r "$when" '+%a %b %d %Y')
debdate=$(LC_ALL=C date -u -d "@$when" -R 2>/dev/null ||
    LC_ALL=C date -u -r "$when" '+%a, %d %b %Y %H:%M:%S +0000')

sed -e "s/@VERSION@/$ver/g" -e "s/@DATE@/$rpmdate/g" \
    "$src/packaging/pcompress.spec.in" > "$src/packaging/pcompress.spec"

mkdir -p "$src/debian"
cp -R "$src/packaging/debian/." "$src/debian/"
cat > "$src/debian/changelog" <<EOF
pcompress ($ver-1) unstable; urgency=medium

  * pcompress $ver; see Changelog.

 -- Greg Burd <greg@burd.me>  $debdate
EOF
echo "$ver"
