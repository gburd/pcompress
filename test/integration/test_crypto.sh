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
