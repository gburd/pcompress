/*
 * Unit tests for the bundled BLAKE3 (crypto/blake3, version 1.8.7).
 *
 * Known answers from the official test_vectors.json (BLAKE3 repository,
 * test_vectors/): hash, keyed_hash and derive_key over inputs of the
 * repeating bytes 0..250, the first 32 bytes of each output. They run
 * through the dispatched hasher (the CPU's widest code); then every SIMD
 * hash_many variant this CPU has must match the portable one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <blake3.h>
#include <blake3_impl.h>
#include "minunit.h"

static const struct {
	size_t len;
	const char *hash, *keyed, *derive;
} b3kat[] = {
	{ 0,
	    "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
	    "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26",
	    "2cc39783c223154fea8dfb7c1b1660f2ac2dcbd1c1de8277b0b0dd39b7e50d7d" },
	{ 1,
	    "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213",
	    "6d7878dfff2f485635d39013278ae14f1454b8c0a3a2d34bc1ab38228a80c95b",
	    "b3e2e340a117a499c6cf2398a19ee0d29cca2bb7404c73063382693bf66cb06c" },
	{ 63,
	    "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b",
	    "bb1eb5d4afa793c1ebdd9fb08def6c36d10096986ae0cfe148cd101170ce37ae",
	    "b6451e30b953c206e34644c6803724e9d2725e0893039cfc49584f991f451af3" },
	{ 64,
	    "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98",
	    "ba8ced36f327700d213f120b1a207a3b8c04330528586f414d09f2f7d9ccb7e6",
	    "a5c4a7053fa86b64746d4bb688d06ad1f02a18fce9afd3e818fefaa7126bf73e" },
	{ 65,
	    "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee",
	    "c0a4edefa2d2accb9277c371ac12fcdbb52988a86edc54f0716e1591b4326e72",
	    "51fd05c3c1cfbc8ed67d139ad76f5cf8236cd2acd26627a30c104dfd9d3ff8a8" },
	{ 1023,
	    "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11",
	    "c951ecdf03288d0fcc96ee3413563d8a6d3589547f2c2fb36d9786470f1b9d6e",
	    "74a16c1c3d44368a86e1ca6df64be6a2f64cce8f09220787450722d85725dea5" },
	{ 1024,
	    "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
	    "75c46f6f3d9eb4f55ecaaee480db732e6c2105546f1e675003687c31719c7ba4",
	    "7356cd7720d5b66b6d0697eb3177d9f8d73a4a5c5e968896eb6a689684302706" },
	{ 1025,
	    "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444",
	    "357dc55de0c7e382c900fd6e320acc04146be01db6a8ce7210b7189bd664ea69",
	    "effaa245f065fbf82ac186839a249707c3bddf6d3fdda22d1b95a3c970379bcb" },
	{ 2048,
	    "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a",
	    "879cf1fa2ea0e79126cb1063617a05b6ad9d0b696d0d757cf053439f60a99dd1",
	    "7b2945cb4fef70885cc5d78a87bf6f6207dd901ff239201351ffac04e1088a23" },
	{ 8193,
	    "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b",
	    "954a2a75420c8d6547e3ba5b98d963e6fa6491addc8c023189cc519821b4a1f5",
	    "af1e0346e389b17c23200270a64aa4e1ead98c61695d917de7d5b00491c9b0f1" },
	{ 31744,
	    "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47",
	    "efa53b389ab67c593dba624d898d0f7353ab99e4ac9d42302ee64cbf9939a419",
	    "39772aef80e0ebe60596361e45b061e8f417429d529171b6764468c22928e28e" },
	{ 102400,
	    "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085",
	    "1c35d1a5811083fd7119f5d5d1ba027b4d01c0c6c49fb6ff2cf75393ea5db4a7",
	    "4652cff7a3f385a6103b5c260fc1593e13c778dbe608efb092fe7ee69df6e9c6" },
};

static const char b3key[] = "whats the Elvish word for friend";
static const char b3ctx[] = "BLAKE3 2019-12-27 16:29:52 test vectors context";

static void
unhex(const char *h, uint8_t out[32])
{
	int i;

	for (i = 0; i < 32; i++) {
		unsigned v;

		sscanf(h + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

/* The official vectors, in each mode, fed in one call and in odd pieces. */
MU_TEST(test_blake3_kat)
{
	uint8_t *in = malloc(102400), want[32], got[32];
	size_t i, k;
	int mode, split;

	for (i = 0; i < 102400; i++)
		in[i] = (uint8_t)(i % 251);
	for (k = 0; k < sizeof (b3kat) / sizeof (b3kat[0]); k++)
		for (mode = 0; mode < 3; mode++)
			for (split = 0; split < 2; split++) {
				blake3_hasher h;
				size_t off = 0, step = split ? 1031 : b3kat[k].len + 1;
				char what[64];

				if (mode == 0)
					blake3_hasher_init(&h);
				else if (mode == 1)
					blake3_hasher_init_keyed(&h, (const uint8_t *)b3key);
				else
					blake3_hasher_init_derive_key(&h, b3ctx);
				for (; off < b3kat[k].len; off += step)
					blake3_hasher_update(&h, in + off, b3kat[k].len - off < step ?
					    b3kat[k].len - off : step);
				blake3_hasher_finalize(&h, got, 32);
				unhex(mode == 0 ? b3kat[k].hash : mode == 1 ? b3kat[k].keyed :
				    b3kat[k].derive, want);
				snprintf(what, sizeof (what), "BLAKE3 KAT len %zu mode %d split %d",
				    b3kat[k].len, mode, split);
				mu_assert(memcmp(got, want, 32) == 0, what);
			}
	free(in);
}

typedef void (*hash_many_fn)(const uint8_t *const *, size_t, size_t, const uint32_t[8],
    uint64_t, bool, uint8_t, uint8_t, uint8_t, uint8_t *);

/* Sixteen whole BLAKE3 chunks (1 KB each) hashed at once, as the tree does. */
static int
same_as_portable(hash_many_fn f)
{
	enum { N = 16, LEN = BLAKE3_CHUNK_LEN };
	static uint8_t data[N][LEN];
	const uint8_t *ptrs[N];
	uint8_t a[N * BLAKE3_OUT_LEN], b[N * BLAKE3_OUT_LEN];
	uint32_t r = 7;
	int i, j;

	for (i = 0; i < N; i++) {
		for (j = 0; j < LEN; j++) {
			r = r * 1103515245 + 12345;
			data[i][j] = (uint8_t)(r >> 16);
		}
		ptrs[i] = data[i];
	}
	blake3_hash_many_portable(ptrs, N, LEN / BLAKE3_BLOCK_LEN, IV, 5, true, 0,
	    CHUNK_START, CHUNK_END, a);
	f(ptrs, N, LEN / BLAKE3_BLOCK_LEN, IV, 5, true, 0, CHUNK_START, CHUNK_END, b);
	return (memcmp(a, b, sizeof (a)) == 0);
}

MU_TEST(test_blake3_variants)
{
#if defined(__x86_64__) || defined(__i386__)
	mu_assert(same_as_portable(blake3_hash_many_sse2), "sse2");
	if (__builtin_cpu_supports("sse4.1"))
		mu_assert(same_as_portable(blake3_hash_many_sse41), "sse4.1");
	if (__builtin_cpu_supports("avx2"))
		mu_assert(same_as_portable(blake3_hash_many_avx2), "avx2");
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl"))
		mu_assert(same_as_portable(blake3_hash_many_avx512), "avx512");
#elif BLAKE3_USE_NEON == 1
	mu_assert(same_as_portable(blake3_hash_many_neon), "neon");
#endif
}

MU_TEST_SUITE(suite_blake3)
{
	MU_RUN_TEST(test_blake3_kat);
	MU_RUN_TEST(test_blake3_variants);
}

int
main(void)
{
	printf("BLAKE3 %s\n", blake3_version());
	MU_RUN_SUITE(suite_blake3);
	MU_REPORT();
	return MU_EXIT_CODE;
}
