#include "appliance_key_custody.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_KEY_CUSTODY_MAGIC "HFOCKEY1"

#define APPLIANCE_KEY_CUSTODY_OFFSET_MAGIC 0U
#define APPLIANCE_KEY_CUSTODY_OFFSET_VERSION 8U
#define APPLIANCE_KEY_CUSTODY_OFFSET_BINDING 12U
#define APPLIANCE_KEY_CUSTODY_OFFSET_KDF_ID 16U
#define APPLIANCE_KEY_CUSTODY_OFFSET_ITERATIONS 20U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_ITERATIONS 24U
#define APPLIANCE_KEY_CUSTODY_OFFSET_SALT 32U
#define APPLIANCE_KEY_CUSTODY_OFFSET_VOLUME_ID 48U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RESERVED_1 56U
#define APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK 64U
#define APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK_TAG 96U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK 112U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK_TAG 144U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_SALT 160U
#define APPLIANCE_KEY_CUSTODY_OFFSET_DEK_FINGERPRINT 176U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_FINGERPRINT 208U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RECORD_MAC 240U
#define APPLIANCE_KEY_CUSTODY_OFFSET_RESERVED_2 272U
#define APPLIANCE_KEY_CUSTODY_CANONICAL_LENGTH \
    APPLIANCE_KEY_CUSTODY_OFFSET_RECORD_MAC
#define APPLIANCE_KEY_CUSTODY_RESERVED_0_OFFSET 28U
#define APPLIANCE_KEY_CUSTODY_RESERVED_0_LENGTH 4U
#define APPLIANCE_KEY_CUSTODY_RESERVED_1_LENGTH 8U

static const uint8_t appliance_key_custody_wrap_enc_label[] =
    "HFOC-CUSTODY-WRAP-ENC";
static const uint8_t appliance_key_custody_wrap_mac_label[] =
    "HFOC-CUSTODY-WRAP-MAC";
static const uint8_t appliance_key_custody_wrap_tag_label[] =
    "HFOC-CUSTODY-WRAP-TAG";
static const uint8_t appliance_key_custody_dek_label[] =
    "HFOC-CUSTODY-DEK-FINGERPRINT";
static const uint8_t appliance_key_custody_recovery_label[] =
    "HFOC-CUSTODY-RECOVERY-FINGERPRINT";

static void key_custody_store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t key_custody_load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static bool key_custody_range_zero(
    const uint8_t *bytes,
    size_t offset,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[offset + index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool key_custody_binding_valid(
    enum appliance_key_custody_binding binding)
{
    return binding >= APPLIANCE_KEY_CUSTODY_BINDING_NONE &&
           binding <= APPLIANCE_KEY_CUSTODY_BINDING_TPM;
}

static bool key_custody_iterations_valid(uint32_t iterations)
{
    return iterations >= APPLIANCE_STATE_CRYPTO_PBKDF2_MIN_ITERATIONS &&
           iterations <= APPLIANCE_STATE_CRYPTO_PBKDF2_MAX_ITERATIONS;
}

static void key_custody_canonical(
    const struct appliance_key_custody_record *record,
    uint8_t bytes[APPLIANCE_KEY_CUSTODY_CANONICAL_LENGTH])
{
    memset(bytes, 0, APPLIANCE_KEY_CUSTODY_CANONICAL_LENGTH);
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_MAGIC,
           APPLIANCE_KEY_CUSTODY_MAGIC,
           APPLIANCE_KEY_CUSTODY_MAGIC_LENGTH);
    key_custody_store_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_VERSION, record->version);
    key_custody_store_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_BINDING,
        (uint32_t)record->binding);
    key_custody_store_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_KDF_ID, record->kdf_id);
    key_custody_store_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_ITERATIONS,
        record->kdf_iterations);
    key_custody_store_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_ITERATIONS,
        record->recovery_kdf_iterations);
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_SALT, record->salt,
           sizeof(record->salt));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_VOLUME_ID,
           record->volume_id, sizeof(record->volume_id));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK,
           record->wrapped_dek, sizeof(record->wrapped_dek));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK_TAG,
           record->wrapped_dek_tag, sizeof(record->wrapped_dek_tag));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK,
           record->recovery_wrapped_dek,
           sizeof(record->recovery_wrapped_dek));
    memcpy(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK_TAG,
        record->recovery_wrapped_dek_tag,
        sizeof(record->recovery_wrapped_dek_tag));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_SALT,
           record->recovery_salt, sizeof(record->recovery_salt));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_DEK_FINGERPRINT,
           record->dek_fingerprint, sizeof(record->dek_fingerprint));
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_FINGERPRINT,
           record->recovery_fingerprint,
           sizeof(record->recovery_fingerprint));
}

static enum appliance_key_custody_status key_custody_derive_wrap_keys(
    const uint8_t *secret,
    size_t secret_length,
    const uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH],
    uint32_t iterations,
    uint8_t wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    uint8_t wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH])
{
    uint8_t kek[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];

    if (appliance_state_crypto_pbkdf2_sha256(
            secret, secret_length, salt, iterations, kek,
            sizeof(kek)) != APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    if (appliance_state_crypto_hmac_sha256(
            kek, sizeof(kek), appliance_key_custody_wrap_enc_label,
            sizeof(appliance_key_custody_wrap_enc_label) - 1U,
            wrap_key) != APPLIANCE_STATE_CRYPTO_OK ||
        appliance_state_crypto_hmac_sha256(
            kek, sizeof(kek), appliance_key_custody_wrap_mac_label,
            sizeof(appliance_key_custody_wrap_mac_label) - 1U,
            wrap_mac_key) != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_state_crypto_zeroize(kek, sizeof(kek));
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    appliance_state_crypto_zeroize(kek, sizeof(kek));
    return APPLIANCE_KEY_CUSTODY_OK;
}

static enum appliance_key_custody_status key_custody_wrap(
    const uint8_t wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH],
    const uint8_t volume_id[APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH],
    const uint8_t dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH],
    uint8_t wrapped[APPLIANCE_KEY_CUSTODY_DEK_LENGTH],
    uint8_t tag[APPLIANCE_STATE_CRYPTO_TAG_LENGTH])
{
    uint8_t mac_input[sizeof(appliance_key_custody_wrap_tag_label) - 1U +
                       APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH +
                       APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    size_t offset = 0U;

    if (appliance_state_crypto_ctr(
            wrap_key, nonce, dek, wrapped,
            APPLIANCE_KEY_CUSTODY_DEK_LENGTH) !=
        APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    memcpy(mac_input + offset, appliance_key_custody_wrap_tag_label,
           sizeof(appliance_key_custody_wrap_tag_label) - 1U);
    offset += sizeof(appliance_key_custody_wrap_tag_label) - 1U;
    memcpy(mac_input + offset, volume_id,
           APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH);
    offset += APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH;
    memcpy(mac_input + offset, wrapped,
           APPLIANCE_KEY_CUSTODY_DEK_LENGTH);
    offset += APPLIANCE_KEY_CUSTODY_DEK_LENGTH;
    if (appliance_state_crypto_hmac_sha256(
            wrap_mac_key, APPLIANCE_STATE_CRYPTO_KEY_LENGTH, mac_input,
            offset, mac) != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_state_crypto_zeroize(mac_input, sizeof(mac_input));
        appliance_state_crypto_zeroize(mac, sizeof(mac));
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    memcpy(tag, mac, APPLIANCE_STATE_CRYPTO_TAG_LENGTH);
    appliance_state_crypto_zeroize(mac_input, sizeof(mac_input));
    appliance_state_crypto_zeroize(mac, sizeof(mac));
    return APPLIANCE_KEY_CUSTODY_OK;
}

static enum appliance_key_custody_status key_custody_unwrap(
    const uint8_t wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH],
    const uint8_t nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH],
    const uint8_t volume_id[APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH],
    const uint8_t wrapped[APPLIANCE_KEY_CUSTODY_DEK_LENGTH],
    const uint8_t tag[APPLIANCE_STATE_CRYPTO_TAG_LENGTH],
    uint8_t dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH])
{
    uint8_t mac_input[sizeof(appliance_key_custody_wrap_tag_label) - 1U +
                       APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH +
                       APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    size_t offset = 0U;

    memcpy(mac_input + offset, appliance_key_custody_wrap_tag_label,
           sizeof(appliance_key_custody_wrap_tag_label) - 1U);
    offset += sizeof(appliance_key_custody_wrap_tag_label) - 1U;
    memcpy(mac_input + offset, volume_id,
           APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH);
    offset += APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH;
    memcpy(mac_input + offset, wrapped,
           APPLIANCE_KEY_CUSTODY_DEK_LENGTH);
    offset += APPLIANCE_KEY_CUSTODY_DEK_LENGTH;
    if (appliance_state_crypto_hmac_sha256(
            wrap_mac_key, APPLIANCE_STATE_CRYPTO_KEY_LENGTH, mac_input,
            offset, mac) != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_state_crypto_zeroize(mac_input, sizeof(mac_input));
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    appliance_state_crypto_zeroize(mac_input, sizeof(mac_input));
    if (!appliance_state_crypto_equal(
            mac, tag, APPLIANCE_STATE_CRYPTO_TAG_LENGTH)) {
        appliance_state_crypto_zeroize(mac, sizeof(mac));
        return APPLIANCE_KEY_CUSTODY_WRONG_KEY;
    }
    appliance_state_crypto_zeroize(mac, sizeof(mac));
    if (appliance_state_crypto_ctr(
            wrap_key, nonce, wrapped, dek,
            APPLIANCE_KEY_CUSTODY_DEK_LENGTH) !=
        APPLIANCE_STATE_CRYPTO_OK) {
        return APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
    }
    return APPLIANCE_KEY_CUSTODY_OK;
}

static void key_custody_fingerprint(
    const uint8_t *key,
    size_t key_length,
    const uint8_t *label,
    size_t label_length,
    uint8_t fingerprint[APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH])
{
    (void)appliance_state_crypto_hmac_sha256(
        key, key_length, label, label_length, fingerprint);
}

static void key_custody_recovery_text(
    const uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH],
    char text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U])
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index;

    for (index = 0U; index < APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH;
         ++index) {
        text[index * 2U] = digits[(recovery_key[index] >> 4U) & 0x0FU];
        text[index * 2U + 1U] = digits[recovery_key[index] & 0x0FU];
    }
    text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH] = '\0';
}

static bool key_custody_hex_digit(char character, uint8_t *value_out)
{
    if (character >= '0' && character <= '9') {
        *value_out = (uint8_t)(character - '0');
        return true;
    }
    if (character >= 'A' && character <= 'F') {
        *value_out = (uint8_t)(character - 'A' + 10);
        return true;
    }
    if (character >= 'a' && character <= 'f') {
        *value_out = (uint8_t)(character - 'a' + 10);
        return true;
    }
    return false;
}

enum appliance_key_custody_status appliance_key_custody_create(
    struct appliance_key_custody *custody,
    appliance_state_crypto_random_fn random,
    void *random_context,
    const struct appliance_key_custody_material *passphrase,
    struct appliance_key_custody_record *record,
    char recovery_text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U])
{
    struct appliance_key_custody_record draft;
    uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH];
    uint8_t wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t recovery_wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t recovery_wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t canonical[APPLIANCE_KEY_CUSTODY_CANONICAL_LENGTH];
    enum appliance_key_custody_status status;

    if (custody == NULL || random == NULL || passphrase == NULL ||
        record == NULL || recovery_text == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    if (passphrase->binding == APPLIANCE_KEY_CUSTODY_BINDING_TPM) {
        return APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE;
    }
    if (passphrase->binding !=
            APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE ||
        passphrase->secret == NULL) {
        return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
    }
    if (passphrase->secret_length == 0U ||
        passphrase->secret_length > APPLIANCE_STATE_CRYPTO_PASSPHRASE_MAX) {
        return APPLIANCE_KEY_CUSTODY_BAD_PASSPHRASE;
    }

    memset(&draft, 0, sizeof(draft));
    memset(recovery_key, 0, sizeof(recovery_key));
    memset(wrap_key, 0, sizeof(wrap_key));
    memset(wrap_mac_key, 0, sizeof(wrap_mac_key));
    memset(recovery_wrap_key, 0, sizeof(recovery_wrap_key));
    memset(recovery_wrap_mac_key, 0, sizeof(recovery_wrap_mac_key));
    memset(canonical, 0, sizeof(canonical));
    memset(custody, 0, sizeof(*custody));
    memset(record, 0, sizeof(*record));
    recovery_text[0] = '\0';

    if (appliance_state_crypto_random_fill(
            random, random_context, custody->dek,
            sizeof(custody->dek)) != APPLIANCE_STATE_CRYPTO_OK ||
        appliance_state_crypto_random_fill(
            random, random_context, recovery_key,
            sizeof(recovery_key)) != APPLIANCE_STATE_CRYPTO_OK ||
        appliance_state_crypto_random_fill(
            random, random_context, draft.salt,
            sizeof(draft.salt)) != APPLIANCE_STATE_CRYPTO_OK ||
        appliance_state_crypto_random_fill(
            random, random_context, draft.recovery_salt,
            sizeof(draft.recovery_salt)) != APPLIANCE_STATE_CRYPTO_OK ||
        appliance_state_crypto_random_fill(
            random, random_context, custody->volume_id,
            sizeof(custody->volume_id)) != APPLIANCE_STATE_CRYPTO_OK) {
        appliance_key_custody_clear(custody);
        return APPLIANCE_KEY_CUSTODY_RANDOM_FAILED;
    }

    draft.version = APPLIANCE_KEY_CUSTODY_VERSION;
    draft.binding = APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE;
    draft.kdf_id = APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256;
    draft.kdf_iterations =
        APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS;
    draft.recovery_kdf_iterations =
        APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS;
    memcpy(draft.volume_id, custody->volume_id,
           sizeof(draft.volume_id));

    status = key_custody_derive_wrap_keys(
        passphrase->secret, passphrase->secret_length, draft.salt,
        draft.kdf_iterations, wrap_key, wrap_mac_key);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }
    status = key_custody_derive_wrap_keys(
        recovery_key, sizeof(recovery_key), draft.recovery_salt,
        draft.recovery_kdf_iterations, recovery_wrap_key,
        recovery_wrap_mac_key);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }
    status = key_custody_wrap(
        wrap_key, wrap_mac_key, draft.salt, draft.volume_id, custody->dek,
        draft.wrapped_dek, draft.wrapped_dek_tag);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }
    status = key_custody_wrap(
        recovery_wrap_key, recovery_wrap_mac_key, draft.recovery_salt,
        draft.volume_id, custody->dek, draft.recovery_wrapped_dek,
        draft.recovery_wrapped_dek_tag);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }

    key_custody_fingerprint(
        custody->dek, sizeof(custody->dek),
        appliance_key_custody_dek_label,
        sizeof(appliance_key_custody_dek_label) - 1U,
        draft.dek_fingerprint);
    key_custody_fingerprint(
        recovery_key, sizeof(recovery_key),
        appliance_key_custody_recovery_label,
        sizeof(appliance_key_custody_recovery_label) - 1U,
        draft.recovery_fingerprint);
    memcpy(custody->dek_fingerprint, draft.dek_fingerprint,
           sizeof(custody->dek_fingerprint));
    key_custody_canonical(&draft, canonical);
    if (appliance_state_crypto_hmac_sha256(
            custody->dek, sizeof(custody->dek), canonical,
            sizeof(canonical), draft.record_mac) !=
        APPLIANCE_STATE_CRYPTO_OK) {
        status = APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
        goto fail;
    }

    custody->version = APPLIANCE_KEY_CUSTODY_VERSION;
    custody->state = APPLIANCE_KEY_CUSTODY_STATE_UNSEALED;
    custody->binding = APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE;
    key_custody_recovery_text(recovery_key, recovery_text);
    *record = draft;

    appliance_state_crypto_zeroize(recovery_key, sizeof(recovery_key));
    appliance_state_crypto_zeroize(wrap_key, sizeof(wrap_key));
    appliance_state_crypto_zeroize(wrap_mac_key, sizeof(wrap_mac_key));
    appliance_state_crypto_zeroize(
        recovery_wrap_key, sizeof(recovery_wrap_key));
    appliance_state_crypto_zeroize(
        recovery_wrap_mac_key, sizeof(recovery_wrap_mac_key));
    appliance_state_crypto_zeroize(canonical, sizeof(canonical));
    return APPLIANCE_KEY_CUSTODY_OK;

fail:
    appliance_state_crypto_zeroize(recovery_key, sizeof(recovery_key));
    appliance_state_crypto_zeroize(wrap_key, sizeof(wrap_key));
    appliance_state_crypto_zeroize(wrap_mac_key, sizeof(wrap_mac_key));
    appliance_state_crypto_zeroize(
        recovery_wrap_key, sizeof(recovery_wrap_key));
    appliance_state_crypto_zeroize(
        recovery_wrap_mac_key, sizeof(recovery_wrap_mac_key));
    appliance_state_crypto_zeroize(canonical, sizeof(canonical));
    appliance_key_custody_clear(custody);
    memset(record, 0, sizeof(*record));
    recovery_text[0] = '\0';
    return status;
}

enum appliance_key_custody_status appliance_key_custody_open(
    struct appliance_key_custody *custody,
    const struct appliance_key_custody_record *record,
    const struct appliance_key_custody_material *material)
{
    uint8_t wrap_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t wrap_mac_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH];
    uint8_t dek[APPLIANCE_KEY_CUSTODY_DEK_LENGTH];
    uint8_t fingerprint[APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH];
    uint8_t canonical[APPLIANCE_KEY_CUSTODY_CANONICAL_LENGTH];
    uint8_t record_mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    const uint8_t *salt;
    const uint8_t *wrapped;
    const uint8_t *tag;
    const uint8_t *secret;
    size_t secret_length;
    uint32_t iterations;
    enum appliance_key_custody_status status;

    if (custody == NULL || record == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    memset(custody, 0, sizeof(*custody));
    if (record->version != APPLIANCE_KEY_CUSTODY_VERSION) {
        return APPLIANCE_KEY_CUSTODY_BAD_VERSION;
    }
    if (!key_custody_binding_valid(record->binding)) {
        return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
    }
    if (record->kdf_id != APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256) {
        return APPLIANCE_KEY_CUSTODY_BAD_KDF;
    }
    if (!key_custody_iterations_valid(record->kdf_iterations) ||
        !key_custody_iterations_valid(record->recovery_kdf_iterations)) {
        return APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS;
    }
    if (record->binding == APPLIANCE_KEY_CUSTODY_BINDING_TPM) {
        return APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE;
    }
    if (material == NULL) {
        return APPLIANCE_KEY_CUSTODY_KEY_REQUIRED;
    }
    if (material->binding == APPLIANCE_KEY_CUSTODY_BINDING_TPM) {
        return APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE;
    }
    if (material->secret == NULL) {
        return APPLIANCE_KEY_CUSTODY_KEY_REQUIRED;
    }
    if (material->binding ==
        APPLIANCE_KEY_CUSTODY_BINDING_RECOVERY_KEY) {
        if (material->secret_length !=
            APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH) {
            return APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY;
        }
        secret = material->secret;
        secret_length = material->secret_length;
        salt = record->recovery_salt;
        wrapped = record->recovery_wrapped_dek;
        tag = record->recovery_wrapped_dek_tag;
        iterations = record->recovery_kdf_iterations;
    } else if (material->binding ==
               APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE) {
        if (record->binding !=
            APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE) {
            return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
        }
        if (material->secret_length == 0U ||
            material->secret_length >
                APPLIANCE_STATE_CRYPTO_PASSPHRASE_MAX) {
            return APPLIANCE_KEY_CUSTODY_BAD_PASSPHRASE;
        }
        secret = material->secret;
        secret_length = material->secret_length;
        salt = record->salt;
        wrapped = record->wrapped_dek;
        tag = record->wrapped_dek_tag;
        iterations = record->kdf_iterations;
    } else {
        return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
    }

    memset(wrap_key, 0, sizeof(wrap_key));
    memset(wrap_mac_key, 0, sizeof(wrap_mac_key));
    memset(dek, 0, sizeof(dek));
    memset(fingerprint, 0, sizeof(fingerprint));
    memset(canonical, 0, sizeof(canonical));
    memset(record_mac, 0, sizeof(record_mac));

    status = key_custody_derive_wrap_keys(
        secret, secret_length, salt, iterations, wrap_key, wrap_mac_key);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }
    status = key_custody_unwrap(
        wrap_key, wrap_mac_key, salt, record->volume_id, wrapped, tag,
        dek);
    if (status != APPLIANCE_KEY_CUSTODY_OK) {
        goto fail;
    }

    key_custody_fingerprint(
        dek, sizeof(dek), appliance_key_custody_dek_label,
        sizeof(appliance_key_custody_dek_label) - 1U, fingerprint);
    if (!appliance_state_crypto_equal(
            fingerprint, record->dek_fingerprint, sizeof(fingerprint))) {
        status = APPLIANCE_KEY_CUSTODY_AUTH_FAILED;
        goto fail;
    }
    key_custody_canonical(record, canonical);
    if (appliance_state_crypto_hmac_sha256(
            dek, sizeof(dek), canonical, sizeof(canonical),
            record_mac) != APPLIANCE_STATE_CRYPTO_OK) {
        status = APPLIANCE_KEY_CUSTODY_UNSUPPORTED;
        goto fail;
    }
    if (!appliance_state_crypto_equal(
            record_mac, record->record_mac, sizeof(record_mac))) {
        status = APPLIANCE_KEY_CUSTODY_AUTH_FAILED;
        goto fail;
    }

    custody->version = APPLIANCE_KEY_CUSTODY_VERSION;
    custody->state = APPLIANCE_KEY_CUSTODY_STATE_UNSEALED;
    custody->binding = material->binding;
    memcpy(custody->dek, dek, sizeof(custody->dek));
    memcpy(custody->volume_id, record->volume_id,
           sizeof(custody->volume_id));
    memcpy(custody->dek_fingerprint, record->dek_fingerprint,
           sizeof(custody->dek_fingerprint));

    appliance_state_crypto_zeroize(wrap_key, sizeof(wrap_key));
    appliance_state_crypto_zeroize(wrap_mac_key, sizeof(wrap_mac_key));
    appliance_state_crypto_zeroize(dek, sizeof(dek));
    appliance_state_crypto_zeroize(fingerprint, sizeof(fingerprint));
    appliance_state_crypto_zeroize(canonical, sizeof(canonical));
    appliance_state_crypto_zeroize(record_mac, sizeof(record_mac));
    return APPLIANCE_KEY_CUSTODY_OK;

fail:
    appliance_state_crypto_zeroize(wrap_key, sizeof(wrap_key));
    appliance_state_crypto_zeroize(wrap_mac_key, sizeof(wrap_mac_key));
    appliance_state_crypto_zeroize(dek, sizeof(dek));
    appliance_state_crypto_zeroize(fingerprint, sizeof(fingerprint));
    appliance_state_crypto_zeroize(canonical, sizeof(canonical));
    appliance_state_crypto_zeroize(record_mac, sizeof(record_mac));
    appliance_key_custody_clear(custody);
    return status;
}

enum appliance_key_custody_status appliance_key_custody_record_encode(
    const struct appliance_key_custody_record *record,
    uint8_t bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH])
{
    if (record == NULL || bytes == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    if (record->version != APPLIANCE_KEY_CUSTODY_VERSION) {
        return APPLIANCE_KEY_CUSTODY_BAD_VERSION;
    }
    if (!key_custody_binding_valid(record->binding)) {
        return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
    }
    if (record->kdf_id != APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256) {
        return APPLIANCE_KEY_CUSTODY_BAD_KDF;
    }
    if (!key_custody_iterations_valid(record->kdf_iterations) ||
        !key_custody_iterations_valid(record->recovery_kdf_iterations)) {
        return APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS;
    }

    memset(bytes, 0, APPLIANCE_KEY_CUSTODY_RECORD_LENGTH);
    key_custody_canonical(record, bytes);
    memcpy(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECORD_MAC,
           record->record_mac, sizeof(record->record_mac));
    return APPLIANCE_KEY_CUSTODY_OK;
}

enum appliance_key_custody_status appliance_key_custody_record_decode(
    const uint8_t bytes[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH],
    struct appliance_key_custody_record *record)
{
    uint32_t iterations;
    uint32_t recovery_iterations;

    if (bytes == NULL || record == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    if (memcmp(bytes + APPLIANCE_KEY_CUSTODY_OFFSET_MAGIC,
               APPLIANCE_KEY_CUSTODY_MAGIC,
               APPLIANCE_KEY_CUSTODY_MAGIC_LENGTH) != 0) {
        return APPLIANCE_KEY_CUSTODY_BAD_MAGIC;
    }
    if (!key_custody_range_zero(
            bytes, APPLIANCE_KEY_CUSTODY_RESERVED_0_OFFSET,
            APPLIANCE_KEY_CUSTODY_RESERVED_0_LENGTH) ||
        !key_custody_range_zero(
            bytes, APPLIANCE_KEY_CUSTODY_OFFSET_RESERVED_1,
            APPLIANCE_KEY_CUSTODY_RESERVED_1_LENGTH) ||
        !key_custody_range_zero(
            bytes, APPLIANCE_KEY_CUSTODY_OFFSET_RESERVED_2,
            APPLIANCE_KEY_CUSTODY_RECORD_LENGTH -
                APPLIANCE_KEY_CUSTODY_OFFSET_RESERVED_2)) {
        return APPLIANCE_KEY_CUSTODY_RESERVED_NONZERO;
    }

    memset(record, 0, sizeof(*record));
    record->version = key_custody_load_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_VERSION);
    if (record->version != APPLIANCE_KEY_CUSTODY_VERSION) {
        return APPLIANCE_KEY_CUSTODY_BAD_VERSION;
    }
    record->binding = (enum appliance_key_custody_binding)
        key_custody_load_le32(
            bytes + APPLIANCE_KEY_CUSTODY_OFFSET_BINDING);
    if (!key_custody_binding_valid(record->binding)) {
        return APPLIANCE_KEY_CUSTODY_BAD_BINDING;
    }
    record->kdf_id = key_custody_load_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_KDF_ID);
    if (record->kdf_id != APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256) {
        return APPLIANCE_KEY_CUSTODY_BAD_KDF;
    }
    iterations = key_custody_load_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_ITERATIONS);
    recovery_iterations = key_custody_load_le32(
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_ITERATIONS);
    if (!key_custody_iterations_valid(iterations) ||
        !key_custody_iterations_valid(recovery_iterations)) {
        return APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS;
    }
    record->kdf_iterations = iterations;
    record->recovery_kdf_iterations = recovery_iterations;
    memcpy(record->salt, bytes + APPLIANCE_KEY_CUSTODY_OFFSET_SALT,
           sizeof(record->salt));
    memcpy(record->volume_id,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_VOLUME_ID,
           sizeof(record->volume_id));
    memcpy(record->wrapped_dek,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK,
           sizeof(record->wrapped_dek));
    memcpy(record->wrapped_dek_tag,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_WRAPPED_DEK_TAG,
           sizeof(record->wrapped_dek_tag));
    memcpy(record->recovery_wrapped_dek,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK,
           sizeof(record->recovery_wrapped_dek));
    memcpy(
        record->recovery_wrapped_dek_tag,
        bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_WRAPPED_DEK_TAG,
        sizeof(record->recovery_wrapped_dek_tag));
    memcpy(record->recovery_salt,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_SALT,
           sizeof(record->recovery_salt));
    memcpy(record->dek_fingerprint,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_DEK_FINGERPRINT,
           sizeof(record->dek_fingerprint));
    memcpy(record->recovery_fingerprint,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECOVERY_FINGERPRINT,
           sizeof(record->recovery_fingerprint));
    memcpy(record->record_mac,
           bytes + APPLIANCE_KEY_CUSTODY_OFFSET_RECORD_MAC,
           sizeof(record->record_mac));
    return APPLIANCE_KEY_CUSTODY_OK;
}

enum appliance_key_custody_status appliance_key_custody_recovery_decode(
    const char *text,
    size_t text_length,
    uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH])
{
    size_t index;

    if (text == NULL || recovery_key == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    if (text_length != APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH) {
        return APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY;
    }
    for (index = 0U; index < APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH;
         ++index) {
        uint8_t high;
        uint8_t low;

        if (!key_custody_hex_digit(text[index * 2U], &high) ||
            !key_custody_hex_digit(text[index * 2U + 1U], &low)) {
            appliance_state_crypto_zeroize(
                recovery_key,
                APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH);
            return APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY;
        }
        recovery_key[index] = (uint8_t)((high << 4U) | low);
    }
    return APPLIANCE_KEY_CUSTODY_OK;
}

enum appliance_key_custody_status appliance_key_custody_escrow_line(
    const struct appliance_key_custody_record *record,
    const char *recovery_text,
    char *output,
    size_t output_capacity,
    size_t *output_length)
{
    static const char prefix[] = "HFOC-RECOVERY-KEY v1 key=";
    static const char fingerprint_prefix[] = " fp=";
    static const char digits[] = "0123456789ABCDEF";
    size_t offset = 0U;
    size_t index;

    if (record == NULL || recovery_text == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_KEY_CUSTODY_NULL;
    }
    if (output_capacity <
        sizeof(prefix) - 1U +
            APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH +
            sizeof(fingerprint_prefix) - 1U +
            APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH * 2U) {
        return APPLIANCE_KEY_CUSTODY_ESCROW_TOO_SMALL;
    }

    memcpy(output + offset, prefix, sizeof(prefix) - 1U);
    offset += sizeof(prefix) - 1U;
    memcpy(output + offset, recovery_text,
           APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH);
    offset += APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH;
    memcpy(output + offset, fingerprint_prefix,
           sizeof(fingerprint_prefix) - 1U);
    offset += sizeof(fingerprint_prefix) - 1U;
    for (index = 0U; index < APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH;
         ++index) {
        output[offset++] =
            digits[(record->recovery_fingerprint[index] >> 4U) & 0x0FU];
        output[offset++] =
            digits[record->recovery_fingerprint[index] & 0x0FU];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_KEY_CUSTODY_OK;
}

bool appliance_key_custody_tpm_available(void)
{
    return false;
}

bool appliance_key_custody_unsealed(
    const struct appliance_key_custody *custody)
{
    return custody != NULL &&
           custody->state == APPLIANCE_KEY_CUSTODY_STATE_UNSEALED;
}

void appliance_key_custody_clear(struct appliance_key_custody *custody)
{
    if (custody == NULL) {
        return;
    }
    appliance_state_crypto_zeroize(custody, sizeof(*custody));
}

const char *appliance_key_custody_status_string(
    enum appliance_key_custody_status status)
{
    switch (status) {
    case APPLIANCE_KEY_CUSTODY_OK:
        return "OK";
    case APPLIANCE_KEY_CUSTODY_NULL:
        return "NULL";
    case APPLIANCE_KEY_CUSTODY_BAD_MAGIC:
        return "BAD_MAGIC";
    case APPLIANCE_KEY_CUSTODY_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_KEY_CUSTODY_BAD_BINDING:
        return "BAD_BINDING";
    case APPLIANCE_KEY_CUSTODY_BAD_KDF:
        return "BAD_KDF";
    case APPLIANCE_KEY_CUSTODY_BAD_LENGTH:
        return "BAD_LENGTH";
    case APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS:
        return "BAD_ITERATIONS";
    case APPLIANCE_KEY_CUSTODY_RESERVED_NONZERO:
        return "RESERVED_NONZERO";
    case APPLIANCE_KEY_CUSTODY_RECORD_TOO_LARGE:
        return "RECORD_TOO_LARGE";
    case APPLIANCE_KEY_CUSTODY_AUTH_FAILED:
        return "AUTH_FAILED";
    case APPLIANCE_KEY_CUSTODY_WRONG_KEY:
        return "WRONG_KEY";
    case APPLIANCE_KEY_CUSTODY_KEY_REQUIRED:
        return "KEY_REQUIRED";
    case APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE:
        return "TPM_UNAVAILABLE";
    case APPLIANCE_KEY_CUSTODY_CORRUPT:
        return "CORRUPT";
    case APPLIANCE_KEY_CUSTODY_RANDOM_FAILED:
        return "RANDOM_FAILED";
    case APPLIANCE_KEY_CUSTODY_UNSUPPORTED:
        return "UNSUPPORTED";
    case APPLIANCE_KEY_CUSTODY_ESCROW_TOO_SMALL:
        return "ESCROW_TOO_SMALL";
    case APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY:
        return "BAD_RECOVERY_KEY";
    case APPLIANCE_KEY_CUSTODY_BAD_PASSPHRASE:
        return "BAD_PASSPHRASE";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_key_custody_binding_string(
    enum appliance_key_custody_binding binding)
{
    switch (binding) {
    case APPLIANCE_KEY_CUSTODY_BINDING_NONE:
        return "NONE";
    case APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE:
        return "PASSPHRASE";
    case APPLIANCE_KEY_CUSTODY_BINDING_RECOVERY_KEY:
        return "RECOVERY_KEY";
    case APPLIANCE_KEY_CUSTODY_BINDING_TPM:
        return "TPM";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_key_custody_state_string(
    enum appliance_key_custody_state state)
{
    switch (state) {
    case APPLIANCE_KEY_CUSTODY_STATE_EMPTY:
        return "EMPTY";
    case APPLIANCE_KEY_CUSTODY_STATE_SEALED:
        return "SEALED";
    case APPLIANCE_KEY_CUSTODY_STATE_UNSEALED:
        return "UNSEALED";
    default:
        return "UNKNOWN";
    }
}
