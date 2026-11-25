#ifndef HFOC_VERIFY_ALLOC_H
#define HFOC_VERIFY_ALLOC_H

#include <stddef.h>

/*
 * ITEM-372 verification arena boundary.
 *
 * The production verification profile routes every Mbed TLS temporary
 * through this fixed arena instead of the kernel heap or the TLS
 * connection pools, so a signature verification has a hard memory bound
 * and cannot disturb an unrelated allocator.
 */

void *hfoc_verify_calloc(size_t count, size_t size);
void hfoc_verify_free(void *pointer);

/* MBEDTLS_PLATFORM_ZEROIZE_ALT: the profile zeroizes through this bounded
 * implementation instead of a libc explicit_bzero the kernel does not have. */
void mbedtls_platform_zeroize(void *buffer, size_t length);

#endif
