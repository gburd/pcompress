#!/bin/sh
#
# Integration test: Encryption round-trip with various crypto + compression combos.
# Tests AES and SALSA20 encryption with password files, and verifies that
# wrong passwords fail decompression.
#

PCOMPRESS="${PCOMPRESS:-$(cd "$(dirname "$0")/../.." && pwd)/pcompress}"
PASS=0
FAIL=0
SKIP=0
PWFILE="${TMPDIR:-/tmp}/pcompress_test_pwf"

log_pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
log_fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
log_skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1"; }

algo_available() {
	_aa_tmp="${TMPDIR:-/tmp}/pcompress_algo_probe_$$"
	printf 'probe' > "$_aa_tmp"
	$PCOMPRESS -c "$1" -l 1 -s 64k "$_aa_tmp" 2>/dev/null
	_aa_rv=$?
	rm -f "$_aa_tmp" "$_aa_tmp.pz"
	return $_aa_rv
}

echo "========================================="
echo " Integration: Encryption Round-Trip"
echo "========================================="

if [ ! -d datafiles ]; then
	mkdir datafiles
fi

if [ ! -f datafiles/crypto_test.dat ]; then
	dd if=/dev/urandom of=datafiles/crypto_test.dat bs=1024 count=128 2>/dev/null
fi

tf="datafiles/crypto_test.dat"

for algo in lz4 zlib zstd; do
	if ! algo_available "$algo"; then
		log_skip "${algo} (not compiled in)"
		continue
	fi

	for cipher in AES SALSA20; do
		echo ""
		echo "--- ${algo} + ${cipher} ---"

		for cksum in CRC64 SHA256; do
			label="${algo}/${cipher}/${cksum}"
			rm -f ${tf}.pz ${tf}.out

			# Compress with encryption
			echo "testpassword123" > "$PWFILE"
			$PCOMPRESS -c ${algo} -l 3 -s 1m -e ${cipher} -S ${cksum} -w "$PWFILE" ${tf} 2>/dev/null
			if [ $? -ne 0 ]; then
				log_fail "${label}: encrypted compression failed"
				rm -f ${tf}.pz
				continue
			fi

			# Verify password file was zeroed
			pw=$(cat "$PWFILE" 2>/dev/null)
			if [ "$pw" = "testpassword123" ]; then
				log_fail "${label}: password file not zeroed after compress"
			fi

			# Decompress with correct password
			echo "testpassword123" > "$PWFILE"
			$PCOMPRESS -d -w "$PWFILE" ${tf}.pz ${tf}.out 2>/dev/null
			if [ $? -ne 0 ]; then
				log_fail "${label}: encrypted decompression failed"
				rm -f ${tf}.pz ${tf}.out
				continue
			fi

			if diff -q ${tf} ${tf}.out > /dev/null 2>&1; then
				log_pass "${label}: round-trip OK"
			else
				log_fail "${label}: data mismatch"
			fi

			# Verify wrong password fails
			rm -f ${tf}.out
			echo "wrongpassword" > "$PWFILE"
			$PCOMPRESS -d -w "$PWFILE" ${tf}.pz ${tf}.out 2>/dev/null
			if [ $? -eq 0 ]; then
				log_fail "${label}: wrong password should have failed"
			else
				log_pass "${label}: wrong password rejected"
			fi

			rm -f ${tf}.pz ${tf}.out
		done
	done
done

echo ""
echo "--- Long passwords ---"
# The whole password counts (4.1.0 used only 15 bytes of a longer -w file).
LTF="${TMPDIR:-/tmp}/pc_longpw_$$"
head -c 100000 /dev/urandom > "$LTF"
long="a passphrase of considerably more than sixteen bytes"
for alg in AES SALSA20; do
	printf '%s\n' "$long" > "$PWFILE"
	rm -f "$LTF.pz" "$LTF.out"
	$PCOMPRESS -c zstd -l 3 -e $alg -w "$PWFILE" "$LTF" 2>/dev/null
	printf '%s\n' "$long" > "$PWFILE"
	if $PCOMPRESS -d -w "$PWFILE" "$LTF.pz" "$LTF.out" 2>/dev/null && cmp -s "$LTF" "$LTF.out"; then
		log_pass "$alg: ${#long}-byte password round trip"
	else
		log_fail "$alg: ${#long}-byte password round trip"
	fi
	# Same first 15 bytes, different tail: must fail.
	printf '%s\n' "a passphrase ofXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" > "$PWFILE"
	rm -f "$LTF.out"
	if $PCOMPRESS -d -w "$PWFILE" "$LTF.pz" "$LTF.out" 2>/dev/null; then
		log_fail "$alg: password tail ignored"
	else
		log_pass "$alg: password tail matters"
	fi
done
rm -f "$LTF" "$LTF.pz" "$LTF.out"

echo ""
echo "--- Key derivation cost in the header ---"
# Encrypted files are format version 11 and record the scrypt cost; plain
# files stay version 10. AES header: algorithm 8, version 2, flags 2, chunk
# size 8, level 4, salt length 4, salt 32, nonce 8, key length 4, then KDF
# id, log2 N, r and p (4 bytes each, big-endian), so log2 N ends at byte 79.
byte_at() { od -An -tu1 -j "$2" -N 1 "$1" | tr -d ' '; }
KTF="${TMPDIR:-/tmp}/pc_kdf_$$"
head -c 50000 /dev/urandom > "$KTF"
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.pz"
$PCOMPRESS -c zstd -l 3 -e AES -w "$PWFILE" "$KTF" 2>/dev/null
if [ "$(byte_at "$KTF.pz" 9)" = 11 ] && [ "$(byte_at "$KTF.pz" 79)" = 17 ]; then
	log_pass "encrypted file: version 11, log2 N 17 recorded"
else
	log_fail "encrypted file: version $(byte_at "$KTF.pz" 9), log2 N $(byte_at "$KTF.pz" 79)"
fi
cp "$KTF.pz" "$KTF.good"
rm -f "$KTF.pz"
$PCOMPRESS -c zstd -l 3 "$KTF" 2>/dev/null
if [ "$(byte_at "$KTF.pz" 9)" = 10 ]; then
	log_pass "plain file: still version 10"
else
	log_fail "plain file: version $(byte_at "$KTF.pz" 9)"
fi
# log2 N 40 (2^40 * 1 KB of memory): refused before any work, not attempted.
cp "$KTF.good" "$KTF.pz"
printf '\050' | dd of="$KTF.pz" bs=1 seek=79 conv=notrunc 2>/dev/null
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.out"
if msg=$($PCOMPRESS -d -w "$PWFILE" "$KTF.pz" "$KTF.out" 2>&1); then
	log_fail "hostile KDF cost accepted"
elif echo "$msg" | grep -q 'Invalid key derivation parameters'; then
	log_pass "hostile KDF cost refused"
else
	log_fail "hostile KDF cost: $msg"
fi
# A valid but changed cost is covered by the header HMAC.
cp "$KTF.good" "$KTF.pz"
printf '\020' | dd of="$KTF.pz" bs=1 seek=79 conv=notrunc 2>/dev/null
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.out"
if msg=$($PCOMPRESS -d -w "$PWFILE" "$KTF.pz" "$KTF.out" 2>&1); then
	log_fail "changed KDF cost accepted"
elif echo "$msg" | grep -q 'Header verification failed'; then
	log_pass "changed KDF cost fails the header HMAC"
else
	log_fail "changed KDF cost: $msg"
fi
rm -f "$KTF" "$KTF.pz" "$KTF.good" "$KTF.out"

rm -f "$PWFILE"

echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "========================================="

if [ $FAIL -gt 0 ]; then
	exit 1
fi
# Nothing ran: treat as failure so a broken harness cannot pass silently.
if [ $PASS -eq 0 ]; then
	echo "ERROR: no test cases ran"
	exit 1
fi
exit 0
