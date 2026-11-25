#ifndef HFOC_MBEDTLS_MANIFEST_HOST_CONFIG_H
#define HFOC_MBEDTLS_MANIFEST_HOST_CONFIG_H

/*
 * Host-only mbedTLS profile for the item 265 manifest parser proof and the
 * item 508 ACME host proofs.
 *
 * This is not the production TLS configuration. It keeps the frozen HFOC
 * allocator mapping and compiles only the X.509/PK/ECDSA/SHA-256 sources
 * needed to prove bounded DER manifest validation and, since item 508
 * increment 22, the PKCS#10 CSR the ACME finalize carries.
 *
 * ITEM-508 increment 33: the host profile mirrors the frozen profile's
 * owner-decided RSA contract, so the trust proof can verify the real ISRG
 * Root X1 chain (R10/R11).  RSA exists for verification only; PKCS#1 v2.1 and
 * key generation stay off, exactly as in `kernel/include/hfoc_mbedtls_config.h`.
 */

#include <stdio.h>

#include "hfoc_mbedtls_alloc.h"

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO hfoc_mbedtls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO hfoc_mbedtls_free
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO snprintf
#define MBEDTLS_PLATFORM_PRINTF_MACRO printf

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
/* ITEM-508 increment 33: CA chain verification, exactly as the profile. */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CSR_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#endif
