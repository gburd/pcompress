/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012-2013 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *
 */

/**
 * @file crypto_utils.h
 * @brief Cryptographic utilities: checksums, encryption, HMAC.
 *
 * Provides a unified interface for data integrity checksums (BLAKE2, SHA-2,
 * SHA-3/Keccak, CRC64, SKEIN), symmetric encryption (AES-CTR, Salsa20-CTR),
 * and HMAC authentication. Key derivation uses the Scrypt algorithm from
 * Tarsnap.
 */

#ifndef	_CRYPTO_UTILS_H
#define	_CRYPTO_UTILS_H

#include <arpa/nameser_compat.h>
#include <sys/types.h>
#include <stdint.h>

#include <utils.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	MAX_PW_LEN	16          /**< Maximum password length in characters. */
#define	CKSUM_MASK		0x700   /**< Bitmask for checksum type in file flags. */
#define	CKSUM_MAX_BYTES		64  /**< Maximum checksum size in bytes. */
#define	DEFAULT_CKSUM		"BLAKE256" /**< Default checksum algorithm name. */

/*
 * Default key length for Encryption and Decryption
 */
#ifndef	DEFAULT_KEYLEN
#define	DEFAULT_KEYLEN	32
#define	MAX_KEYLEN 32
#else
#define	MAX_KEYLEN DEFAULT_KEYLEN
#endif

#define	OLD_KEYLEN		16
#define	ENCRYPT_FLAG		1
#define	DECRYPT_FLAG		0
#define	CRYPTO_ALG_AES		0x10
#define	CRYPTO_ALG_SALSA20	0x20
#define	MAX_SALTLEN		64
#define	MAX_NONCE		32

#define	KECCAK_MAX_SEG	(2305843009213693950ULL)

/** Encryption/decryption context. */
typedef struct {
	void *crypto_ctx;   /**< Opaque cipher context. */
	int crypto_alg;     /**< Cipher algorithm (CRYPTO_ALG_AES or CRYPTO_ALG_SALSA20). */
	int enc_dec;        /**< ENCRYPT_FLAG or DECRYPT_FLAG. */
	uchar_t *salt;      /**< Per-session random salt. */
	uchar_t *pkey;      /**< Derived key material. */
	int saltlen;        /**< Salt length in bytes. */
	int keylen;         /**< Key length in bytes (16 or 32). */
} crypto_ctx_t;

/** HMAC authentication context. */
typedef struct {
	void *mac_ctx;          /**< Opaque HMAC state. */
	void *mac_ctx_reinit;   /**< Saved state for reinit. */
	int mac_cksum;          /**< Checksum algorithm used for HMAC. */
} mac_ctx_t;

/** @name Checksum Functions */
/**@{*/

/**
 * Compute a checksum over a buffer.
 * @param cksum_buf  Output buffer for the checksum (must be CKSUM_MAX_BYTES).
 * @param cksum      Checksum algorithm (cksum_t enum value).
 * @param buf        Input data buffer.
 * @param bytes      Length of input data.
 * @param mt         If non-zero, use multi-threaded variant if available.
 * @param verbose    If non-zero, print progress information.
 * @return 0 on success, -1 on failure.
 */
int compute_checksum(uchar_t *cksum_buf, int cksum, uchar_t *buf, uint64_t bytes, int mt, int verbose);

/**
 * List available checksum algorithms to a stream.
 * @param strm  Output stream.
 * @param pad   Prefix string for each line.
 */
void list_checksums(FILE *strm, char *pad);

/**
 * Get properties of a named checksum algorithm.
 * @param name                Checksum name string (e.g., "BLAKE256").
 * @param cksum               Output: cksum_t enum value.
 * @param cksum_bytes          Output: checksum size in bytes.
 * @param mac_bytes            Output: HMAC size in bytes.
 * @param accept_compatible   If non-zero, accept backward-compatible names.
 * @return 0 on success, -1 if not found.
 */
int get_checksum_props(const char *name, int *cksum, int *cksum_bytes,
		      int *mac_bytes, int accept_compatible);

/** Serialize a checksum to a byte buffer (for writing to file). */
void serialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);

/** Deserialize a checksum from a byte buffer (when reading from file). */
void deserialize_checksum(uchar_t *checksum, uchar_t *buf, int cksum_bytes);

/**@}*/

/** @name Encryption Functions */
/**@{*/

/**
 * Initialize an encryption or decryption context.
 * @param cctx       Context to initialize.
 * @param pwd        Password bytes.
 * @param pwd_len    Password length.
 * @param crypto_alg Algorithm (CRYPTO_ALG_AES or CRYPTO_ALG_SALSA20).
 * @param salt       Salt bytes (NULL to generate new).
 * @param saltlen    Salt length.
 * @param keylen     Key length (16 or 32).
 * @param nonce      Nonce bytes (NULL to generate new).
 * @param enc_dec    ENCRYPT_FLAG or DECRYPT_FLAG.
 * @return 0 on success, -1 on failure.
 */
int init_crypto(crypto_ctx_t *cctx, uchar_t *pwd, int pwd_len, int crypto_alg,
	       uchar_t *salt, int saltlen, int keylen, uchar_t *nonce, int enc_dec);

/**
 * Encrypt or decrypt a buffer.
 * @param cctx   Initialized crypto context.
 * @param from   Input buffer.
 * @param to     Output buffer.
 * @param bytes  Buffer length.
 * @param id     Chunk/block ID for CTR nonce derivation.
 * @return 0 on success, -1 on failure.
 */
int crypto_buf(crypto_ctx_t *cctx, uchar_t *from, uchar_t *to, uint64_t bytes, uint64_t id);

/** Get the nonce from a crypto context. */
uchar_t *crypto_nonce(crypto_ctx_t *cctx);

/** Securely zero and free the derived key in a crypto context. */
void crypto_clean_pkey(crypto_ctx_t *cctx);

/** Clean up all resources in a crypto context. */
void cleanup_crypto(crypto_ctx_t *cctx);

/**
 * Prompt the user for a password.
 * @param pw     Output buffer (MAX_PW_LEN bytes).
 * @param prompt Prompt string.
 * @param twice  If non-zero, prompt twice for confirmation.
 * @return Password length on success, -1 on failure.
 */
int get_pw_string(uchar_t pw[MAX_PW_LEN], const char *prompt, int twice);

/**
 * Get a crypto algorithm enum value from its name.
 * @param name  Algorithm name ("AES" or "SALSA20").
 * @return CRYPTO_ALG_AES, CRYPTO_ALG_SALSA20, or 0 if invalid.
 */
int get_crypto_alg(char *name);

/**
 * Generate cryptographically secure random bytes.
 * @param rbytes  Output buffer.
 * @param nbytes  Number of random bytes to generate.
 * @return 0 on success, -1 on failure.
 */
int geturandom_bytes(uchar_t *rbytes, int nbytes);

/**@}*/

/** @name HMAC Functions */
/**@{*/

/**
 * Initialize an HMAC context for chunk authentication.
 * @param mctx   HMAC context to initialize.
 * @param cksum  Checksum algorithm to use as HMAC basis.
 * @param cctx   Crypto context (provides key material).
 * @return 0 on success, -1 on failure.
 */
int hmac_init(mac_ctx_t *mctx, int cksum, crypto_ctx_t *cctx);

/** Reset HMAC context for computing a new HMAC. */
int hmac_reinit(mac_ctx_t *mctx);

/**
 * Feed data into an HMAC computation.
 * @param mctx  HMAC context.
 * @param data  Data to authenticate.
 * @param len   Data length.
 * @return 0 on success.
 */
int hmac_update(mac_ctx_t *mctx, uchar_t *data, uint64_t len);

/**
 * Finalize HMAC computation and output the result.
 * @param mctx  HMAC context.
 * @param hash  Output buffer for HMAC value.
 * @param len   Output: actual HMAC length.
 * @return 0 on success.
 */
int hmac_final(mac_ctx_t *mctx, uchar_t *hash, unsigned int *len);

/** Free HMAC context resources. */
int hmac_cleanup(mac_ctx_t *mctx);

/**@}*/

#ifdef	__cplusplus
}
#endif

#endif
