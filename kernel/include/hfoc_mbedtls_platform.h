#ifndef HFOC_MBEDTLS_PLATFORM_H
#define HFOC_MBEDTLS_PLATFORM_H

#include <stdarg.h>
#include <stddef.h>

/*
 * ITEM-386 production mbedTLS platform shim.
 *
 * The production TLS profile sets MBEDTLS_PLATFORM_SNPRINTF_MACRO and
 * MBEDTLS_PLATFORM_VSNPRINTF_MACRO to these two entry points, because the
 * freestanding kernel links no libc formatting.  The declarations live in a
 * header so the mbedTLS configuration header can pull them in exactly the way
 * the ITEM-270 QEMU TLS fixture does, without either profile depending on the
 * other.
 *
 * The same translation unit provides the three bounded C string helpers
 * mbedTLS 3.6.7 X.509 and PK parsing uses and the kernel string module does
 * not implement.
 */

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

#endif
