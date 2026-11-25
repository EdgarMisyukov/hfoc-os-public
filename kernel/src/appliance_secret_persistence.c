#include "appliance_secret_persistence.h"

#include "appliance_admin_credential.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct appliance_secret_persistence_record
    appliance_secret_persistence_records[] = {
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW,
        "certificate.https",
        "v1:certificate.https=",
        1U,
        1U,
        1U,
        "TLS HTTPS identity certificate chain",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER,
        "secret.admin.credential",
        "v1:secret.admin.credential=",
        1U,
        1U,
        1U,
        "administrative credential PBKDF2-HMAC-SHA256 verifier with salt and iteration count",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY,
        "secret.https.private_key.digest",
        "v1:secret.https.private_key.digest=",
        1U,
        1U,
        1U,
        "HTTPS private-key digest and rotation metadata",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY,
        "secret.https.private_key.wrapped",
        "v1:secret.https.private_key.wrapped=",
        1U,
        1U,
        1U,
        "opaque wrapped HTTPS private-key blob",
    },
};

static bool appliance_secret_persistence_bytes_equal(
    const char *left,
    const char *right,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if ((unsigned char)left[index] !=
            (unsigned char)right[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_secret_persistence_starts_with(
    const char *key,
    size_t key_length,
    const char *text,
    size_t text_length)
{
    return key_length >= text_length &&
           appliance_secret_persistence_bytes_equal(
               key, text, text_length);
}

static size_t appliance_secret_persistence_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_secret_persistence_digit_valid(
    unsigned char value)
{
    return value >= (unsigned char)'0' &&
           value <= (unsigned char)'9';
}

static bool appliance_secret_persistence_lower_hex_valid(
    unsigned char value)
{
    return (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' &&
            value <= (unsigned char)'f');
}

static bool appliance_secret_persistence_kind_valid(
    enum appliance_secret_persistence_record_kind kind)
{
    return kind >=
               APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY &&
           kind <=
               APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED;
}

static bool appliance_secret_persistence_bytes_all_zero(
    const uint8_t *bytes,
    size_t length)
{
    size_t index;
    uint8_t nonzero = 0U;

    for (index = 0U; index < length; ++index) {
        nonzero |= bytes[index];
    }
    return nonzero == 0U;
}

static bool appliance_secret_persistence_hex_digest_valid(
    const uint8_t *value,
    size_t value_length)
{
    size_t index;

    if (value_length !=
        APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        if (!appliance_secret_persistence_lower_hex_valid(
                (unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-506 plausibility check for the structured administrator credential
 * value.  The exact grammar (iteration bounds, hex fields, login policy)
 * belongs to `appliance_admin_credential`; this catalogue keeps only the
 * shape every valid value shares - the KDF label, printable ASCII and the
 * documented maximum length - so a truncated or binary blob is refused at
 * commit time and the exact parse still happens in one owner.  The constant
 * comes from the credential header, so the label cannot drift.
 */
static bool appliance_secret_persistence_credential_value_plausible(
    const uint8_t *value,
    size_t value_length)
{
    static const char label[] = APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL "$";
    const size_t label_length = sizeof(label) - 1U;
    size_t index;

    if (value_length <= label_length ||
        value_length > APPLIANCE_ADMIN_CREDENTIAL_RECORD_VALUE_MAX) {
        return false;
    }
    if (!appliance_secret_persistence_bytes_equal(
            (const char *)value, label, label_length)) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        if (value[index] < 0x20U || value[index] > 0x7eU) {
            return false;
        }
    }
    return true;
}

size_t appliance_secret_persistence_count(void)
{
    return sizeof(appliance_secret_persistence_records) /
        sizeof(appliance_secret_persistence_records[0]);
}

enum appliance_secret_persistence_status
appliance_secret_persistence_catalogue_at(
    size_t index,
    const struct appliance_secret_persistence_record **record_out)
{
    if (record_out == NULL) {
        return APPLIANCE_SECRET_PERSISTENCE_NULL;
    }
    if (index >= appliance_secret_persistence_count()) {
        *record_out = NULL;
        return APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE;
    }
    *record_out = &appliance_secret_persistence_records[index];
    return APPLIANCE_SECRET_PERSISTENCE_OK;
}

enum appliance_secret_persistence_status
appliance_secret_persistence_describe(
    enum appliance_secret_persistence_record_kind kind,
    const struct appliance_secret_persistence_record **record_out)
{
    size_t index;

    if (record_out == NULL) {
        return APPLIANCE_SECRET_PERSISTENCE_NULL;
    }
    for (index = 0U;
         index < appliance_secret_persistence_count();
         ++index) {
        if (appliance_secret_persistence_records[index].kind == kind) {
            *record_out = &appliance_secret_persistence_records[index];
            return APPLIANCE_SECRET_PERSISTENCE_OK;
        }
    }
    *record_out = NULL;
    return APPLIANCE_SECRET_PERSISTENCE_BAD_ID;
}

enum appliance_secret_persistence_status
appliance_secret_persistence_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_secret_persistence_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out)
{
    size_t index;
    size_t position;

    if (key == NULL || kind_out == NULL ||
        namespace_out == NULL || version_out == NULL) {
        return APPLIANCE_SECRET_PERSISTENCE_NULL;
    }
    *kind_out = APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
    *namespace_out = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    *version_out = 0U;

    if (key_length == 0U ||
        key_length > APPLIANCE_SECRET_PERSISTENCE_RECORD_MAX) {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
    }
    if (key[key_length - 1U] == (unsigned char)'\n') {
        --key_length;
    }
    if (key_length < 3U ||
        key[0] != (unsigned char)'v' ||
        key[2] != (unsigned char)':') {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
    }
    if (!appliance_secret_persistence_digit_valid(
            (unsigned char)key[1])) {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
    }
    if (key[1] != (unsigned char)'1') {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION;
    }
    for (position = 0U; position < key_length; ++position) {
        if (key[position] == (unsigned char)'\n') {
            return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
        }
    }

    for (index = 0U;
         index < appliance_secret_persistence_count();
         ++index) {
        const struct appliance_secret_persistence_record *record =
            &appliance_secret_persistence_records[index];
        const size_t prefix_length =
            appliance_secret_persistence_string_length(record->prefix);

        if (!appliance_secret_persistence_starts_with(
                key, key_length, record->prefix, prefix_length)) {
            continue;
        }
        if (key_length == prefix_length) {
            return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
        }
        *kind_out = record->kind;
        *namespace_out = record->namespace_id;
        *version_out = record->version;
        return APPLIANCE_SECRET_PERSISTENCE_OK;
    }

    for (index = 0U;
         index < appliance_secret_persistence_count();
         ++index) {
        const struct appliance_secret_persistence_record *record =
            &appliance_secret_persistence_records[index];
        const size_t prefix_length =
            appliance_secret_persistence_string_length(record->prefix);

        if (appliance_secret_persistence_starts_with(
                key, key_length, record->prefix, key_length) &&
            key_length < prefix_length) {
            return APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD;
        }
    }
    return APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY;
}

enum appliance_secret_persistence_status
appliance_secret_persistence_validate_value(
    enum appliance_secret_persistence_record_kind kind,
    const uint8_t *value,
    size_t value_length)
{
    if (value == NULL) {
        return APPLIANCE_SECRET_PERSISTENCE_NULL;
    }
    if (!appliance_secret_persistence_kind_valid(kind)) {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_ID;
    }
    if (value_length == 0U ||
        value_length > APPLIANCE_SECRET_PERSISTENCE_RECORD_MAX ||
        appliance_secret_persistence_bytes_all_zero(
            value, value_length)) {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE;
    }
    if (kind == APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL ||
        kind ==
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST) {
        if (appliance_secret_persistence_hex_digest_valid(
                value, value_length)) {
            return APPLIANCE_SECRET_PERSISTENCE_OK;
        }
        /*
         * ITEM-506: the administrator record may instead be the structured
         * PBKDF2 value.  Nothing else changed shape, so the digest record
         * still requires its exact hex.
         */
        if (kind ==
                APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL &&
            appliance_secret_persistence_credential_value_plausible(
                value, value_length)) {
            return APPLIANCE_SECRET_PERSISTENCE_OK;
        }
        return APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE;
    }
    return APPLIANCE_SECRET_PERSISTENCE_OK;
}

enum appliance_secret_persistence_status
appliance_secret_persistence_https_readiness(
    const struct appliance_secret_persistence_https_state *state,
    uint32_t *dependency_mask_out,
    bool *ready_out)
{
    bool ready;

    if (state == NULL || dependency_mask_out == NULL ||
        ready_out == NULL) {
        return APPLIANCE_SECRET_PERSISTENCE_NULL;
    }
    if (state->version != APPLIANCE_SECRET_PERSISTENCE_VERSION) {
        return APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION;
    }

    ready = state->certificate_published &&
            state->private_key_wrapped_published;
    *dependency_mask_out =
        ready ? APPLIANCE_SECRET_PERSISTENCE_HTTPS_DEPENDENCY_READY : 0U;
    *ready_out = ready;
    return APPLIANCE_SECRET_PERSISTENCE_OK;
}

const char *appliance_secret_persistence_record_kind_string(
    enum appliance_secret_persistence_record_kind kind)
{
    switch (kind) {
    case APPLIANCE_SECRET_PERSISTENCE_KIND_NONE:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_NONE";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED";
    default:
        return NULL;
    }
}

const char *appliance_secret_persistence_handling_string(
    enum appliance_secret_persistence_handling handling)
{
    switch (handling) {
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_FINGERPRINT_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_FINGERPRINT_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER";
    default:
        return NULL;
    }
}

const char *appliance_secret_persistence_status_string(
    enum appliance_secret_persistence_status status)
{
    switch (status) {
    case APPLIANCE_SECRET_PERSISTENCE_OK:
        return "APPLIANCE_SECRET_PERSISTENCE_OK";
    case APPLIANCE_SECRET_PERSISTENCE_NULL:
        return "APPLIANCE_SECRET_PERSISTENCE_NULL";
    case APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE:
        return "APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION";
    case APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY:
        return "APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_ID:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_ID";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_STATE:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_STATE";
    default:
        return NULL;
    }
}
