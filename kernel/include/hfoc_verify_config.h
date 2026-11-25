#ifndef HFOC_VERIFY_CONFIG_H
#define HFOC_VERIFY_CONFIG_H

/*
 * HFOC OS production signature-verification profile (ITEM-372).
 *
 * This is deliberately not the TLS profile: the production kernel links
 * only the bounded ECDSA P-256 verification path and never a TLS, X.509,
 * PSA or RNG family.  Every allocation routes through the fixed arena in
 * image_signature_ecdsa.c, so a verification cannot grow the kernel heap
 * and cannot fail for a reason outside the arena bound.
 */

#include "hfoc_verify_alloc.h"

/*
 * ITEM-386 object partition: the verification closure gets its own link-time
 * symbol namespace.  The production image links this profile's objects
 * together with the complete ITEM-386 TLS-serving profile, and both profiles
 * compile the same mbedTLS translation units.  Without this rename the two
 * object sets would collide on every shared symbol; with it, the verification
 * objects define hfoc_verify_mbedtls_* while the TLS closure keeps the plain
 * mbedTLS names.
 *
 * The rename is installed here, before any mbedTLS header is parsed, so every
 * declaration and every definition in this profile agree.  It changes no
 * allocator hook, algorithm, error code or buffer bound: it only decides which
 * profile's object a given call path links.
 */
#include "hfoc_verify_symbols.h"

#define MBEDTLS_CONFIG_VERSION 0x03060700

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO hfoc_verify_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO hfoc_verify_free
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

/* 32-bit limbs keep the freestanding kernel free of libgcc 128-bit
 * division helpers (__udivti3). */
#define MBEDTLS_HAVE_INT32
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* Explicitly absent: TLS, X.509, PK, PSA, RNG, entropy, DRBG, ciphers. */
#undef MBEDTLS_SSL_TLS_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CLI_C
#undef MBEDTLS_X509_CRT_PARSE_C
#undef MBEDTLS_PK_C
#undef MBEDTLS_PK_PARSE_C
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_CTR_DRBG_C
#undef MBEDTLS_ENTROPY_C
#undef MBEDTLS_AES_C
#undef MBEDTLS_GCM_C
#undef MBEDTLS_HKDF_C
#undef MBEDTLS_ECDH_C

#endif
