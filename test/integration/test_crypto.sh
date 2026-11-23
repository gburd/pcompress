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
# Encrypted files are format version 12 and record the Argon2id cost;
# plain files stay version 10. Header: algorithm 8, version 2, flags 2,
# chunk size 8, level 4, salt 32, then KDF id, memory (KiB), passes and
# lanes (4 bytes each, big-endian) at bytes 56-71, then a 16-byte tag.
# Numeric: illumos od pads with zeros ("011").
byte_at() { echo $(( $(od -An -tu1 -j "$2" -N 1 "$1" | tr -d ' ' | sed 's/^0*\(.\)/\1/') )); }
KTF="${TMPDIR:-/tmp}/pc_kdf_$$"
head -c 50000 /dev/urandom > "$KTF"
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.pz"
$PCOMPRESS -c zstd -l 3 -e AES -w "$PWFILE" "$KTF" 2>/dev/null
if [ "$(byte_at "$KTF.pz" 9)" = 12 ] && [ "$(byte_at "$KTF.pz" 59)" = 2 ] &&
    [ "$(byte_at "$KTF.pz" 61)" = 2 ] && [ "$(byte_at "$KTF.pz" 67)" = 4 ]; then
	log_pass "encrypted file: version 12, Argon2id 128 MiB t=4 recorded"
else
	log_fail "encrypted file: version $(byte_at "$KTF.pz" 9), KDF $(byte_at "$KTF.pz" 59)"
fi
cp "$KTF.pz" "$KTF.good"
rm -f "$KTF.pz"
$PCOMPRESS -c zstd -l 3 "$KTF" 2>/dev/null
if [ "$(byte_at "$KTF.pz" 9)" = 10 ]; then
	log_pass "plain file: still version 10"
else
	log_fail "plain file: version $(byte_at "$KTF.pz" 9)"
fi
# A password with a plain file: refused, not silently decompressed (the
# plain file could be anyone's substitute for the encrypted one).
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.out"
if msg=$($PCOMPRESS -d -w "$PWFILE" "$KTF.pz" "$KTF.out" 2>&1); then
	log_fail "password with a plain file accepted"
elif echo "$msg" | grep -q 'file is not encrypted' && [ ! -s "$KTF.out" ]; then
	log_pass "password with a plain file refused"
else
	log_fail "password with a plain file: $msg"
fi
# 1 TiB of memory: refused before any work, not attempted.
cp "$KTF.good" "$KTF.pz"
printf '\100' | dd of="$KTF.pz" bs=1 seek=60 conv=notrunc 2>/dev/null
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.out"
if msg=$($PCOMPRESS -d -w "$PWFILE" "$KTF.pz" "$KTF.out" 2>&1); then
	log_fail "hostile KDF cost accepted"
elif echo "$msg" | grep -q 'Invalid key derivation parameters'; then
	log_pass "hostile KDF cost refused"
else
	log_fail "hostile KDF cost: $msg"
fi
# A valid but changed cost (3 passes) is covered by the header tag.
cp "$KTF.good" "$KTF.pz"
printf '\003' | dd of="$KTF.pz" bs=1 seek=67 conv=notrunc 2>/dev/null
printf 'kdf-test-password\n' > "$PWFILE"
rm -f "$KTF.out"
if msg=$($PCOMPRESS -d -w "$PWFILE" "$KTF.pz" "$KTF.out" 2>&1); then
	log_fail "changed KDF cost accepted"
elif echo "$msg" | grep -q 'Header verification failed'; then
	log_pass "changed KDF cost fails the header tag"
else
	log_fail "changed KDF cost: $msg"
fi
rm -f "$KTF" "$KTF.pz" "$KTF.good" "$KTF.out"

echo ""
echo "--- Tampering (format version 12) ---"
# Each change must fail decompression: a swapped, repeated or dropped
# chunk, a cut-off end record, a changed header byte, payload byte or
# metadata chunk. 4.4 accepted the swap, the drop and the cut (a chunk's
# HMAC did not bind its position, and nothing recorded the count).
TAMPER="$(cd "$(dirname "$0")/.." && pwd)/tools/pz_tamper.py"
if command -v python3 >/dev/null 2>&1; then
	TTF="${TMPDIR:-/tmp}/pc_tamper_$$"
	head -c 3000000 /dev/urandom > "$TTF"
	for alg in AES SALSA20; do
		printf 'tamper-pw\n' > "$PWFILE"
		rm -f "$TTF.pz"
		$PCOMPRESS -c zstd -l 3 -s 1m -e $alg -w "$PWFILE" "$TTF" 2>/dev/null
		for op in reorder dup drop-last cut-end header payload; do
			python3 "$TAMPER" "$TTF.pz" "$TTF.bad.pz" $op
			printf 'tamper-pw\n' > "$PWFILE"
			rm -f "$TTF.out"
			if $PCOMPRESS -d -w "$PWFILE" "$TTF.bad.pz" "$TTF.out" 2>/dev/null; then
				log_fail "$alg: $op accepted"
			elif [ -e "$TTF.out" ]; then
				log_fail "$alg: $op refused, partial output left"
			else
				log_pass "$alg: $op refused, no output"
			fi
		done
	done
	# Archive with a metadata stream: a changed metadata chunk, a lost chunk.
	TD="${TTF}.d"; TX="${TTF}.x"
	mkdir -p "$TD/sub"
	head -c 1500000 "$TTF" > "$TD/sub/a"; echo small > "$TD/b"
	printf 'tamper-pw\n' > "$PWFILE"
	rm -f "$TTF.a.pz"
	$PCOMPRESS -a -c zstd -l 3 -s 1m -e SALSA20 -w "$PWFILE" "$TD" "$TTF.a" 2>/dev/null
	for op in meta drop-last; do
		find "$TX" -depth -delete 2>/dev/null
		mkdir -p "$TX"
		python3 "$TAMPER" "$TTF.a.pz" "$TTF.bad.pz" $op
		printf 'tamper-pw\n' > "$PWFILE"
		if $PCOMPRESS -d -w "$PWFILE" "$TTF.bad.pz" "$TX" 2>/dev/null; then
			log_fail "archive: $op accepted"
		else
			log_pass "archive: $op refused"
		fi
	done
	find "$TD" "$TX" -depth -delete 2>/dev/null
	rm -f "$TTF" "$TTF.pz" "$TTF.bad.pz" "$TTF.out" "$TTF.a.pz"
else
	log_skip "tampering (no python3)"
fi

# 4.4 wrote past its header buffer encrypting a 1- or 2-byte file.
OTF="${TMPDIR:-/tmp}/pc_one_$$"
for n in 1 2; do
	head -c $n /dev/urandom > "$OTF"
	rm -f "$OTF.pz" "$OTF.out"
	printf 'tiny-pw\n' > "$PWFILE"
	if $PCOMPRESS -c zstd -l 3 -e AES -w "$PWFILE" "$OTF" 2>/dev/null &&
	    printf 'tiny-pw\n' > "$PWFILE" &&
	    $PCOMPRESS -d -w "$PWFILE" "$OTF.pz" "$OTF.out" 2>/dev/null && cmp -s "$OTF" "$OTF.out"; then
		log_pass "$n-byte file round trip"
	else
		log_fail "$n-byte file round trip"
	fi
done
rm -f "$OTF" "$OTF.pz" "$OTF.out"

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
