#include "appliance_state_crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(APPLIANCE_STATE_CRYPTO_ENABLED)

#include "sha256.h"

#include "mbedtls/aes.h"

#define APPLIANCE_STATE_CRYPTO_HMAC_BLOCK SHA256_BLOCK_SIZE

struct state_crypto_hmac_key {
    struct sha256 inner;
    struct sha256 outer;
    bool ready;
};

static void state_crypto_hmac_key_clear(struct state_crypto_hmac_key *prepared)
{
    if (prepared != NULL) {
        appliance_state_crypto_zeroize(prepared, sizeof(*prepared));
    }
}

/*
 * ITEM-611: prepare the HMAC key once per derivation.  The previous PBKDF2
 * rebuilt and hashed both pads for every one of its 20,000/200,000 rounds; the
 * account KDF already avoids that, the seed path did not.  A prepared state is
 * secret-derived and is zeroed on every exit path.
 */
static enum appliance_state_crypto_status state_crypto_hmac_key_prepare(
    const uint8_t *key,
    size_t key_length,
    struct state_crypto_hmac_key *prepared)
{
    uint8_t key_block[APPLIANCE_STATE_CRYPTO_HMAC_BLOCK];
    uint8_t pad[APPLIANCE_STATE_CRYPTO_HMAC_BLOCK];
    size_t index;

    if (key == NULL || prepared == NULL) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    memset(prepared, 0, sizeof(*prepared));
    if (key_length > APPLIANCE_STATE_CRYPTO_HMAC_BLOCK) {
        if (sha256_compute(key, key_length, key_block) != SHA256_OK) {
            return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
        }
    } else {
        memset(key_block, 0, sizeof(key_block));
        memcpy(key_block, key, key_length);
    }
    for (index = 0U; index < sizeof(pad); ++index) {
        pad[index] = (uint8_t)(key_block[index] ^ 0x36U);
    }
    if (sha256_init(&prepared->inner) != SHA256_OK ||
        sha256_update(&prepared->inner, pad, sizeof(pad)) != SHA256_OK ||
        sha256_absorb_block(&prepared->inner) != SHA256_OK) {
        goto fail;
    }
    for (index = 0U; index < sizeof(pad); ++index) {
        pad[index] = (uint8_t)(key_block[index] ^ 0x5CU);
    }
    if (sha256_init(&prepared->outer) != SHA256_OK ||
        sha256_update(&prepared->outer, pad, sizeof(pad)) != SHA256_OK ||
        sha256_absorb_block(&prepared->outer) != SHA256_OK) {
        goto fail;
    }
    prepared->ready = true;
    appliance_state_crypto_zeroize(key_block, sizeof(key_block));
    appliance_state_crypto_zeroize(pad, sizeof(pad));
    return APPLIANCE_STATE_CRYPTO_OK;

fail:
    appliance_state_crypto_zeroize(key_block, sizeof(key_block));
    appliance_state_crypto_zeroize(pad, sizeof(pad));
    state_crypto_hmac_key_clear(prepared);
    return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
}

static enum appliance_state_crypto_status state_crypto_hmac_prepared(
    const struct state_crypto_hmac_key *prepared,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH])
{
    struct sha256 inner;
    struct sha256 outer;
    uint8_t inner_mac[SHA256_DIGEST_SIZE];

    if (prepared == NULL || mac == NULL ||
        (data == NULL && data_length != 0U)) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    if (!prepared->ready) {
        return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
    }
    inner = prepared->inner;
    outer = prepared->outer;
    if ((data_length != 0U &&
         sha256_update(&inner, data, data_length) != SHA256_OK) ||
        sha256_final(&inner, inner_mac) != SHA256_OK ||
        sha256_update(&outer, inner_mac, sizeof(inner_mac)) != SHA256_OK ||
        sha256_final(&outer, mac) != SHA256_OK) {
        appliance_state_crypto_zeroize(&inner, sizeof(inner));
        appliance_state_crypto_zeroize(&outer, sizeof(outer));
        appliance_state_crypto_zeroize(inner_mac, sizeof(inner_mac));
        return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
    }
    appliance_state_crypto_zeroize(&inner, sizeof(inner));
    appliance_state_crypto_zeroize(&outer, sizeof(outer));
    appliance_state_crypto_zeroize(inner_mac, sizeof(inner_mac));
    return APPLIANCE_STATE_CRYPTO_OK;
}

/* The one-shot HMAC keeps its old contract through the prepared path. */
static enum appliance_state_crypto_status state_crypto_hmac(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH])
{
    struct state_crypto_hmac_key prepared;
    enum appliance_state_crypto_status status;

    status = state_crypto_hmac_key_prepare(key, key_length, &prepared);
    if (status == APPLIANCE_STATE_CRYPTO_OK) {
        status = state_crypto_hmac_prepared(
            &prepared, data, data_length, mac);
    }
    state_crypto_hmac_key_clear(&prepared);
    return status;
}

enum appliance_state_crypto_status appliance_state_crypto_random_fill(
    appliance_state_crypto_random_fn random,
    void *context,
    uint8_t *output,
    size_t length)
{
    if (random == NULL || output == NULL) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    if (length == 0U) {
        return APPLIANCE_STATE_CRYPTO_BAD_LENGTH;
    }
    return random(context, output, length);
}

enum appliance_state_crypto_status appliance_state_crypto_pbkdf2_sha256(
    const uint8_t *passphrase,
    size_t passphrase_length,
    const uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH],
    uint32_t iterations,
    uint8_t *output,
    size_t output_length)
{
    uint8_t block_input[APPLIANCE_STATE_CRYPTO_SALT_LENGTH + 4U];
    uint8_t u[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    uint8_t t[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    struct state_crypto_hmac_key prepared;
    enum appliance_state_crypto_status status;
    uint32_t block_index;
    uint32_t block_count;
    size_t output_offset = 0U;

    if (passphrase == NULL || salt == NULL || output == NULL) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    if (iterations == 0U ||
        iterations > APPLIANCE_STATE_CRYPTO_PBKDF2_MAX_ITERATIONS) {
        return APPLIANCE_STATE_CRYPTO_BAD_ITERATIONS;
    }
    if (output_length == 0U ||
        output_length > APPLIANCE_STATE_CRYPTO_OUTPUT_MAX) {
        return APPLIANCE_STATE_CRYPTO_BAD_LENGTH;
    }
    if (passphrase_length == 0U ||
        passphrase_length > APPLIANCE_STATE_CRYPTO_PASSPHRASE_MAX) {
        return APPLIANCE_STATE_CRYPTO_BAD_PASSPHRASE;
    }

    status = state_crypto_hmac_key_prepare(
        passphrase, passphrase_length, &prepared);
    if (status != APPLIANCE_STATE_CRYPTO_OK) {
        return status;
    }
    block_count = (uint32_t)((output_length +
                              APPLIANCE_STATE_CRYPTO_MAC_LENGTH - 1U) /
                             APPLIANCE_STATE_CRYPTO_MAC_LENGTH);
    memcpy(block_input, salt, APPLIANCE_STATE_CRYPTO_SALT_LENGTH);

    for (block_index = 1U; block_index <= block_count; ++block_index) {
        uint32_t round;
        size_t copy_length;

        /* PBKDF2 defines INT(i) as a big-endian 32-bit block index. */
        block_input[APPLIANCE_STATE_CRYPTO_SALT_LENGTH] =
            (uint8_t)(block_index >> 24U);
        block_input[APPLIANCE_STATE_CRYPTO_SALT_LENGTH + 1U] =
            (uint8_t)(block_index >> 16U);
        block_input[APPLIANCE_STATE_CRYPTO_SALT_LENGTH + 2U] =
            (uint8_t)(block_index >> 8U);
        block_input[APPLIANCE_STATE_CRYPTO_SALT_LENGTH + 3U] =
            (uint8_t)block_index;
        if (state_crypto_hmac_prepared(
                &prepared, block_input, sizeof(block_input), u) !=
            APPLIANCE_STATE_CRYPTO_OK) {
            state_crypto_hmac_key_clear(&prepared);
            return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
        }
        memcpy(t, u, sizeof(t));
        for (round = 1U; round < iterations; ++round) {
            size_t index;

            if (state_crypto_hmac_prepared(
                    &prepared, u, sizeof(u), u) !=
                APPLIANCE_STATE_CRYPTO_OK) {
                state_crypto_hmac_key_clear(&prepared);
                return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
            }
            for (index = 0U; index < sizeof(t); ++index) {
                t[index] ^= u[index];
            }
        }
        copy_length = output_length - output_offset;
        if (copy_length > APPLIANCE_STATE_CRYPTO_MAC_LENGTH) {
            copy_length = APPLIANCE_STATE_CRYPTO_MAC_LENGTH;
        }
        memcpy(output + output_offset, t, copy_length);
        output_offset += copy_length;
    }

    state_crypto_hmac_key_clear(&prepared);
    appliance_state_crypto_zeroize(u, sizeof(u));
    appliance_state_crypto_zeroize(t, sizeof(t));
    appliance_state_crypto_zeroize(block_input, sizeof(block_input));
    return APPLIANCE_STATE_CRYPTO_OK;
}

enum appliance_state_crypto_status appliance_state_crypto_hmac_sha256(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH])
{
    return state_crypto_hmac(key, key_length, data, data_length, mac);
}

enum appliance_state_crypto_status appliance_state_crypto_ctr(
    const uint8_t key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH],
    const uint8_t *input,
    uint8_t *output,
    size_t length)
{
    mbedtls_aes_context context;
    uint8_t counter[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH];
    uint8_t stream[16U];
    size_t nonce_offset = 0U;
    int result;

    if (key == NULL || nonce == NULL || input == NULL || output == NULL) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    if (length == 0U) {
        return APPLIANCE_STATE_CRYPTO_BAD_LENGTH;
    }

    memcpy(counter, nonce, sizeof(counter));
    memset(stream, 0, sizeof(stream));
    mbedtls_aes_init(&context);
    result = mbedtls_aes_setkey_enc(
        &context, key, APPLIANCE_STATE_CRYPTO_KEY_LENGTH * 8U);
    if (result == 0) {
        result = mbedtls_aes_crypt_ctr(
            &context, length, &nonce_offset, counter, stream, input,
            output);
    }
    mbedtls_aes_free(&context);
    appliance_state_crypto_zeroize(counter, sizeof(counter));
    appliance_state_crypto_zeroize(stream, sizeof(stream));
    if (result != 0) {
        return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
    }
    return APPLIANCE_STATE_CRYPTO_OK;
}

#else /* !APPLIANCE_STATE_CRYPTO_ENABLED */

enum appliance_state_crypto_status appliance_state_crypto_random_fill(
    appliance_state_crypto_random_fn random,
    void *context,
    uint8_t *output,
    size_t length)
{
    (void)random;
    (void)context;
    (void)output;
    (void)length;
    return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
}

enum appliance_state_crypto_status appliance_state_crypto_pbkdf2_sha256(
    const uint8_t *passphrase,
    size_t passphrase_length,
    const uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH],
    uint32_t iterations,
    uint8_t *output,
    size_t output_length)
{
    (void)passphrase;
    (void)passphrase_length;
    (void)salt;
    (void)iterations;
    (void)output;
    (void)output_length;
    return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
}

enum appliance_state_crypto_status appliance_state_crypto_hmac_sha256(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH])
{
    (void)key;
    (void)key_length;
    (void)data;
    (void)data_length;
    (void)mac;
    return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
}

enum appliance_state_crypto_status appliance_state_crypto_ctr(
    const uint8_t key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH],
    const uint8_t *input,
    uint8_t *output,
    size_t length)
{
    (void)key;
    (void)nonce;
    (void)input;
    (void)output;
    (void)length;
    return APPLIANCE_STATE_CRYPTO_UNAVAILABLE;
}

#endif /* APPLIANCE_STATE_CRYPTO_ENABLED */

bool appliance_state_crypto_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length)
{
    uint8_t difference = 0U;
    size_t index;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

void appliance_state_crypto_zeroize(void *buffer, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;

    if (bytes == NULL) {
        return;
    }
    while (length > 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

const char *appliance_state_crypto_status_string(
    enum appliance_state_crypto_status status)
{
    switch (status) {
    case APPLIANCE_STATE_CRYPTO_OK:
        return "OK";
    case APPLIANCE_STATE_CRYPTO_NULL:
        return "NULL";
    case APPLIANCE_STATE_CRYPTO_BAD_LENGTH:
        return "BAD_LENGTH";
    case APPLIANCE_STATE_CRYPTO_BAD_ITERATIONS:
        return "BAD_ITERATIONS";
    case APPLIANCE_STATE_CRYPTO_BAD_PASSPHRASE:
        return "BAD_PASSPHRASE";
    case APPLIANCE_STATE_CRYPTO_RANDOM_FAILED:
        return "RANDOM_FAILED";
    case APPLIANCE_STATE_CRYPTO_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_STATE_CRYPTO_OVERFLOW:
        return "OVERFLOW";
    default:
        return "UNKNOWN";
    }
}
