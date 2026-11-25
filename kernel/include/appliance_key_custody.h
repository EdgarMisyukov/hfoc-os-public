#ifndef HFOC_APPLIANCE_KEY_CUSTODY_H
#define HFOC_APPLIANCE_KEY_CUSTODY_H

#include "appliance_state_crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-373 key custody.
 *
 * A 512-byte custody record holds a random data-encryption key (DEK) wrapped
 * under a key-encryption key derived from an operator passphrase, a second
 * wrap under a random recovery key, both fingerprints, and an HMAC over the
 * plaintext record fields.  The record stores only wrapped key material, so it
 * can live unencrypted in the state partition while the stores stay sealed.
 *
 * The TPM binding is declared but has no backend in the frozen baseline: it
 * reports UNAVAILABLE and never falls back to a weaker binding.
 */

#define APPLIANCE_KEY_CUSTODY_VERSION 1U
#define APPLIANCE_KEY_CUSTODY_RECORD_LENGTH 512U
#define APPLIANCE_KEY_CUSTODY_MAGIC_LENGTH 8U
#define APPLIANCE_KEY_CUSTODY_DEK_LENGTH \
    APPLIANCE_STATE_CRYPTO_KEY_LENGTH
#define APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH \
    APPLIANCE_STATE_CRYPTO_KEY_LENGTH
#define APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH \
    (APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH * 2U)
#define APPLIANCE_KEY_CUSTODY_ESCROW_MAX 192U
#define APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH \
    APPLIANCE_STATE_CRYPTO_MAC_LENGTH
#define APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH 8U
#define APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256 1U

enum appliance_key_custody_binding {
    APPLIANCE_KEY_CUSTODY_BINDING_NONE = 0,
    APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE = 1,
    APPLIANCE_KEY_CUSTODY_BINDING_RECOVERY_KEY = 2,
    APPLIANCE_KEY_CUSTODY_BINDING_TPM = 3,
};

enum appliance_key_custody_state {
    APPLIANCE_KEY_CUSTODY_STATE_EMPTY = 0,
    APPLIANCE_KEY_CUSTODY_STATE_SEALED = 1,
    APPLIANCE_KEY_CUSTODY_STATE_UNSEALED = 2,
};

enum appliance_key_custody_status {
    APPLIANCE_KEY_CUSTODY_OK = 0,
    APPLIANCE_KEY_CUSTODY_NULL = 1,
    APPLIANCE_KEY_CUSTODY_BAD_MAGIC = 2,
    APPLIANCE_KEY_CUSTODY_BAD_VERSION = 3,
    APPLIANCE_KEY_CUSTODY_BAD_BINDING = 4,
    APPLIANCE_KEY_CUSTODY_BAD_KDF = 5,
    APPLIANCE_KEY_CUSTODY_BAD_LENGTH = 6,
    APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS = 7,
    APPLIANCE_KEY_CUSTODY_RESERVED_NONZERO = 8,
    APPLIANCE_KEY_CUSTODY_RECORD_TOO_LARGE = 9,
    APPLIANCE_KEY_CUSTODY_AUTH_FAILED = 10,
    APPLIANCE_KEY_CUSTODY_WRONG_KEY = 11,
    APPLIANCE_KEY_CUSTODY_KEY_REQUIRED = 12,
    APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE = 13,
    APPLIANCE_KEY_CUSTODY_CORRUPT = 14,
    APPLIANCE_KEY_CUSTODY_RANDOM_FAILED = 15,
    APPLIANCE_KEY_CUSTODY_UNSUPPORTED = 16,
    APPLIANCE_KEY_CUSTODY_ESCROW_TOO_SMALL = 17,
    APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY = 18,
    APPLIANCE_KEY_CUSTODY_BAD_PASSPHRASE = 19,
};

struct appliance_key_custody_material {
    enum appliance_key_custody_binding binding;
    const uint8_t *secret;
    size_t secret_length;
};

struct appliance_key_custody_record {
    uint32_t version;
    enum appliance_key_custody_binding binding;
    uint32_t kdf_id;
    uint32_t kdf_iterations;
    uint32_t recovery_kdf_iterations;
    uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH];
    uint8_t volume_id[APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH];
    uint8_t wrapped_dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t wrapped_dek_tag[APPLIANCE_STATE_CRYPTO_TAG_LENGTH];
    uint8_t recovery_salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH];
    uint8_t recovery_wrapped_dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t recovery_wrapped_dek_tag[APPLIANCE_STATE_CRYPTO_TAG_LENGTH];
    uint8_t dek_fingerprint[APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
    uint8_t recovery_fingerprint[
        APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
    uint8_t record_mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
};

struct appliance_key_custody {
    uint32_t version;
    enum appliance_key_custody_state state;
    enum appliance_key_custody_binding binding;
    uint8_t dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t volume_id[APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH];
    uint8_t dek_fingerprint[APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
};

enum appliance_key_custody_status appliance_key_custody_create(
    struct appliance_key_custody *custody,
    appliance_state_crypto_random_fn random,
    void *random_context,
    const struct appliance_key_custody_material *passphrase,
    struct appliance_key_custody_record *record,
    char recovery_text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U]);
enum appliance_key_custody_status appliance_key_custody_open(
    struct appliance_key_custody *custody,
    const struct appliance_key_custody_record *record,
    const struct appliance_key_custody_material *material);
enum appliance_key_custody_status appliance_key_custody_record_encode(
    const struct appliance_key_custody_record *record,
    uint8_t bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH]);
enum appliance_key_custody_status appliance_key_custody_record_decode(
    const uint8_t bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH],
    struct appliance_key_custody_record *record);
enum appliance_key_custody_status appliance_key_custody_recovery_decode(
    const char *text,
    size_t text_length,
    uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH]);
enum appliance_key_custody_status appliance_key_custody_escrow_line(
    const struct appliance_key_custody_record *record,
    const char *recovery_text,
    char *output,
    size_t output_capacity,
    size_t *output_length);
bool appliance_key_custody_tpm_available(void);
bool appliance_key_custody_unsealed(
    const struct appliance_key_custody *custody);
void appliance_key_custody_clear(struct appliance_key_custody *custody);
const char *appliance_key_custody_status_string(
    enum appliance_key_custody_status status);
const char *appliance_key_custody_binding_string(
    enum appliance_key_custody_binding binding);
const char *appliance_key_custody_state_string(
    enum appliance_key_custody_state state);

#endif
