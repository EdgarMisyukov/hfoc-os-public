#ifndef HFOC_SHA256_H
#define HFOC_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32U
#define SHA256_BLOCK_SIZE 64U
#define SHA256_MAX_INPUT_BYTES (UINT64_MAX / 8U)

enum sha256_status {
    SHA256_OK = 0,
    SHA256_NULL = 1,
    SHA256_BAD_STATE = 2,
    SHA256_OVERFLOW = 3,
};

struct sha256 {
    bool initialized;
    uint64_t total_bytes;
    uint32_t state[8];
    uint8_t buffer[SHA256_BLOCK_SIZE];
    uint32_t buffer_used;
};

enum sha256_status sha256_init(struct sha256 *context);
enum sha256_status sha256_update(
    struct sha256 *context,
    const void *data,
    size_t data_length);
/*
 * Compress the buffered block now.
 *
 * `sha256_update()` compresses a full buffer lazily - on the next update, or in
 * `final()` - which is the right shape for a message that flows through once.
 * A caller that keeps a prepared context between messages has to spend that
 * compression once instead of once per message, so it absorbs the block
 * explicitly.  The byte counter moves with the block, so the padding that
 * `sha256_final()` writes still describes the whole message, and a caller that
 * copies the context (the credential KDF copies a pad state per derivation
 * round) gets the counter it needs.  A context whose buffer is not exactly one
 * block is refused as BAD_STATE and left untouched.
 */
enum sha256_status sha256_absorb_block(struct sha256 *context);
enum sha256_status sha256_final(
    struct sha256 *context,
    uint8_t digest[SHA256_DIGEST_SIZE]);
enum sha256_status sha256_compute(
    const void *data,
    size_t data_length,
    uint8_t digest[SHA256_DIGEST_SIZE]);
const char *sha256_status_string(enum sha256_status status);

#if defined(HFOC_SHA256_HOST_TEST)
/*
 * Host proofs measure what a derivation costs in compressions: the credential
 * KDF's whole purpose is to make them expensive, so a change to it has to name
 * the number it moved.  The counter is compiled in for the host proofs only,
 * never for the kernel.
 */
void sha256_host_test_reset_compressions(void);
unsigned long long sha256_host_test_compressions(void);
#endif

#endif
