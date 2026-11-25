#ifndef HFOC_MBEDTLS_TLS_CONFIG_H
#define HFOC_MBEDTLS_TLS_CONFIG_H

#include "hfoc_mbedtls_config.h"
#include "hfoc_mbedtls_platform.h"

/*
 * ITEM-386 production TLS-serving mbedTLS profile.
 *
 * The frozen ITEM-262 production profile (`hfoc_mbedtls_config.h`) describes
 * the TLS 1.3 shape but deliberately carries no PSA implementation and no
 * formatting shim: until ITEM-386 nothing in the production image compiled a
 * TLS handshake, so the profile was never built into a working stack.  This
 * header is the production serving profile: it starts from the frozen profile
 * and adds exactly the families a real handshake needs.  The X.509 write side
 * the first-boot self-signed certificate and the ACME CSR need lives in the
 * frozen profile itself since item 508 increment 22 (version 4), so the
 * verifier checks it as a positive contract.
 *
 * The result is byte-for-byte the same option set the ITEM-270 QEMU TLS
 * fixture proved against a real TLS 1.3 client (`tests/hfoc-tls-qemu-config.h`)
 * with two deliberate differences:
 *
 *   - no MBEDTLS_TEST_SW_INET_PTON: that is a test-only parser switch;
 *   - no deterministic generator: MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG routes every
 *     PSA random request to appliance_production_entropy.c, which mixes a
 *     hardware RDRAND/RDSEED block through the ITEM-268 accumulator, the
 *     ITEM-269 seed controller and the ITEM-264 CTR-DRBG.  The fixture's
 *     `index * 29 + 3` generator exists nowhere in the production kernel.
 */

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_USE_PSA_CRYPTO
#define MBEDTLS_HAVE_INT32
#define MBEDTLS_SSL_IN_CONTENT_LEN 2048
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO hfoc_mbedtls_snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO hfoc_mbedtls_vsnprintf
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

#endif
