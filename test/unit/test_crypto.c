/*
 * Unit tests for crypto/crypto_utils.c: every checksum through each of
 * its single-thread / parallel / legacy paths, HMAC for every checksum
 * (incremental == one-shot, reinit, cleanup idempotence), checksum
 * property lookup, (de)serialisation, and the cipher contexts (round
 * trip, chunk-id nonce separation, error handling), and the AEADs and
 * Argon2id against published known answers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <crypto_utils.h>
#include <aead.h>
#include <pz_crypt.h>
#include <pcompress.h>
#include <monocypher/monocypher.h>
#include <openssl/evp.h>
#include "minunit.h"

#define	LEN	(300 * 1024 + 17)	/* > several parallel blocks, odd tail */

static uchar_t *buf;

static const char *names[] = { "CRC64", "BLAKE256", "BLAKE512", "SHA256", "SHA512",
    "KECCAK256", "KECCAK512", "SKEIN256", "SKEIN512" };
#define	NNAMES	(sizeof (names) / sizeof (names[0]))

static int
props(const char *name, int *ck, int *bytes, int *mac)
{
	*ck = 0;
	/* accept_compatible=1: SKEIN is only valid for old files / dedupe. */
	return (get_checksum_props(name, ck, bytes, mac, 1));
}

MU_TEST(test_checksum_props)
{
	int ck, b, m, i;

	for (i = 0; i < (int)NNAMES; i++) {
		mu_assert(props(names[i], &ck, &b, &m) == 0, names[i]);
		mu_assert(b == 8 || b == 32 || b == 64, "digest size");
		mu_assert(m >= b || ck == CKSUM_CRC64, "mac size");
	}
	ck = 0;
	mu_assert(get_checksum_props("NOPE", &ck, &b, &m, 1) == -1, "unknown name");
	ck = 0;
	mu_assert(get_checksum_props("SKEIN256", &ck, &b, &m, 0) == -1,
	    "SKEIN rejected as a new chunk checksum");
	/* Lookup by id. */
	ck = CKSUM_SHA512;
	mu_assert(get_checksum_props(NULL, &ck, &b, &m, 1) == 0 && b == 64, "by id");
}

MU_TEST(test_checksum_paths_agree)
{
	uchar_t d0[CKSUM_MAX_BYTES], d1[CKSUM_MAX_BYTES];
	int ck, b, m, i;

	for (i = 0; i < (int)NNAMES; i++) {
		props(names[i], &ck, &b, &m);
		memset(d0, 0, sizeof (d0));
		mu_assert(compute_checksum(d0, ck, buf, LEN, 0, 0) == 0, names[i]);
		/* Deterministic. */
		memset(d1, 0, sizeof (d1));
		compute_checksum(d1, ck, buf, LEN, 0, 0);
		mu_assert(memcmp(d0, d1, b) == 0, "checksum not deterministic");
		/* Sensitive to a single bit. */
		buf[LEN / 2] ^= 1;
		compute_checksum(d1, ck, buf, LEN, 0, 0);
		buf[LEN / 2] ^= 1;
		mu_assert(memcmp(d0, d1, b) != 0, "checksum ignores a flipped bit");
		/* Parallel (mt=1) and legacy-parallel (mt=2) modes must run. */
		mu_assert(compute_checksum(d1, ck, buf, LEN, 1, 0) == 0, "mt=1");
		mu_assert(compute_checksum(d1, ck, buf, LEN, 2, 0) == 0, "mt=2");
		/* Short and empty inputs. */
		mu_assert(compute_checksum(d1, ck, buf, 1, 1, 0) == 0, "1 byte");
		mu_assert(compute_checksum(d1, ck, buf, 0, 0, 0) == 0, "0 bytes");
	}
	mu_assert(compute_checksum(d0, 0x7fff, buf, 16, 0, 0) == -1, "bad id");
}

MU_TEST(test_serialize)
{
	uchar_t a[64], b[64], c[64];
	int i;

	for (i = 0; i < 64; i++) a[i] = (uchar_t)(i * 37 + 1);
	serialize_checksum(a, b, 64);
	deserialize_checksum(c, b, 64);
	mu_assert(memcmp(a, c, 64) == 0, "serialize round trip");
	mu_assert(b[0] == a[63], "serialized byte order");
}

static crypto_ctx_t cc_enc, cc_dec;

static void
crypto_pair(int alg, int keylen)
{
	uchar_t pw[] = "unit-test-pw", pw2[] = "unit-test-pw";
	uchar_t nonce[MAX_NONCE];

	memset(&cc_enc, 0, sizeof (cc_enc));
	memset(&cc_dec, 0, sizeof (cc_dec));
	mu_assert(init_crypto(&cc_enc, pw, (int)strlen((char *)pw), alg, NULL, 0,
	    keylen, NULL, ENCRYPT_FLAG) == 0, "init_crypto encrypt");
	/* As pcompress does: pass a copy (decrypt init wipes the nonce buffer). */
	memcpy(nonce, crypto_nonce(&cc_enc), MAX_NONCE);
	/* The scrypt cost travels in the file header. */
	cc_dec.kdf = cc_enc.kdf;
	mu_assert(init_crypto(&cc_dec, pw2, (int)strlen((char *)pw2), alg,
	    cc_enc.salt, cc_enc.saltlen, keylen, nonce,
	    DECRYPT_FLAG) == 0, "init_crypto decrypt");
}

MU_TEST(test_hmac_all)
{
	uchar_t h1[CKSUM_MAX_BYTES], h2[CKSUM_MAX_BYTES];
	unsigned int l1, l2;
	mac_ctx_t m;
	int ck, b, mb, i;

	crypto_pair(CRYPTO_ALG_AES, 32);
	for (i = 0; i < (int)NNAMES; i++) {
		props(names[i], &ck, &b, &mb);
		memset(&m, 0, sizeof (m));
		mu_assert(hmac_init(&m, ck, &cc_enc) == 0, names[i]);
		hmac_update(&m, buf, LEN);
		hmac_final(&m, h1, &l1);
		/* Incremental in uneven pieces == one shot. */
		hmac_reinit(&m);
		hmac_update(&m, buf, 1);
		hmac_update(&m, buf + 1, 4095);
		hmac_update(&m, buf + 4096, LEN - 4096);
		hmac_final(&m, h2, &l2);
		mu_assert(l1 == l2 && l1 > 0, "hmac length");
		mu_assert(memcmp(h1, h2, l1) == 0, "incremental hmac differs");
		/* Different data, different MAC. */
		hmac_reinit(&m);
		hmac_update(&m, buf + 1, LEN - 1);
		hmac_final(&m, h2, &l2);
		mu_assert(memcmp(h1, h2, l1) != 0, "hmac ignores data");
		mu_assert(hmac_cleanup(&m) == 0, "cleanup");
		mu_assert(hmac_cleanup(&m) == 0, "cleanup must be idempotent");
	}
	memset(&m, 0, sizeof (m));
	mu_assert(hmac_init(&m, 0x7fff, &cc_enc) == -1, "hmac bad checksum id");
	cleanup_crypto(&cc_enc);
	cleanup_crypto(&cc_dec);
}

MU_TEST(test_ciphers)
{
	static const int algs[] = { CRYPTO_ALG_AES, CRYPTO_ALG_SALSA20 };
	static const int keys[] = { 16, 32 };
	uchar_t *e = malloc(LEN), *d = malloc(LEN), *e2 = malloc(LEN);
	int a, k;

	for (a = 0; a < 2; a++) for (k = 0; k < 2; k++) {
		crypto_pair(algs[a], keys[k]);
		mu_assert(crypto_buf(&cc_enc, buf, e, LEN, 5) == 0, "encrypt");
		mu_assert(memcmp(buf, e, 64) != 0, "ciphertext == plaintext");
		mu_assert(crypto_buf(&cc_dec, e, d, LEN, 5) == 0, "decrypt");
		mu_assert(memcmp(buf, d, LEN) == 0, "cipher round trip");
		/* The chunk id is part of the nonce: other ids differ. */
		crypto_buf(&cc_enc, buf, e2, LEN, 6);
		mu_assert(memcmp(e, e2, LEN) != 0, "chunk id ignored by nonce");
		/* Short buffers (the SSE path used to wrap for < 16 bytes). */
		mu_assert(crypto_buf(&cc_enc, buf, e, 3, 9) == 0, "3 bytes");
		mu_assert(crypto_buf(&cc_dec, e, d, 3, 9) == 0 &&
		    memcmp(buf, d, 3) == 0, "3-byte round trip");
		crypto_clean_pkey(&cc_enc);
		cleanup_crypto(&cc_enc);
		cleanup_crypto(&cc_dec);
		cleanup_crypto(&cc_dec);	/* idempotent */
	}
	mu_assert(get_crypto_alg("AES") == CRYPTO_ALG_AES, "AES name");
	mu_assert(get_crypto_alg("SALSA20") == CRYPTO_ALG_SALSA20, "SALSA20 name");
	mu_assert(get_crypto_alg("DES") == 0, "unknown cipher");
	{
		crypto_ctx_t c; uchar_t pw[] = "x";
		memset(&c, 0, sizeof (c));
		mu_assert(init_crypto(&c, pw, 1, 99, NULL, 0, 32, NULL, ENCRYPT_FLAG) != 0,
		    "unknown algorithm id");
		cleanup_crypto(&c);
	}
	free(e); free(d); free(e2);
}

MU_TEST(test_urandom)
{
	uchar_t a[64], b[64];
	mu_assert(geturandom_bytes(a, 64) == 0, "urandom");
	mu_assert(geturandom_bytes(b, 64) == 0, "urandom 2");
	mu_assert(memcmp(a, b, 64) != 0, "urandom repeated");
}

/*
 * The scrypt cost a .pz header records (version 11): new files use the
 * stored cost, old ones the fixed pickparams() one, and values outside
 * the bounds are refused before any work (a crafted header must not make
 * the KDF allocate gigabytes or run for minutes).
 */
MU_TEST(test_kdf_params)
{
	static const uchar_t pw[] = "pw", salt[32] = { 7 };
	scrypt_params_t ok = { SCRYPT_PZ_LOGN, SCRYPT_PZ_R, SCRYPT_PZ_P };
	scrypt_params_t legacy = { 0, 0, 0 }, low = { 10, 1, 1 };
	scrypt_params_t bad[] = { { 9, 8, 1 }, { 23, 1, 1 }, { 40, 8, 1 },
	    { 17, 0, 1 }, { 17, 33, 1 }, { 17, 8, 0 }, { 17, 8, 17 },
	    { 20, 8, 1 }, { 22, 2, 1 } };
	uchar_t k1[32], k2[32];
	size_t i;

	mu_assert(scrypt_params_ok(&ok), "default cost refused");
	mu_assert(scrypt_params_ok(&low), "lowest cost refused");
	for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++) {
		mu_assert(!scrypt_params_ok(&bad[i]), "out-of-bounds cost accepted");
		mu_assert(scrypt_derive(pw, 2, salt, 32, &bad[i], k1, 32) == -1,
		    "derive with an out-of-bounds cost");
	}
	/* The stored cost is used: another cost gives another key. */
	mu_assert(scrypt_derive(pw, 2, salt, 32, &low, k1, 32) == 0, "derive low");
	mu_assert(scrypt_derive(pw, 2, salt, 32, &legacy, k2, 32) == 0, "derive legacy");
	mu_assert(memcmp(k1, k2, 32) != 0, "stored cost ignored");
	mu_assert(scrypt_derive(pw, 2, salt, 32, NULL, k1, 32) == 0 &&
	    memcmp(k1, k2, 32) == 0, "NULL is the legacy cost");
}

/* Hex string to bytes; returns the byte count. */
static size_t
unhex(const char *h, uchar_t *out)
{
	size_t n = 0;
	unsigned int b;

	while (h[0] != '\0' && h[1] != '\0' && sscanf(h, "%2x", &b) == 1) {
		out[n++] = (uchar_t)b;
		h += 2;
	}
	return (n);
}

static const char lady[] = "Ladies and Gentlemen of the class of '99: If I could "
    "offer you only one tip for the future, sunscreen would be it.";

/*
 * HChaCha20 (draft-irtf-cfrg-xchacha-03 section 2.2.1) and the
 * AEAD_XCHACHA20_POLY1305 vector (appendix A.3.1) through pc_aead_seal()
 * and pc_aead_open(): the extended-nonce construction and its OpenSSL
 * ChaCha20-Poly1305 core end to end.
 */
MU_TEST(test_xchacha_kat)
{
	uchar_t key[32], n16[16], sub[32], want[32], nonce[24], ad[12];
	uchar_t ct[200], pt[200], tag[16], wtag[16], wct[200];
	size_t len = strlen(lady), adl, i;

	unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
	unhex("000000090000004a0000000031415927", n16);
	unhex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc", want);
	pcmono_chacha20_h(sub, key, n16);
	mu_assert(memcmp(sub, want, 32) == 0, "HChaCha20 known answer");

	unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
	unhex("404142434445464748494a4b4c4d4e4f5051525354555657", nonce);
	adl = unhex("50515253c0c1c2c3c4c5c6c7", ad);
	unhex("bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
	    "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
	    "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
	    "21f9664c97637da9768812f615c68b13b52e", wct);
	unhex("c0875924c1c7987947deafd8780acf49", wtag);
	mu_assert(pc_aead_nonce_len(AEAD_XCHACHA20) == 24, "XChaCha nonce length");
	mu_assert_int_eq(0, pc_aead_seal(AEAD_XCHACHA20, key, nonce, ad, adl,
	    (const uchar_t *)lady, ct, len, tag));
	mu_assert(memcmp(ct, wct, len) == 0, "XChaCha20-Poly1305 ciphertext");
	mu_assert(memcmp(tag, wtag, 16) == 0, "XChaCha20-Poly1305 tag");
	mu_assert_int_eq(0, pc_aead_open(AEAD_XCHACHA20, key, nonce, ad, adl, ct, pt, len, tag));
	mu_assert(memcmp(pt, lady, len) == 0, "XChaCha20-Poly1305 open");

	/* Any changed bit of tag, data or associated data fails, output zeroed. */
	for (i = 0; i < 3; i++) {
		uchar_t *p = i == 0 ? tag : i == 1 ? ct + 50 : ad + 3;

		*p ^= 0x10;
		memset(pt, 0xaa, len);
		mu_assert(pc_aead_open(AEAD_XCHACHA20, key, nonce, ad, adl, ct, pt, len, tag) != 0,
		    "tampered AEAD input accepted");
		mu_assert(pt[0] == 0 && pt[len - 1] == 0, "unauthenticated plaintext left");
		*p ^= 0x10;
	}
	nonce[23] ^= 1;
	mu_assert(pc_aead_open(AEAD_XCHACHA20, key, nonce, ad, adl, ct, pt, len, tag) != 0,
	    "wrong nonce accepted");
	mu_assert(pc_aead_seal(99, key, nonce, ad, adl, ct, pt, len, tag) != 0, "unknown AEAD");
}

/*
 * RFC 8439 section 2.8.2 through the OpenSSL cipher the XChaCha20 code
 * builds on (the IETF nonce is the 4-byte constant and the 8-byte IV).
 */
MU_TEST(test_rfc8439_kat)
{
	uchar_t key[32], iv[12], ad[12], ct[200], wct[200], tag[16], wtag[16];
	size_t len = strlen(lady);
	EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
	int n;

	unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
	unhex("070000004041424344454647", iv);
	unhex("50515253c0c1c2c3c4c5c6c7", ad);
	unhex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
	    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
	    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
	    "3ff4def08e4b7a9de576d26586cec64b6116", wct);
	unhex("1ae10b594f09e26a7e902ecbd0600691", wtag);
	mu_assert(x != NULL && EVP_EncryptInit_ex(x, EVP_chacha20_poly1305(), NULL, key, iv) == 1 &&
	    EVP_EncryptUpdate(x, NULL, &n, ad, 12) == 1 &&
	    EVP_EncryptUpdate(x, ct, &n, (const uchar_t *)lady, (int)len) == 1 &&
	    EVP_EncryptFinal_ex(x, ct + n, &n) == 1 &&
	    EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1, "RFC 8439 seal");
	EVP_CIPHER_CTX_free(x);
	mu_assert(memcmp(ct, wct, len) == 0 && memcmp(tag, wtag, 16) == 0, "RFC 8439 vector");
}

/*
 * AES-256-GCM, NIST GCM specification test case 16 (as in BoringSSL's
 * aes_256_gcm_tests.txt): 60 bytes with 20 bytes of associated data.
 */
MU_TEST(test_gcm_kat)
{
	uchar_t key[32], iv[12], ad[20], in[64], ct[64], wct[64], tag[16], wtag[16], pt[64];
	size_t len, adl;

	unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key);
	unhex("cafebabefacedbaddecaf888", iv);
	adl = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", ad);
	len = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
	    "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", in);
	unhex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
	    "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662", wct);
	unhex("76fc6ece0f4e1768cddf8853bb2d551b", wtag);
	mu_assert(pc_aead_nonce_len(AEAD_AES256GCM) == 12, "GCM nonce length");
	mu_assert_int_eq(0, pc_aead_seal(AEAD_AES256GCM, key, iv, ad, adl, in, ct, len, tag));
	mu_assert(memcmp(ct, wct, len) == 0 && memcmp(tag, wtag, 16) == 0, "GCM vector");
	mu_assert_int_eq(0, pc_aead_open(AEAD_AES256GCM, key, iv, ad, adl, ct, pt, len, tag));
	mu_assert(memcmp(pt, in, len) == 0, "GCM open");
	tag[0] ^= 1;
	mu_assert(pc_aead_open(AEAD_AES256GCM, key, iv, ad, adl, ct, pt, len, tag) != 0,
	    "GCM bad tag accepted");
}

/*
 * Argon2id: RFC 9106 section 5.3 (secret and associated data, so through
 * Monocypher directly), and pc_argon2id_derive() against the reference
 * implementation (phc-winner-argon2 20190702, Ubuntu's argon2 CLI:
 * echo -n password | argon2 somesaltsomesalt -id -m 10 -t 3 -p 4 -l 32).
 */
MU_TEST(test_argon2id_kat)
{
	uchar_t pw[32], salt[16], secret[8], ad[12], out[32], want[32];
	static uint64_t work[32 * 128];
	pcmono_argon2_config cf = { PCMONO_ARGON2_ID, 32, 3, 4 };
	pcmono_argon2_inputs in = { pw, salt, 32, 16 };
	pcmono_argon2_extras ex = { secret, ad, 8, 12 };
	argon2_params_t kp = { 1024, 3, 4 };
	argon2_params_t bad[] = { { 1024, 0, 4 }, { 1024, 17, 4 }, { 1024, 3, 0 },
	    { 1024, 3, 17 }, { 31, 3, 4 }, { 2 * 1024 * 1024, 1, 1 }, { 1024 * 1024, 5, 4 } };
	size_t i;

	memset(pw, 1, sizeof (pw));
	memset(salt, 2, sizeof (salt));
	memset(secret, 3, sizeof (secret));
	memset(ad, 4, sizeof (ad));
	pcmono_argon2(out, 32, work, cf, in, ex);
	unhex("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", want);
	mu_assert(memcmp(out, want, 32) == 0, "Argon2id RFC 9106 vector");

	mu_assert_int_eq(0, pc_argon2id_derive((const uchar_t *)"password", 8,
	    (const uchar_t *)"somesaltsomesalt", 16, &kp, out, 32));
	unhex("d488a244d764d18a6db69e08a29c66824d78ddfacf324c13239e35dce7517eea", want);
	mu_assert(memcmp(out, want, 32) == 0, "Argon2id reference CLI vector");

	mu_assert(pc_argon2_params_ok(&kp), "valid cost refused");
	for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++)
		mu_assert(pc_argon2id_derive((const uchar_t *)"pw", 2,
		    (const uchar_t *)"somesalt", 8, &bad[i], out, 32) == -1,
		    "out-of-bounds Argon2 cost accepted");
}

/*
 * Format version 12 framing (pz_crypt.c): a piece opens only as the
 * stream and ordinal it was sealed as, with its associated data, under
 * the file's key; the end record carries both counts; a header cost out
 * of bounds is refused before the KDF runs. Both ciphers.
 */
MU_TEST(test_pz_v12)
{
	static const int flags[] = { CRYPTO_ALG_AES, CRYPTO_ALG_SALSA20 };
	uchar_t hdr[PZ_HDR_CRYPTO], bad[PZ_HDR_CRYPTO], p[300], q[300], tag[16], end[PZ_END_LEN];
	uchar_t ad[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	pz_aead_t a, b;
	uint64_t nd, nm;
	int f;

	memset(&a, 0, sizeof (a));
	memset(&b, 0, sizeof (b));
	for (f = 0; f < 2; f++) {
		mu_assert_int_eq(0, pc_pz_key_new(&a, flags[f], (const uchar_t *)"pw", 2, hdr));
		memcpy(p, buf, sizeof (p));
		mu_assert_int_eq(0, pc_pz_seal(&a, PZ_STREAM_DATA, 7, ad, 9, p, sizeof (p), tag));
		memcpy(q, p, sizeof (q));
		mu_assert_int_eq(0, pc_pz_open(&a, PZ_STREAM_DATA, 7, ad, 9, q, sizeof (q), tag));
		mu_assert(memcmp(q, buf, sizeof (q)) == 0, "v12 round trip");
		/* Another ordinal or stream: the 4.4 two-time pad and reordering. */
		memcpy(q, p, sizeof (q));
		mu_assert(pc_pz_open(&a, PZ_STREAM_DATA, 8, ad, 9, q, sizeof (q), tag) != 0,
		    "chunk opened at another ordinal");
		memcpy(q, p, sizeof (q));
		mu_assert(pc_pz_open(&a, PZ_STREAM_META, 7, ad, 9, q, sizeof (q), tag) != 0,
		    "data chunk opened as metadata");
		ad[0] ^= 1;
		memcpy(q, p, sizeof (q));
		mu_assert(pc_pz_open(&a, PZ_STREAM_DATA, 7, ad, 9, q, sizeof (q), tag) != 0,
		    "changed chunk header accepted");
		ad[0] ^= 1;
		/* Same password, same salt: same key; new salt: another key. */
		mu_assert_int_eq(0, pc_pz_key_open(&b, flags[f], (const uchar_t *)"pw", 2, hdr));
		memcpy(q, p, sizeof (q));
		mu_assert_int_eq(0, pc_pz_open(&b, PZ_STREAM_DATA, 7, ad, 9, q, sizeof (q), tag));
		mu_assert_int_eq(0, pc_pz_key_open(&b, flags[f], (const uchar_t *)"pW", 2, hdr));
		memcpy(q, p, sizeof (q));
		mu_assert(pc_pz_open(&b, PZ_STREAM_DATA, 7, ad, 9, q, sizeof (q), tag) != 0,
		    "wrong password accepted");
		mu_assert_int_eq(0, pc_pz_key_new(&b, flags[f], (const uchar_t *)"pw", 2, bad));
		mu_assert(memcmp(bad, hdr, PZ_SALT_LEN) != 0, "salt repeated");
		/* The end record. */
		mu_assert_int_eq(0, pc_pz_end_make(&a, 12345, 6, end));
		mu_assert(pc_pz_end_check(&a, end, &nd, &nm) == 0 && nd == 12345 && nm == 6, "end");
		end[7] ^= 1;
		mu_assert(pc_pz_end_check(&a, end, &nd, &nm) != 0, "forged chunk count accepted");
		/* Costs out of bounds: -2 before any work. */
		memcpy(bad, hdr, sizeof (bad));
		bad[PZ_SALT_LEN + 4] = 0x40;	/* 1 TiB */
		mu_assert_int_eq(-2, pc_pz_key_open(&b, flags[f], (const uchar_t *)"pw", 2, bad));
		memcpy(bad, hdr, sizeof (bad));
		bad[PZ_SALT_LEN + 3] = 1;	/* scrypt id */
		mu_assert_int_eq(-2, pc_pz_key_open(&b, flags[f], (const uchar_t *)"pw", 2, bad));
		pc_pz_key_wipe(&a);
		mu_assert(pc_pz_seal(&a, PZ_STREAM_DATA, 0, NULL, 0, p, 1, tag) != 0,
		    "wiped key used");
	}
	mu_assert(pc_pz_key_new(&a, 0x40, (const uchar_t *)"pw", 2, hdr) != 0, "unknown cipher");
	pc_pz_key_wipe(&b);
}

/*
 * The version 12 ordinal is 64 bits end to end: the nonce of piece
 * (stream, ord) is stream (u32 BE), ord (u64 BE), zeros, and the AD
 * starts with the same 12 bytes. Built here by hand and sealed with the
 * bare AEAD, ordinals 2^32 and 2^32 + 1 must match pc_pz_seal() and
 * differ from 0 and 1 (a 32-bit ordinal wraps onto them). The chunk
 * counters that feed the ordinal must be 64 bits too.
 */
MU_TEST(test_pz_ordinal64)
{
	static const uint64_t ord[] = { 0, 1, 1ULL << 32, (1ULL << 32) + 1 };
	uchar_t nonce[AEAD_NONCE_MAX], tag[4][AEAD_TAGLEN], t[AEAD_TAGLEN], p[64], q[64];
	pz_aead_t a;
	int f, i, j;

	mu_assert(sizeof (((pc_ctx_t *)0)->chunk_num) == 8, "chunk_num is not 64 bits");
	mu_assert(sizeof (((struct cmp_data *)0)->id) == 8, "chunk id is not 64 bits");
	memset(&a, 0, sizeof (a));
	for (f = 0; f < 2; f++) {
		int alg = f ? CRYPTO_ALG_SALSA20 : CRYPTO_ALG_AES;

		mu_assert_int_eq(0, pc_pz_key_new(&a, alg, (const uchar_t *)"pw", 2,
		    (uchar_t[PZ_HDR_CRYPTO]){ 0 }));
		for (i = 0; i < 4; i++) {
			memset(nonce, 0, sizeof (nonce));
			nonce[3] = PZ_STREAM_DATA;
			for (j = 0; j < 8; j++)
				nonce[4 + j] = (uchar_t)(ord[i] >> (56 - 8 * j));
			memcpy(p, buf, sizeof (p));
			mu_assert_int_eq(0, pc_aead_seal(a.alg, a.key, nonce, nonce, 12, p, p,
			    sizeof (p), tag[i]));
			memcpy(q, buf, sizeof (q));
			mu_assert_int_eq(0, pc_pz_seal(&a, PZ_STREAM_DATA, ord[i], NULL, 0, q,
			    sizeof (q), t));
			mu_assert(memcmp(p, q, sizeof (p)) == 0 && memcmp(t, tag[i], 16) == 0,
			    "ordinal nonce mapping");
		}
		mu_assert(memcmp(tag[2], tag[0], 16) != 0 && memcmp(tag[3], tag[1], 16) != 0,
		    "ordinal 2^32 wraps");
		/* The last one, sealed at 2^32 + 1, does not open as chunk 0. */
		mu_assert(pc_pz_open(&a, PZ_STREAM_DATA, 0, NULL, 0, q, sizeof (q), t) != 0,
		    "chunk 2^32 + 1 opened as chunk 0");
		pc_pz_key_wipe(&a);
	}
}

MU_TEST_SUITE(suite_crypto)
{
	MU_RUN_TEST(test_checksum_props);
	MU_RUN_TEST(test_checksum_paths_agree);
	MU_RUN_TEST(test_serialize);
	MU_RUN_TEST(test_hmac_all);
	MU_RUN_TEST(test_ciphers);
	MU_RUN_TEST(test_urandom);
	MU_RUN_TEST(test_kdf_params);
	MU_RUN_TEST(test_xchacha_kat);
	MU_RUN_TEST(test_rfc8439_kat);
	MU_RUN_TEST(test_gcm_kat);
	MU_RUN_TEST(test_argon2id_kat);
	MU_RUN_TEST(test_pz_v12);
	MU_RUN_TEST(test_pz_ordinal64);
}

int
main(void)
{
	size_t i;

	slab_init();
	init_pcompress();
	buf = malloc(LEN);
	for (i = 0; i < LEN; i++)
		buf[i] = (uchar_t)((i * 2654435761u) >> 11);
	MU_RUN_SUITE(suite_crypto);
	MU_REPORT();
	free(buf);
	return MU_EXIT_CODE;
}
