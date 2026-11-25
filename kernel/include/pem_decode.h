#ifndef HFOC_PEM_DECODE_H
#define HFOC_PEM_DECODE_H

#include <stddef.h>

/*
 * ITEM-503: decode one PEM block into DER bytes.
 *
 * The frozen mbedTLS profile deliberately leaves MBEDTLS_PEM_PARSE_C and
 * MBEDTLS_BASE64_C undefined, but a certificate an operator brings is issued
 * as PEM by every CA and every tool that exports one.  This is the one decoder
 * the BYO import path uses: it is bounded, it validates the alphabet and the
 * padding instead of guessing, and it refuses rather than truncating.
 *
 * The first block whose armour names `label` is decoded, so a bundle that
 * carries a leaf and a chain can be read one block at a time.  Text before or
 * after the block is ignored; a block that never closes, a wrong label, a
 * non-base64 character, a length that is not a multiple of four or an output
 * larger than the caller's buffer all fail with their own status.
 */
enum pem_decode_status {
    PEM_DECODE_OK = 0,
    PEM_DECODE_NULL = 1,
    PEM_DECODE_NO_BLOCK = 2,
    PEM_DECODE_BAD_BASE64 = 3,
    PEM_DECODE_TOO_LARGE = 4,
    PEM_DECODE_BAD_ARMOUR = 5,
};

enum pem_decode_status pem_decode_block(
    const char *text,
    size_t text_length,
    const char *label,
    unsigned char *output,
    size_t output_capacity,
    size_t *output_length);

const char *pem_decode_status_string(enum pem_decode_status status);

#endif
