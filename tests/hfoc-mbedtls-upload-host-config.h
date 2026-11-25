#ifndef HFOC_MBEDTLS_UPLOAD_HOST_CONFIG_H
#define HFOC_MBEDTLS_UPLOAD_HOST_CONFIG_H

/*
 * ITEM-503 host-only mbedTLS profile for the certificate upload and record
 * proof.  It is the manifest host profile plus the X.509 certificate writer,
 * because the proof exercises the certificate owner's own generation path as
 * well as the operator import: a self-signed record is generated, an operator
 * record is adopted over it, and both are read back.  This is not the
 * production TLS configuration.
 *
 * ITEM-508 increment 24 adds the CSR writer the ACME JWS module links with,
 * because the same proof now binds the ACME account key from the persisted
 * identity (`appliance_acme_account.c`); the writer itself is never called by
 * this proof.
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
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_X509_CSR_WRITE_C

#endif
