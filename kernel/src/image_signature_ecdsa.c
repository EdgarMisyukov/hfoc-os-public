/*
 * ITEM-372 verification arithmetic backend.
 *
 * This translation unit is compiled only into kernels that carry the
 * verification profile (the production kernel and the boot-chain proof
 * kernel).  It owns:
 *   - the bounded arena that every Mbed TLS temporary in this profile
 *     allocates from, so verification never touches the kernel heap and
 *     never grows the TLS allocator pools;
 *   - the ECDSA P-256 verification over a 32-byte SHA-256 digest.
 *
 * image_signature.c keeps a weak default that reports UNAVAILABLE, so a
 * kernel without this object fails closed instead of failing to link.
 */

#include "image_signature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_IMAGE_SIGNATURE_MBEDTLS)

#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

#define HFOC_VERIFY_ARENA_SIZE 65536U
#define HFOC_VERIFY_BLOCK_ALIGN 16U
#define HFOC_VERIFY_BLOCK_HEADER \
    ((sizeof(struct hfoc_verify_block) + \
      (HFOC_VERIFY_BLOCK_ALIGN - 1U)) & \
     ~(size_t)(HFOC_VERIFY_BLOCK_ALIGN - 1U))
#define HFOC_VERIFY_ARENA_PAYLOAD \
    (((HFOC_VERIFY_ARENA_SIZE - HFOC_VERIFY_BLOCK_HEADER) / \
      HFOC_VERIFY_BLOCK_ALIGN) * HFOC_VERIFY_BLOCK_ALIGN)
#define HFOC_VERIFY_ARENA_END \
    (HFOC_VERIFY_ARENA_SIZE - \
     ((HFOC_VERIFY_ARENA_SIZE - HFOC_VERIFY_BLOCK_HEADER) % \
      HFOC_VERIFY_BLOCK_ALIGN))

/*
 * A verification is one-shot: the arena is reset to a single free block
 * before each verification and every temporary Mbed TLS allocates is
 * released before the call returns, so the arena is fully reusable and a
 * verification never touches the kernel heap or the TLS pools.
 */
struct hfoc_verify_block {
    size_t size;
    size_t reserved_alignment;
    bool free;
};

static _Alignas(HFOC_VERIFY_BLOCK_ALIGN) unsigned char
    hfoc_verify_arena[HFOC_VERIFY_ARENA_SIZE];
static bool hfoc_verify_arena_ready;

static size_t hfoc_verify_align_up(size_t value)
{
    return (value + (HFOC_VERIFY_BLOCK_ALIGN - 1U)) &
           ~(size_t)(HFOC_VERIFY_BLOCK_ALIGN - 1U);
}

static struct hfoc_verify_block *hfoc_verify_first_block(void)
{
    return (struct hfoc_verify_block *)(void *)hfoc_verify_arena;
}

static unsigned char *hfoc_verify_block_payload(
    struct hfoc_verify_block *block)
{
    return (unsigned char *)(void *)block + HFOC_VERIFY_BLOCK_HEADER;
}

static struct hfoc_verify_block *hfoc_verify_block_next(
    struct hfoc_verify_block *block)
{
    unsigned char *next = (unsigned char *)(void *)block +
                          HFOC_VERIFY_BLOCK_HEADER + block->size;

    if (next >= hfoc_verify_arena + HFOC_VERIFY_ARENA_END) {
        return NULL;
    }
    return (struct hfoc_verify_block *)(void *)next;
}

static void hfoc_verify_arena_reset(void)
{
    struct hfoc_verify_block *block = hfoc_verify_first_block();

    block->size = HFOC_VERIFY_ARENA_PAYLOAD;
    block->reserved_alignment = 0U;
    block->free = true;
    hfoc_verify_arena_ready = true;
}

static void *hfoc_verify_arena_alloc(size_t size)
{
    struct hfoc_verify_block *block;
    size_t payload_size;

    if (size == 0U || size > HFOC_VERIFY_ARENA_PAYLOAD) {
        return NULL;
    }
    payload_size = hfoc_verify_align_up(size);

    block = hfoc_verify_first_block();
    while (block != NULL) {
        if (block->free && block->size >= payload_size) {
            size_t remainder = block->size - payload_size;

            /* Split only when the remainder can hold header and payload. */
            if (remainder >= HFOC_VERIFY_BLOCK_HEADER +
                             HFOC_VERIFY_BLOCK_ALIGN) {
                struct hfoc_verify_block *split =
                    (struct hfoc_verify_block *)(void *)(
                        hfoc_verify_block_payload(block) + payload_size);

                split->size = remainder - HFOC_VERIFY_BLOCK_HEADER;
                split->reserved_alignment = 0U;
                split->free = true;
                block->size = payload_size;
            }
            block->free = false;
            return hfoc_verify_block_payload(block);
        }
        block = hfoc_verify_block_next(block);
    }
    return NULL;
}

static void hfoc_verify_arena_free(void *pointer)
{
    struct hfoc_verify_block *block;
    struct hfoc_verify_block *next;

    if (pointer == NULL || !hfoc_verify_arena_ready) {
        return;
    }
    block = (struct hfoc_verify_block *)(void *)(
        (unsigned char *)pointer - HFOC_VERIFY_BLOCK_HEADER);
    if ((unsigned char *)(void *)block < hfoc_verify_arena ||
        (unsigned char *)(void *)block >=
            hfoc_verify_arena + HFOC_VERIFY_ARENA_END) {
        return;
    }
    block->free = true;

    /* Coalesce forward so a verification leaves one reusable region. */
    next = hfoc_verify_block_next(block);
    if (next != NULL && next->free) {
        block->size += HFOC_VERIFY_BLOCK_HEADER + next->size;
    }
}

void *hfoc_verify_calloc(size_t count, size_t size)
{
    size_t total;
    void *result;

    if (count == 0U || size == 0U || count > SIZE_MAX / size) {
        return NULL;
    }
    total = count * size;
    result = hfoc_verify_arena_alloc(total);
    if (result != NULL) {
        memset(result, 0, total);
    }
    return result;
}

void hfoc_verify_free(void *pointer)
{
    hfoc_verify_arena_free(pointer);
}

void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;

    if (bytes == NULL) {
        return;
    }
    while (length > 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

static enum image_signature_status image_signature_ecdsa_p256_run(
    const unsigned char public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE],
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char signature[IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE])
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r;
    mbedtls_mpi s;
    enum image_signature_status status;
    int result;

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    status = IMAGE_SIGNATURE_MALFORMED_SIGNATURE;
    result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_ecp_point_read_binary(
            &group, &point, public_key, IMAGE_SIGNATURE_PUBLIC_KEY_SIZE);
    }
    if (result == 0) {
        result = mbedtls_ecp_check_pubkey(&group, &point);
    }
    if (result == 0) {
        result = mbedtls_mpi_read_binary(&r, signature, 32U);
    }
    if (result == 0) {
        result = mbedtls_mpi_read_binary(&s, signature + 32U, 32U);
    }
    if (result == 0) {
        /* Reject r or s outside [1, n-1] before the curve arithmetic. */
        if (mbedtls_mpi_cmp_int(&r, 1) < 0 ||
            mbedtls_mpi_cmp_mpi(&r, &group.N) >= 0 ||
            mbedtls_mpi_cmp_int(&s, 1) < 0 ||
            mbedtls_mpi_cmp_mpi(&s, &group.N) >= 0) {
            result = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        } else {
            result = mbedtls_ecdsa_verify(
                &group, digest, SHA256_DIGEST_SIZE, &point, &r, &s);
            if (result == 0) {
                status = IMAGE_SIGNATURE_OK;
            } else if (result == MBEDTLS_ERR_ECP_VERIFY_FAILED) {
                status = IMAGE_SIGNATURE_MISMATCH;
            } else {
                status = IMAGE_SIGNATURE_MALFORMED_SIGNATURE;
            }
        }
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return status;
}

enum image_signature_status image_signature_ecdsa_p256_verify(
    const unsigned char public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE],
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char signature[IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE])
{
    if (public_key == NULL || digest == NULL || signature == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    if (public_key[0] != 0x04U) {
        return IMAGE_SIGNATURE_UNSUPPORTED_KEY;
    }
    hfoc_verify_arena_reset();
    return image_signature_ecdsa_p256_run(public_key, digest, signature);
}

#endif
