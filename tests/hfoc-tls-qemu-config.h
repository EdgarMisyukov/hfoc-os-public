#ifndef HFOC_TLS_QEMU_CONFIG_H
#define HFOC_TLS_QEMU_CONFIG_H

/*
 * Dedicated item 270 mbedTLS profile. It starts from the frozen production
 * server profile and adds only the PSA implementation options needed to run
 * a real TLS 1.3 handshake in the dedicated QEMU proof image.
 *
 * ITEM-508 increment 22: the frozen profile now carries the X.509 write side
 * (the first-boot self-signed certificate and the ACME CSR).  This fixture
 * never writes a certificate or a request, and its kernel links neither the
 * production platform shim nor a libc string layer, so it turns the write-side
 * families back off explicitly.  The result is byte-for-byte the fixture shape
 * item 270 proved: the handshake profile plus the PSA implementation.
 */

#include "hfoc_mbedtls_config.h"

#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_PK_WRITE_C

#include <stdarg.h>
#include <stddef.h>

int hfoc_mbedtls_snprintf(
    char *s,
    size_t n,
    const char *format,
    ...);
int hfoc_mbedtls_vsnprintf(
    char *s,
    size_t n,
    const char *format,
    va_list arguments);

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_USE_PSA_CRYPTO
#define MBEDTLS_HAVE_INT32
#define MBEDTLS_SSL_IN_CONTENT_LEN 2048
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO hfoc_mbedtls_snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO hfoc_mbedtls_vsnprintf
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#define MBEDTLS_TEST_SW_INET_PTON

#endif
