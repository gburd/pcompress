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
 */

/*
 * OpenSSL version compatibility layer.
 *
 * Handles API differences across OpenSSL versions:
 *   - OpenSSL 1.0.x: Legacy API (HMAC_CTX on stack, HMAC_CTX_init, etc.)
 *   - OpenSSL 1.1.x: Opaque types, HMAC_CTX_new/free, return values on update/final
 *   - OpenSSL 3.0:   Deprecates low-level APIs (AES_*, HMAC_*, SHA*_Init, etc.)
 *   - OpenSSL 3.1+:  Provider-based architecture fully stabilized
 *
 * Minimum supported version: OpenSSL 1.1.1
 *
 * Strategy for OpenSSL 3.x:
 *   We suppress deprecation warnings for the low-level AES, HMAC, and SHA
 *   APIs since they still function correctly. A full migration to the EVP
 *   API would be a larger refactoring effort and is tracked separately.
 *   Each source file that uses deprecated APIs wraps its OpenSSL includes
 *   with OSSL_DEPR_PUSH / OSSL_DEPR_POP to suppress warnings locally.
 */

#ifndef _OSSL_COMPAT_H
#define _OSSL_COMPAT_H

#include <openssl/opensslv.h>
#include <openssl/crypto.h>

/*
 * ----------------------------------------------------------------
 * Version detection macros
 * ----------------------------------------------------------------
 */

/* Minimum required OpenSSL version: 1.1.1 (0x10101000L) */
#define OSSL_MIN_VERSION	0x10101000L
#define OSSL_MIN_VERSION_TEXT	"1.1.1"

/* Compile-time check: error if building against very old OpenSSL */
#if OPENSSL_VERSION_NUMBER < OSSL_MIN_VERSION
#error "Building against OpenSSL < 1.1.1 is unsupported."
#endif

/* OpenSSL 1.1.0+ made HMAC_CTX opaque and added HMAC_CTX_new/free */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
#define OSSL_HAVE_OPAQUE_HMAC_CTX	1
#else
#define OSSL_HAVE_OPAQUE_HMAC_CTX	0
#endif

/* OpenSSL 1.1.1+ (our minimum supported version) */
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
#define OSSL_HAVE_111	1
#else
#define OSSL_HAVE_111	0
#endif

/* OpenSSL 3.0.0+ deprecates low-level cipher, digest, and HMAC APIs */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define OSSL_HAVE_3X	1
#else
#define OSSL_HAVE_3X	0
#endif

/* OpenSSL 3.1.0+ (provider architecture fully stabilized) */
#if OPENSSL_VERSION_NUMBER >= 0x30100000L
#define OSSL_HAVE_31	1
#else
#define OSSL_HAVE_31	0
#endif

/*
 * ----------------------------------------------------------------
 * Deprecation warning suppression macros
 *
 * Use OSSL_DEPR_PUSH before including OpenSSL headers that declare
 * deprecated symbols, and OSSL_DEPR_POP after the last usage.
 *
 * Example:
 *   #include "ossl_compat.h"
 *   OSSL_DEPR_PUSH
 *   #include <openssl/hmac.h>
 *   ...code using HMAC_*...
 *   OSSL_DEPR_POP
 * ----------------------------------------------------------------
 */
#if defined(__GNUC__) && OSSL_HAVE_3X
#define OSSL_DEPR_PUSH \
	_Pragma("GCC diagnostic push") \
	_Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define OSSL_DEPR_POP \
	_Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER) && OSSL_HAVE_3X
#define OSSL_DEPR_PUSH \
	__pragma(warning(push)) \
	__pragma(warning(disable: 4996))
#define OSSL_DEPR_POP \
	__pragma(warning(pop))
#else
#define OSSL_DEPR_PUSH
#define OSSL_DEPR_POP
#endif

/*
 * ----------------------------------------------------------------
 * Suppress OpenSSL 3.x deprecation warnings
 *
 * The low-level AES, HMAC, and SHA APIs are deprecated in 3.0 but
 * still functional. We define OPENSSL_API_COMPAT to the 1.1.1 level
 * so that the deprecated declarations remain available without
 * compiler warnings. This MUST be defined before including any
 * OpenSSL headers that declare deprecated symbols.
 *
 * NOTE: We do NOT define OPENSSL_NO_DEPRECATED_3_0 as that would
 * hide the declarations entirely. Instead, we use compiler pragmas
 * in individual source files to suppress deprecation warnings.
 * ----------------------------------------------------------------
 */
#if OSSL_HAVE_3X
#ifndef OPENSSL_API_COMPAT
#define OPENSSL_API_COMPAT	0x10101000L
#endif
#endif /* OSSL_HAVE_3X */

/*
 * ----------------------------------------------------------------
 * HMAC compatibility shims for pre-1.1.0 (if ever needed)
 *
 * OpenSSL < 1.1.0 uses stack-allocated HMAC_CTX with HMAC_CTX_init()
 * and HMAC_CTX_cleanup().  1.1.0+ uses opaque heap-allocated contexts
 * with HMAC_CTX_new() and HMAC_CTX_free().
 *
 * Since our minimum is 1.1.1, these shims are provided only for
 * build-time safety if someone attempts to compile against 1.0.x.
 * ----------------------------------------------------------------
 */
#if !OSSL_HAVE_OPAQUE_HMAC_CTX
#include <openssl/hmac.h>
#include <stdlib.h>
#include <string.h>

static inline HMAC_CTX *HMAC_CTX_new(void)
{
	HMAC_CTX *ctx = (HMAC_CTX *)calloc(1, sizeof(HMAC_CTX));
	if (ctx != NULL)
		HMAC_CTX_init(ctx);
	return ctx;
}

static inline void HMAC_CTX_free(HMAC_CTX *ctx)
{
	if (ctx != NULL) {
		HMAC_CTX_cleanup(ctx);
		free(ctx);
	}
}
#endif /* !OSSL_HAVE_OPAQUE_HMAC_CTX */

/*
 * ----------------------------------------------------------------
 * Runtime version check
 *
 * Call this early in program startup to warn if the runtime OpenSSL
 * library is older than what we compiled against.
 *
 * Returns 0 if the version is acceptable, -1 if it is too old.
 * A warning is always printed to stderr on mismatch; the caller
 * can decide whether to treat it as fatal.
 * ----------------------------------------------------------------
 */
#include <stdio.h>

static inline int
ossl_check_version(void)
{
#if OSSL_HAVE_3X
	/* OpenSSL 3.x uses OPENSSL_version_major/minor/patch */
	if (OPENSSL_version_major() < 3) {
		fprintf(stderr,
		    "ERROR: pcompress was compiled against OpenSSL 3.x but "
		    "the runtime library reports version %u.%u.%u.\n"
		    "       Cryptographic operations will not work correctly.\n",
		    OPENSSL_version_major(),
		    OPENSSL_version_minor(),
		    OPENSSL_version_patch());
		return (-1);
	}
#elif OSSL_HAVE_111
	{
		unsigned long rtver = OpenSSL_version_num();
		if (rtver < OSSL_MIN_VERSION) {
			fprintf(stderr,
			    "ERROR: pcompress requires OpenSSL %s or later.\n"
			    "       Runtime library version: %s\n",
			    OSSL_MIN_VERSION_TEXT,
			    OpenSSL_version(OPENSSL_VERSION));
			return (-1);
		}
	}
#else
	/* Compiled against < 1.1.1 -- warn unconditionally */
	fprintf(stderr,
	    "WARNING: pcompress was compiled against OpenSSL < %s.\n"
	    "         This configuration is unsupported. Please upgrade OpenSSL.\n",
	    OSSL_MIN_VERSION_TEXT);
#endif
	return (0);
}

/*
 * Return a static string describing the compile-time OpenSSL version.
 * Useful for diagnostics and --version output.
 */
static inline const char *
ossl_compiled_version(void)
{
	return OPENSSL_VERSION_TEXT;
}

/*
 * Return a static string describing the runtime OpenSSL version.
 */
static inline const char *
ossl_runtime_version(void)
{
#if OSSL_HAVE_111
	return OpenSSL_version(OPENSSL_VERSION);
#else
	return SSLeay_version(SSLEAY_VERSION);
#endif
}

#endif /* _OSSL_COMPAT_H */
