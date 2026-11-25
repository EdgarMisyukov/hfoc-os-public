#include "appliance_persistent_secret_config.h"

#include "appliance_secret_persistence.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char appliance_persistent_secret_config_prefix[] =
    APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX;

static void appliance_persistent_secret_config_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_persistent_secret_config_bytes_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static size_t appliance_persistent_secret_config_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_map_journal(
    enum config_journal_status status)
{
    switch (status) {
    case CONFIG_JOURNAL_OK:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    case CONFIG_JOURNAL_NULL:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    case CONFIG_JOURNAL_BAD_DEVICE:
    case CONFIG_JOURNAL_BAD_REGION:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE;
    case CONFIG_JOURNAL_IO_ERROR:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR;
    case CONFIG_JOURNAL_CORRUPT:
    case CONFIG_JOURNAL_BAD_RECORD:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT;
    case CONFIG_JOURNAL_FULL:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_FULL;
    case CONFIG_JOURNAL_BAD_STATE:
    case CONFIG_JOURNAL_TOO_SMALL:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE;
    case CONFIG_JOURNAL_EMPTY:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT;
    default:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT;
    }
}

static enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_map_data_region(
    enum data_region_status status)
{
    switch (status) {
    case DATA_REGION_OK:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    case DATA_REGION_NULL:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    case DATA_REGION_BAD_DEVICE:
    case DATA_REGION_UNSUPPORTED_BLOCK_SIZE:
    case DATA_REGION_BAD_MAGIC:
    case DATA_REGION_UNSUPPORTED_VERSION:
    case DATA_REGION_RESERVED_NONZERO:
    case DATA_REGION_INVALID_LAYOUT:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE;
    case DATA_REGION_IO_ERROR:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR;
    default:
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE;
    }
}

static char appliance_persistent_secret_config_hex_digit(uint8_t value)
{
    if (value < 10U) {
        return (char)('0' + (int)value);
    }
    return (char)('a' + (int)(value - 10U));
}

static void appliance_persistent_secret_config_encode_hex(
    const uint8_t *bytes,
    size_t length,
    char *output)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index * 2U] =
            appliance_persistent_secret_config_hex_digit(
                (uint8_t)(bytes[index] >> 4U));
        output[index * 2U + 1U] =
            appliance_persistent_secret_config_hex_digit(
                (uint8_t)(bytes[index] & 0x0FU));
    }
}

static int8_t appliance_persistent_secret_config_hex_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return (int8_t)(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return (int8_t)(digit - 'a' + 10);
    }
    if (digit >= 'A' && digit <= 'F') {
        return (int8_t)(digit - 'A' + 10);
    }
    return -1;
}

static bool appliance_persistent_secret_config_decode_hex(
    const char *hex,
    size_t hex_length,
    uint8_t *output,
    size_t output_length)
{
    size_t index;

    if (hex_length != output_length * 2U) {
        return false;
    }
    for (index = 0U; index < output_length; ++index) {
        const int8_t high =
            appliance_persistent_secret_config_hex_value(hex[index * 2U]);
        const int8_t low =
            appliance_persistent_secret_config_hex_value(
                hex[index * 2U + 1U]);

        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = (uint8_t)(((uint8_t)high << 4U) | (uint8_t)low);
    }
    return true;
}

static enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_parse_record(
    const unsigned char *record,
    size_t record_length,
    struct appliance_persistent_secret_config *config)
{
    const size_t prefix_length =
        appliance_persistent_secret_config_string_length(
            appliance_persistent_secret_config_prefix);
    const unsigned char *value;
    size_t value_length;
    enum appliance_admin_credential_status credential_status;

    if (record_length <= prefix_length ||
        record_length - prefix_length >
            APPLIANCE_PERSISTENT_SECRET_CONFIG_VALUE_MAX ||
        !appliance_persistent_secret_config_bytes_equal(
            record, appliance_persistent_secret_config_prefix,
            prefix_length)) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD;
    }
    value = record + prefix_length;
    value_length = record_length - prefix_length;
    if (appliance_secret_persistence_validate_value(
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
            value, value_length) != APPLIANCE_SECRET_PERSISTENCE_OK) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
    }
    if (appliance_admin_credential_record_is_legacy(
            (const char *)value, value_length)) {
        if (!appliance_persistent_secret_config_decode_hex(
                (const char *)value, value_length,
                config->admin_fingerprint, SHA256_DIGEST_SIZE)) {
            return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
        }
        appliance_persistent_secret_config_zero_bytes(
            config->admin_hex, sizeof(config->admin_hex));
        {
            size_t index;

            for (index = 0U; index < value_length; ++index) {
                config->admin_hex[index] = (char)value[index];
            }
        }
        config->admin_present = true;
        config->admin_kind =
            APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT;
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    }
    credential_status = appliance_admin_credential_record_decode(
        (const char *)value, value_length, &config->admin_credential);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
    }
    config->admin_credential_present = true;
    config->admin_kind =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL;
    return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_load(
    struct appliance_persistent_secret_config *config,
    struct block_device *device)
{
    struct data_region region;
    enum data_region_status data_region_status;
    enum config_journal_status journal_status;
    enum appliance_persistent_secret_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_SECRET_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL || device == NULL) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    }
    if (config->initialized) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE;
    }
    data_region_status = data_region_discover(device, &region);
    status = appliance_persistent_secret_config_map_data_region(
        data_region_status);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return status;
    }

    appliance_persistent_secret_config_zero_bytes(config, sizeof(*config));
    journal_status = config_journal_init(
        &config->journal, device, region.block_offset, region.block_count);
    status = appliance_persistent_secret_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return status;
    }

    appliance_persistent_secret_config_zero_bytes(record, sizeof(record));
    journal_status = config_journal_head(
        &config->journal, record, sizeof(record), &record_length);
    if (journal_status == CONFIG_JOURNAL_EMPTY) {
        config->source =
            APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_MISSING;
        config->initialized = true;
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    }
    status = appliance_persistent_secret_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return status;
    }
    status = appliance_persistent_secret_config_parse_record(
        record, record_length, config);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        config->source =
            APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_INVALID;
    } else {
        config->source =
            APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED;
    }
    config->initialized = true;
    return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_commit_admin(
    struct appliance_persistent_secret_config *config,
    const uint8_t fingerprint[SHA256_DIGEST_SIZE])
{
    enum config_journal_status journal_status;
    enum appliance_persistent_secret_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_SECRET_CONFIG_RECORD_MAX];
    const size_t prefix_length =
        appliance_persistent_secret_config_string_length(
            appliance_persistent_secret_config_prefix);
    const size_t value_length =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_HEX_LENGTH;
    const size_t record_length = prefix_length + value_length;
    enum appliance_secret_persistence_status persistence_status;
    enum appliance_secret_persistence_record_kind classified_kind =
        APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
    enum appliance_persistent_storage_namespace_id classified_namespace =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t classified_version = 0U;

    if (config == NULL || fingerprint == NULL) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE;
    }
    if (config->admin_present &&
        appliance_persistent_secret_config_bytes_equal(
            config->admin_fingerprint, fingerprint, SHA256_DIGEST_SIZE)) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    }
    if (record_length > sizeof(record)) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD;
    }

    appliance_persistent_secret_config_zero_bytes(record, sizeof(record));
    {
        size_t index;

        for (index = 0U; index < prefix_length; ++index) {
            record[index] =
                (unsigned char)appliance_persistent_secret_config_prefix[index];
        }
    }
    appliance_persistent_secret_config_encode_hex(
        fingerprint, SHA256_DIGEST_SIZE,
        (char *)(record + prefix_length));
    persistence_status = appliance_secret_persistence_classify_record_key(
        (const char *)record, record_length, &classified_kind,
        &classified_namespace, &classified_version);
    if (persistence_status != APPLIANCE_SECRET_PERSISTENCE_OK ||
        classified_kind !=
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL ||
        classified_namespace !=
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED;
    }
    if (appliance_secret_persistence_validate_value(
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
            record + prefix_length, value_length) !=
        APPLIANCE_SECRET_PERSISTENCE_OK) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_secret_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return status;
    }
    appliance_persistent_secret_config_zero_bytes(
        config->admin_fingerprint, sizeof(config->admin_fingerprint));
    {
        size_t index;

        for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
            config->admin_fingerprint[index] = fingerprint[index];
        }
    }
    appliance_persistent_secret_config_zero_bytes(
        config->admin_hex, sizeof(config->admin_hex));
    appliance_persistent_secret_config_encode_hex(
        fingerprint, SHA256_DIGEST_SIZE, config->admin_hex);
    /*
     * ITEM-506: the legacy fingerprint just became the head, so the store no
     * longer holds a structured credential.  Clearing the read-back state
     * keeps `admin_credential()` honest instead of returning the superseded
     * value.
     */
    appliance_persistent_secret_config_zero_bytes(
        &config->admin_credential, sizeof(config->admin_credential));
    config->admin_credential_present = false;
    config->admin_kind =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT;
    config->admin_present = true;
    config->source =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED;
    return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

static bool appliance_persistent_secret_config_credentials_equal(
    const struct appliance_admin_credential *left,
    const struct appliance_admin_credential *right)
{
    size_t index;
    size_t left_login_length = 0U;
    size_t right_login_length = 0U;

    while (left_login_length < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX &&
           left->login[left_login_length] != '\0') {
        ++left_login_length;
    }
    while (right_login_length < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX &&
           right->login[right_login_length] != '\0') {
        ++right_login_length;
    }
    if (left->version != right->version ||
        left->iterations != right->iterations ||
        left_login_length != right_login_length) {
        return false;
    }
    for (index = 0U; index < left_login_length; ++index) {
        if (left->login[index] != right->login[index]) {
            return false;
        }
    }
    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH;
         ++index) {
        if (left->salt[index] != right->salt[index]) {
            return false;
        }
    }
    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH;
         ++index) {
        if (left->verifier[index] != right->verifier[index]) {
            return false;
        }
    }
    return true;
}

enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_commit_admin_credential(
    struct appliance_persistent_secret_config *config,
    const struct appliance_admin_credential *credential)
{
    char value[APPLIANCE_PERSISTENT_SECRET_CONFIG_VALUE_MAX + 1U];
    unsigned char record[APPLIANCE_PERSISTENT_SECRET_CONFIG_RECORD_MAX];
    const size_t prefix_length =
        appliance_persistent_secret_config_string_length(
            appliance_persistent_secret_config_prefix);
    size_t value_length = 0U;
    size_t record_length;
    enum appliance_admin_credential_status credential_status;
    enum config_journal_status journal_status;
    enum appliance_persistent_secret_config_status status;
    enum appliance_secret_persistence_status persistence_status;
    enum appliance_secret_persistence_record_kind classified_kind =
        APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
    enum appliance_persistent_storage_namespace_id classified_namespace =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t classified_version = 0U;

    if (config == NULL || credential == NULL) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE;
    }
    if (config->admin_credential_present &&
        config->admin_kind ==
            APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL &&
        appliance_persistent_secret_config_credentials_equal(
            &config->admin_credential, credential)) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    }

    appliance_persistent_secret_config_zero_bytes(value, sizeof(value));
    credential_status = appliance_admin_credential_record_encode(
        credential, value, sizeof(value), &value_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
    }
    record_length = prefix_length + value_length;
    if (record_length > sizeof(record)) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD;
    }
    appliance_persistent_secret_config_zero_bytes(record, sizeof(record));
    {
        size_t index;

        for (index = 0U; index < prefix_length; ++index) {
            record[index] = (unsigned char)
                appliance_persistent_secret_config_prefix[index];
        }
        for (index = 0U; index < value_length; ++index) {
            record[prefix_length + index] = (unsigned char)value[index];
        }
    }

    persistence_status = appliance_secret_persistence_classify_record_key(
        (const char *)record, record_length, &classified_kind,
        &classified_namespace, &classified_version);
    if (persistence_status != APPLIANCE_SECRET_PERSISTENCE_OK ||
        classified_kind !=
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL ||
        classified_namespace !=
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED;
    }
    if (appliance_secret_persistence_validate_value(
            APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
            record + prefix_length, value_length) !=
        APPLIANCE_SECRET_PERSISTENCE_OK) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_secret_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return status;
    }
    appliance_persistent_secret_config_zero_bytes(
        config->admin_fingerprint, sizeof(config->admin_fingerprint));
    appliance_persistent_secret_config_zero_bytes(
        config->admin_hex, sizeof(config->admin_hex));
    config->admin_present = false;
    appliance_persistent_secret_config_zero_bytes(
        &config->admin_credential, sizeof(config->admin_credential));
    config->admin_credential = *credential;
    /*
     * The encoder proved the login is at most 32 valid bytes, but the copied
     * array must match what a fresh load produces: bytes after the login's
     * terminator are not part of the credential, so they are zeroed rather
     * than carried along from the caller's structure.
     */
    {
        size_t login_length = 0U;

        while (login_length < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX &&
               config->admin_credential.login[login_length] != '\0') {
            ++login_length;
        }
        appliance_persistent_secret_config_zero_bytes(
            config->admin_credential.login + login_length,
            sizeof(config->admin_credential.login) - login_length);
    }
    config->admin_credential_present = true;
    config->admin_kind =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL;
    config->source =
        APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED;
    return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

bool appliance_persistent_secret_config_admin(
    const struct appliance_persistent_secret_config *config,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE])
{
    if (config == NULL || fingerprint_out == NULL ||
        !config->initialized || !config->admin_present) {
        return false;
    }
    appliance_persistent_secret_config_zero_bytes(
        fingerprint_out, SHA256_DIGEST_SIZE);
    {
        size_t index;

        for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
            fingerprint_out[index] = config->admin_fingerprint[index];
        }
    }
    return true;
}

enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_admin_credential(
    const struct appliance_persistent_secret_config *config,
    struct appliance_admin_credential *credential_out)
{
    if (config == NULL || credential_out == NULL) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL;
    }
    appliance_persistent_secret_config_zero_bytes(
        credential_out, sizeof(*credential_out));
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE;
    }
    if (config->admin_kind ==
            APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL &&
        config->admin_credential_present) {
        *credential_out = config->admin_credential;
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
    }
    if (config->admin_kind ==
        APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD;
    }
    return APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT;
}

enum appliance_persistent_secret_config_admin_kind
appliance_persistent_secret_config_admin_kind(
    const struct appliance_persistent_secret_config *config)
{
    if (config == NULL || !config->initialized ||
        config->source == APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_NONE;
    }
    return config->admin_kind;
}

enum appliance_persistent_secret_config_source
appliance_persistent_secret_config_source(
    const struct appliance_persistent_secret_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE;
    }
    return config->source;
}

const char *appliance_persistent_secret_config_status_string(
    enum appliance_persistent_secret_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_OK:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_OK";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_FULL:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_FULL";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD:
        return "APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD";
    default:
        return NULL;
    }
}

const char *appliance_persistent_secret_config_source_string(
    enum appliance_persistent_secret_config_source source)
{
    switch (source) {
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    default:
        return NULL;
    }
}
