#!/bin/sh
# ci/pwcompat.sh: passwords across versions (release gate, docs/PLATFORMS.md).
#
#   ci/pwcompat.sh <new-pcompress> <old-pcompress> [workdir]
#
# Up to 4.1.0 a -w password file over 16 bytes was read as its first 15
# bytes; 4.2.0 reads the whole password and also tries the old reading when
# it opens a file. This checks, for both ciphers and passwords around the
# old limit:
#   - new reads what old wrote, and what new wrote itself;
#   - old reads what new wrote whenever the password is 16 bytes or less
#     (the forms agree there);
#   - a wrong password, and a password that only shares the first 15 bytes,
#     are refused.
# Exits 1 on any failure.
set -u
NEW=${1:?usage: ci/pwcompat.sh <new-pcompress> <old-pcompress> [workdir]}
OLD=${2:?usage: ci/pwcompat.sh <new-pcompress> <old-pcompress> [workdir]}
W=${3:-$(mktemp -d "${TMPDIR:-/tmp}/pc-pwcompat.XXXXXX")}
mkdir -p "$W" || exit 2
pass=0 fail=0
head -c 300000 /dev/urandom > "$W/in"

ok() { pass=$((pass + 1)); echo "PASS $1"; }
no() { fail=$((fail + 1)); echo "FAIL $1"; }

# A password file holding $1 and a newline (the usual form). -w wipes the
# file after reading it, so every call writes a fresh one.
pwf() { printf '%s\n' "$1" > "$W/pw"; echo "$W/pw"; }

enc() {	# enc <bin> <cipher> <password>: in -> in.pz
	rm -f "$W/in.pz"
	"$1" -c zstd -l3 -s 64k -e "$2" -w "$(pwf "$3")" "$W/in" "$W/in.pz" > /dev/null 2>&1
}
dec() {	# dec <bin> <password>: in.pz -> out, then compare
	rm -f "$W/out"
	"$1" -d -w "$(pwf "$2")" "$W/in.pz" "$W/out" > /dev/null 2> "$W/err" && cmp -s "$W/in" "$W/out"
}
# A release before 4.3 refuses encrypted files from 4.3 on (format version
# 11) up front; that is the expected result, not a password failure.
refused_newer() { grep -q 'Cannot handle newer archive version 11' "$W/err"; }

# Lengths 8, 14, 15, 16 (the old cap), 17, 20 and 40 bytes.
for pw in s3cret12 abcdefghijklmn abcdefghijklmno abcdefghijklmnop \
    abcdefghijklmnopq 0123456789abcdefTAIL \
    'a fairly long passphrase of forty bytes'; do
	n=${#pw}
	for c in AES SALSA20; do
		enc "$OLD" "$c" "$pw" && dec "$NEW" "$pw" && ok "old->new $c len=$n" || no "old->new $c len=$n"
		enc "$NEW" "$c" "$pw" && dec "$NEW" "$pw" && ok "new->new $c len=$n" || no "new->new $c len=$n"
		if [ "$n" -le 16 ]; then
			if enc "$NEW" "$c" "$pw" && dec "$OLD" "$pw"; then ok "new->old $c len=$n"
			elif refused_newer; then ok "new->old $c len=$n (refused: older release)"
			else no "new->old $c len=$n"; fi
		fi
		enc "$NEW" "$c" "$pw"
		# Differ in the first byte: an appended byte can land inside the
		# 15-byte legacy reading (a 15-byte password plus one extra byte is
		# a 17-byte file, read by the 4.1.0 rule as the first 15 bytes).
		wrong="Z$(printf '%s' "$pw" | cut -c2-)"
		if dec "$NEW" "$wrong"; then no "wrong password accepted $c len=$n"; else ok "wrong password refused $c len=$n"; fi
		if [ "$n" -gt 16 ]; then
			prefix=$(printf '%s' "$pw" | cut -c1-15)
			if dec "$NEW" "${prefix}ZZZZZZZZ"; then no "15-byte prefix accepted $c len=$n"
			else ok "15-byte prefix refused $c len=$n"; fi
		fi
	done
done

find "$W" -depth -delete 2>/dev/null
echo "pwcompat pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
