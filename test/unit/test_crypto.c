/*
 * Unit tests for crypto/crypto_utils.c: every checksum through each of
 * its single-thread / parallel / legacy paths, HMAC for every checksum
 * (incremental == one-shot, reinit, cleanup idempotence), checksum
 * property lookup, (de)serialisation, and the cipher contexts (round
 * trip, chunk-id nonce separation, error handling).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <utils.h>
#include <allocator.h>
#include <crypto_utils.h>
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

MU_TEST_SUITE(suite_crypto)
{
	MU_RUN_TEST(test_checksum_props);
	MU_RUN_TEST(test_checksum_paths_agree);
	MU_RUN_TEST(test_serialize);
	MU_RUN_TEST(test_hmac_all);
	MU_RUN_TEST(test_ciphers);
	MU_RUN_TEST(test_urandom);
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
