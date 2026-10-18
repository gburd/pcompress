#!/bin/sh
# End-to-end test of examples/backup/pcbackup.c (ROADMAP Phases 5-7).
# Usage: sh examples/backup/test.sh <pcbackup binary>
set -u
B=${1:?usage: test.sh <pcbackup>}
T=$(mktemp -d "${TMPDIR:-/tmp}/pcbackup.XXXXXX")
trap 'chmod -R u+w "$T" 2>/dev/null; find "$T" -depth -delete' EXIT
export PCBACKUP_PASSWORD="a passphrase longer than sixteen bytes"
R=$T/repo S=$T/src
pass=0 fail=0
ok() { pass=$((pass+1)); echo "  PASS: $1"; }
no() { fail=$((fail+1)); echo "  FAIL: $1"; }
check() { if eval "$2"; then ok "$1"; else no "$1"; fi; }
same() { (cd "$1" && find . -print | LC_ALL=C sort | while read -r f; do
	if [ -L "$f" ]; then echo "L $f $(readlink "$f")"; elif [ -d "$f" ]; then echo "D $f";
	else echo "F $f $(cksum < "$f")"; fi; done) > "$T/a.lst"
	(cd "$2" && find . -print | LC_ALL=C sort | while read -r f; do
	if [ -L "$f" ]; then echo "L $f $(readlink "$f")"; elif [ -d "$f" ]; then echo "D $f";
	else echo "F $f $(cksum < "$f")"; fi; done) > "$T/b.lst"
	cmp -s "$T/a.lst" "$T/b.lst"; }

mkdir -p "$S/docs/deep/er" "$S/empty"
head -c 3000000 /dev/urandom > "$S/random.bin"
i=0; while [ $i -lt 2000 ]; do echo "line $i of a text file that compresses"; i=$((i+1)); done > "$S/docs/text.txt"
cp "$S/docs/text.txt" "$S/docs/deep/er/copy.txt"
: > "$S/zero-length"
ln -s docs/text.txt "$S/link"
chmod 640 "$S/docs/text.txt"
chmod 4755 "$S/zero-length" 2>/dev/null	# refused in some sandboxes (Nix)

$B init "$R" || { echo "init failed"; exit 1; }
check "init refuses an existing repository" "! $B init '$R' 2>/dev/null"
s1=$($B backup "$R" "$S" 2> "$T/b1.log")
check "first backup" "[ -n '$s1' ] && grep -q 'chunks stored' '$T/b1.log'"
check "identical files dedupe within a backup" "grep -q '[1-9][0-9]* already present' '$T/b1.log'"
s2=$($B backup "$R" "$S" 2> "$T/b2.log")
check "second backup of an unchanged tree stores no chunks" "grep -q ' 0 chunks stored' '$T/b2.log'"
check "two snapshots listed" "[ \$($B list '$R' | wc -l) -eq 2 ]"
check "list shows entries" "$B list '$R' '$s1' | grep -q 'docs/deep/er/copy.txt'"

$B restore "$R" "$s1" "$T/out1"; rc=$?
check "full restore matches" "[ $rc -eq 0 ] && same '$S' '$T/out1'"
check "file mode restored" "[ \"\$(ls -l '$T/out1/docs/text.txt' | cut -c1-10)\" = '-rw-r-----' ]"
if [ -u "$S/zero-length" ]; then
	check "setuid bit not restored (owners are not)" "[ \"\$(ls -l '$T/out1/zero-length' | cut -c1-10)\" = '-rwxr-xr-x' ]"
else
	echo "  SKIP: setuid bit (cannot set one here)"
fi
chmod 755 "$S/zero-length"
check "restore into an existing directory refused" "! $B restore '$R' '$s1' '$T/out1' 2>/dev/null"
$B restore "$R" "$s1" "$T/one" docs/deep/er/copy.txt; rc=$?
check "single-file restore" "[ $rc -eq 0 ] && cmp -s '$S/docs/deep/er/copy.txt' '$T/one/docs/deep/er/copy.txt' && [ ! -e '$T/one/random.bin' ]"

# Change one file a little: the next backup stores only new chunks.
printf 'appended\n' >> "$S/random.bin"
$B backup "$R" "$S" > /dev/null 2> "$T/b3.log"
n3=$(sed -n 's/.* \([0-9]*\) chunks stored.*/\1/p' "$T/b3.log")
check "small change stores few chunks ($n3)" "[ '$n3' -ge 1 ] && [ '$n3' -le 3 ]"

objs=$(ls "$R/objects" | wc -l)
$B forget "$R" "$s1"
$B forget "$R" "$s2"
$B prune "$R" 2> "$T/p.log"
check "prune removes unreferenced objects" "[ \$(ls '$R/objects' | wc -l) -lt $objs ] && grep -q '[1-9][0-9]* objects removed' '$T/p.log'"
s4=$($B list "$R")
$B restore "$R" "$s4" "$T/out4"; rc=$?
check "restore after prune" "[ $rc -eq 0 ] && same '$S' '$T/out4'"
$B backup "$R" "$S" > /dev/null 2> "$T/b5.log"
check "index rebuilt by prune: unchanged tree stores nothing" "grep -q ' 0 chunks stored' '$T/b5.log'"

# Prune stopped after deleting objects (simulated: the index is gone, as
# prune removes it first, and objects are missing). The next backup must
# store them again rather than trust a stale index.
rm -f "$R/index"
for o in $(ls "$R/objects" | head -3); do rm -f "$R/objects/$o"; done
s6=$($B backup "$R" "$S" 2> "$T/b6.log")
check "backup after an interrupted prune re-stores missing chunks" "! grep -q ' 0 chunks stored' '$T/b6.log'"
$B restore "$R" "$s6" "$T/out6r"; rc=$?
check "and that snapshot restores" "[ $rc -eq 0 ] && same '$S' '$T/out6r'"

# Hostile storage.
PCBACKUP_PASSWORD=wrong $B list "$R" > /dev/null 2>&1
check "wrong password refused" "[ $? -ne 0 ]"
o=$(ls "$R/objects" | head -1); o2=$(ls "$R/objects" | sed -n 2p)
cp "$R/objects/$o" "$T/save"
cp "$R/objects/$o2" "$R/objects/$o"
check "swapped object detected" "! $B restore '$R' '$s4' '$T/out5' 2>/dev/null"
cp "$T/save" "$R/objects/$o"
printf 'x' | dd of="$R/objects/$o" bs=1 seek=50 conv=notrunc 2>/dev/null
check "modified object detected" "! $B restore '$R' '$s4' '$T/out6' 2>/dev/null"
cp "$T/save" "$R/objects/$o"
check "bad snapshot name refused" "! $B restore '$R' '../keyfile' '$T/out7' 2>/dev/null"

echo "pcbackup: $pass passed, $fail failed"
[ $fail -eq 0 ] && [ $pass -gt 0 ]
