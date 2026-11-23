/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2026 Greg Burd. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/**
 * @file pz_crypt.h
 * @brief Encryption of .pz files, format version 12.
 *
 * One key per file: Argon2id of the password and a random 256-bit salt
 * from the header. Every authenticated piece of the file belongs to a
 * stream, and is sealed with the AEAD the header names under the nonce
 * (stream, ordinal), with the stream and ordinal also in its associated
 * data. DESIGN.md ("Encryption") has the nonce-uniqueness argument.
 */

#ifndef	_PZ_CRYPT_H
#define	_PZ_CRYPT_H

#include <sys/types.h>
#include <stdint.h>
#include <utils.h>
#include <aead.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Stream ids. Never reuse one: each is a nonce domain. Ordinals count
 * from 0 within a stream.
 */
#define	PZ_STREAM_HEADER	0	/* the header tag, ordinal 0 */
#define	PZ_STREAM_DATA		1	/* data chunks, ordinal = chunk number */
#define	PZ_STREAM_META		2	/* metadata chunks, ordinal = their number */
#define	PZ_STREAM_END		3	/* the end record, ordinal 0 */
#define	PZ_STREAM_INDEX		4	/* reserved for the seekable index */

/* Header fields after the level: salt, KDF id and cost (big-endian). */
#define	PZ_SALT_LEN	32
#define	PZ_HDR_CRYPTO	(PZ_SALT_LEN + 16)
#define	PZ_KDF_ARGON2ID	2
/* After the 8 zero bytes that end the chunks: two counts and a tag. */
#define	PZ_END_LEN	(16 + AEAD_TAGLEN)
/*
 * What may follow the end record: nothing, or a seek index, whose last 8
 * bytes are this magic (seek_index.h, SX_TRAILER_MAGIC).
 */
#define	PZ_TAIL_MAGIC	"PCSEEK01"
/* Longest associated data a caller passes (a chunk's or the header's). */
#define	PZ_AD_MAX	96

/** The file key and the AEAD it is used with; alg 0 when not set. */
typedef struct {
	int alg;
	uint8_t key[AEAD_KEYLEN];
} pz_aead_t;

/** The AEAD a header's cipher flag names: AES 0x10 GCM, SALSA20 0x20 XChaCha. */
int pc_pz_aead_alg(int crypto_flag);

/**
 * New file: a random salt and the default Argon2id cost into the
 * PZ_HDR_CRYPTO bytes at hdr, and the key derived from pw into a.
 * Fails (-1) if the system has no random bytes: a repeated salt would
 * repeat the key and with it every nonce.
 */
int pc_pz_key_new(pz_aead_t *a, int crypto_flag, const uchar_t *pw, int pwlen,
    uchar_t hdr[PZ_HDR_CRYPTO]);

/**
 * Existing file: check the KDF id and cost read from hdr (before any
 * work) and derive the key. -2 on a bad id or cost (a corrupt or hostile
 * header), -1 if the KDF fails (no memory).
 */
int pc_pz_key_open(pz_aead_t *a, int crypto_flag, const uchar_t *pw, int pwlen,
    const uchar_t hdr[PZ_HDR_CRYPTO]);

/** Wipe the key. Safe on a zeroed or already wiped a. */
void pc_pz_key_wipe(pz_aead_t *a);

/**
 * Seal len bytes at buf in place as piece ord of stream, with associated
 * data (stream, ord, then ad); write the tag. adlen is at most PZ_AD_MAX.
 */
int pc_pz_seal(const pz_aead_t *a, int stream, uint64_t ord, const uchar_t *ad,
    size_t adlen, uchar_t *buf, size_t len, uchar_t tag[AEAD_TAGLEN]);

/** Open what pc_pz_seal() made, in place. -1 if anything differs. */
int pc_pz_open(const pz_aead_t *a, int stream, uint64_t ord, const uchar_t *ad,
    size_t adlen, uchar_t *buf, size_t len, const uchar_t tag[AEAD_TAGLEN]);

/** The end record: the data and metadata chunk counts, authenticated. */
int pc_pz_end_make(const pz_aead_t *a, uint64_t ndata, uint64_t nmeta,
    uchar_t out[PZ_END_LEN]);

/** Check an end record's tag and return its counts. -1 if forged. */
int pc_pz_end_check(const pz_aead_t *a, const uchar_t in[PZ_END_LEN], uint64_t *ndata,
    uint64_t *nmeta);

#ifdef	__cplusplus
}
#endif

#endif
