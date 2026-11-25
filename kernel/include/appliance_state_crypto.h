#ifndef HFOC_APPLIANCE_STATE_CRYPTO_H
#define HFOC_APPLIANCE_STATE_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-373 state-encryption primitives.
 *
 * The production state volume is sealed with encrypt-then-MAC over
 * AES-256-CTR: a 256-bit data-encryption key, a 128-bit nonce counter and a
 * full HMAC-SHA-256 tag.  Passphrases and recovery keys never leave this
 * module in the clear: they are stretched with PBKDF2-HMAC-SHA-256 and the
 * derived key-encryption key only ever wraps the data-encryption key.
 *
 * The module is enabled only for the production kernel and for the focused
 * host proof.  Every other build links the bounded UNAVAILABLE stubs below,
 * so a proof kernel cannot silently grow a crypto dependency.
 */

#if defined(HFOC_PRODUCTION_STATE_ENCRYPTION) || \
    defined(HFOC_STATE_ENCRYPTION_HOST_TEST)
#define APPLIANCE_STATE_CRYPTO_ENABLED 1
#endif

#define APPLIANCE_STATE_CRYPTO_VERSION 1U
#define APPLIANCE_STATE_CRYPTO_KEY_LENGTH 32U
#define APPLIANCE_STATE_CRYPTO_MAC_LENGTH 32U
#define APPLIANCE_STATE_CRYPTO_TAG_LENGTH 16U
#define APPLIANCE_STATE_CRYPTO_SALT_LENGTH 16U
#define APPLIANCE_STATE_CRYPTO_NONCE_LENGTH 16U
#define APPLIANCE_STATE_CRYPTO_PASSPHRASE_MAX 128U
#define APPLIANCE_STATE_CRYPTO_OUTPUT_MAX 64U
#define APPLIANCE_STATE_CRYPTO_PBKDF2_MIN_ITERATIONS 1024U
#define APPLIANCE_STATE_CRYPTO_PBKDF2_MAX_ITERATIONS 1000000U
#define APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS 20000U

enum appliance_state_crypto_status {
    APPLIANCE_STATE_CRYPTO_OK = 0,
    APPLIANCE_STATE_CRYPTO_NULL = 1,
    APPLIANCE_STATE_CRYPTO_BAD_LENGTH = 2,
    APPLIANCE_STATE_CRYPTO_BAD_ITERATIONS = 3,
    APPLIANCE_STATE_CRYPTO_BAD_PASSPHRASE = 4,
    APPLIANCE_STATE_CRYPTO_RANDOM_FAILED = 5,
    APPLIANCE_STATE_CRYPTO_UNAVAILABLE = 6,
    APPLIANCE_STATE_CRYPTO_OVERFLOW = 7,
};

typedef enum appliance_state_crypto_status
(*appliance_state_crypto_random_fn)(
    void *context,
    uint8_t *output,
    size_t length);

enum appliance_state_crypto_status appliance_state_crypto_random_fill(
    appliance_state_crypto_random_fn random,
    void *context,
    uint8_t *output,
    size_t length);
enum appliance_state_crypto_status appliance_state_crypto_pbkdf2_sha256(
    const uint8_t *passphrase,
    size_t passphrase_length,
    const uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH],
    uint32_t iterations,
    uint8_t *output,
    size_t output_length);
enum appliance_state_crypto_status appliance_state_crypto_hmac_sha256(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH]);
enum appliance_state_crypto_status appliance_state_crypto_ctr(
    const uint8_t key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH],
    const uint8_t *input,
    uint8_t *output,
    size_t length);
bool appliance_state_crypto_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length);
void appliance_state_crypto_zeroize(void *buffer, size_t length);
const char *appliance_state_crypto_status_string(
    enum appliance_state_crypto_status status);

#endif
