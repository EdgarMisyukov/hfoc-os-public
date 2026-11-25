#include "appliance_support_bundle.h"

#include "config_kv.h"
#include "diagnostic_privacy.h"
#include "sha256.h"
#include "support_bundle_exporter.h"
#include "support_bundle_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned char appliance_support_bundle_magic[
    APPLIANCE_SUPPORT_BUNDLE_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'S', '1'
};

static const unsigned char appliance_support_bundle_exporter_magic[
    SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'E', 'X'
};

static const unsigned char appliance_support_bundle_manifest_magic[
    SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE] = {
    'H', 'F', 'O', 'C', 'S', 'B', 'M', 'B'
};

static void appliance_support_bundle_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_support_bundle_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_support_bundle_bytes_equal(
    const unsigned char *left,
    const unsigned char *right,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

/* Constant-time comparison: a MAC check must not leak where it differs. */
static bool appliance_support_bundle_bytes_equal_ct(
    const unsigned char *left,
    const unsigned char *right,
    size_t length)
{
    unsigned char difference = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        difference = (unsigned char)(difference |
                                     (left[index] ^ right[index]));
    }
    return difference == 0U;
}

static bool appliance_support_bundle_text_equal(
    const char *left,
    const char *right,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void appliance_support_bundle_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void appliance_support_bundle_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t appliance_support_bundle_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t appliance_support_bundle_load_le64(
    const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool appliance_support_bundle_state_valid(
    enum appliance_support_bundle_state state)
{
    return state >= APPLIANCE_SUPPORT_BUNDLE_STATE_NONE &&
           state <= APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED;
}

static bool appliance_support_bundle_status_valid(
    enum appliance_support_bundle_status status)
{
    return status >= APPLIANCE_SUPPORT_BUNDLE_OK &&
           status <= APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL;
}

static bool appliance_support_bundle_scheme_valid(
    enum appliance_support_bundle_scheme scheme)
{
    return scheme >= APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE &&
           scheme <= APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256;
}

static bool appliance_support_bundle_result_valid(
    const struct appliance_support_bundle *bundle)
{
    return bundle != NULL &&
           bundle->version == APPLIANCE_SUPPORT_BUNDLE_VERSION &&
           appliance_support_bundle_state_valid(bundle->state) &&
           appliance_support_bundle_scheme_valid(bundle->scheme) &&
           appliance_support_bundle_status_valid(bundle->last_status);
}

static bool appliance_support_bundle_policy_matches(const char *policy_id)
{
    return appliance_support_bundle_text_equal(
        policy_id, SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
}

static bool appliance_support_bundle_config_entry_secret(
    const struct config_kv_entry *entry)
{
    enum diagnostic_privacy_category category;

    return diagnostic_privacy_classify_key(
               entry->key, entry->key_length, &category) ==
               DIAGNOSTIC_PRIVACY_OK &&
           category == DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL;
}

/*
 * The redaction report is re-derived from the live configuration with exactly
 * the counting rule the manifest applies: only CONFIG-kind entries take part,
 * secret-classified keys are withheld, everything else is included.
 */
enum appliance_support_bundle_status appliance_support_bundle_redaction_eval(
    const struct support_bundle_manifest_source *source,
    struct appliance_support_bundle_redaction *report_out)
{
    uint32_t config_entries = 0U;
    uint32_t included = 0U;
    uint32_t withheld = 0U;
    size_t index;

    if (source == NULL || report_out == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    if (source->version != SUPPORT_BUNDLE_MANIFEST_VERSION) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION;
    }
    if (source->config != NULL) {
        if (source->config->entries == NULL ||
            source->config->capacity == 0U ||
            source->config->capacity > CONFIG_KV_CAPACITY_MAX ||
            source->config->count > source->config->capacity) {
            return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
        }
        for (index = 0U; index < source->config->capacity; ++index) {
            const struct config_kv_entry *entry =
                &source->config->entries[index];

            if (!entry->occupied ||
                entry->kind != CONFIG_KV_KIND_CONFIG) {
                continue;
            }
            ++config_entries;
            if (appliance_support_bundle_config_entry_secret(entry)) {
                ++withheld;
            } else {
                ++included;
            }
        }
    }

    appliance_support_bundle_zero_bytes(
        (unsigned char *)report_out, sizeof(*report_out));
    report_out->version = APPLIANCE_SUPPORT_BUNDLE_VERSION;
    appliance_support_bundle_copy_bytes(
        (unsigned char *)report_out->policy_id,
        (const unsigned char *)
            SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
    report_out->policy_id[
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH] = '\0';
    report_out->declared_keys = DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT;
    report_out->config_entries = config_entries;
    report_out->included = included;
    report_out->withheld = withheld;
    report_out->leaked = 0U;
    report_out->policy_applied = true;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

struct appliance_support_bundle_parsed {
    uint32_t exporter_version;
    uint32_t manifest_version;
    uint32_t entry_count;
    uint64_t bundle_bytes;
    uint64_t payload_bytes;
    uint32_t config_included;
    uint32_t config_excluded;
    uint8_t bundle_digest[SHA256_DIGEST_SIZE];
    uint8_t manifest_digest[SHA256_DIGEST_SIZE];
};

static enum appliance_support_bundle_status
appliance_support_bundle_parse(
    const unsigned char *bytes,
    size_t length,
    struct appliance_support_bundle_parsed *parsed_out)
{
    const unsigned char *manifest;
    unsigned char digest[SHA256_DIGEST_SIZE];

    if (length < SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
                     SUPPORT_BUNDLE_MANIFEST_SIZE +
                     SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (!appliance_support_bundle_bytes_equal(
            bytes, appliance_support_bundle_exporter_magic,
            SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE) ||
        bytes[SUPPORT_BUNDLE_EXPORTER_VERSION_OFFSET] !=
            SUPPORT_BUNDLE_EXPORTER_VERSION ||
        !appliance_support_bundle_text_equal(
            (const char *)(bytes +
                           SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_OFFSET),
            SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID,
            SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_LENGTH)) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (appliance_support_bundle_load_le32(
            bytes +
            SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_SIZE_OFFSET) !=
            SUPPORT_BUNDLE_MANIFEST_SIZE ||
        appliance_support_bundle_load_le32(
            bytes +
            SUPPORT_BUNDLE_EXPORTER_HEADER_ENTRY_COUNT_OFFSET) !=
            SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX ||
        appliance_support_bundle_load_le32(
            bytes +
            SUPPORT_BUNDLE_EXPORTER_HEADER_EXPORTER_VERSION_OFFSET) !=
            SUPPORT_BUNDLE_EXPORTER_VERSION ||
        appliance_support_bundle_load_le32(
            bytes +
            SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_VERSION_OFFSET) !=
            SUPPORT_BUNDLE_MANIFEST_VERSION) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (appliance_support_bundle_load_le64(
            bytes +
            SUPPORT_BUNDLE_EXPORTER_HEADER_TOTAL_BYTES_OFFSET) !=
            (uint64_t)length) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE +
            SUPPORT_BUNDLE_MANIFEST_SIZE +
            appliance_support_bundle_load_le64(
                bytes +
                SUPPORT_BUNDLE_EXPORTER_HEADER_PAYLOAD_BYTES_OFFSET) +
            SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE != (uint64_t)length) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }

    manifest = bytes + SUPPORT_BUNDLE_EXPORTER_MANIFEST_OFFSET;
    if (!appliance_support_bundle_bytes_equal(
            manifest, appliance_support_bundle_manifest_magic,
            SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE) ||
        manifest[SUPPORT_BUNDLE_MANIFEST_VERSION_OFFSET] !=
            SUPPORT_BUNDLE_MANIFEST_VERSION ||
        !appliance_support_bundle_text_equal(
            (const char *)(manifest +
                           SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_OFFSET),
            SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID,
            SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_LENGTH) ||
        !appliance_support_bundle_policy_matches(
            (const char *)(manifest +
                           SUPPORT_BUNDLE_MANIFEST_POLICY_ID_OFFSET))) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }

    (void)sha256_compute(
        bytes, length - SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE, digest);
    if (!appliance_support_bundle_bytes_equal(
            digest, bytes + length - SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
            SHA256_DIGEST_SIZE)) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    (void)sha256_compute(manifest, SUPPORT_BUNDLE_MANIFEST_SIZE, digest);

    appliance_support_bundle_zero_bytes(
        (unsigned char *)parsed_out, sizeof(*parsed_out));
    parsed_out->exporter_version = SUPPORT_BUNDLE_EXPORTER_VERSION;
    parsed_out->manifest_version = SUPPORT_BUNDLE_MANIFEST_VERSION;
    parsed_out->entry_count = SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX;
    parsed_out->bundle_bytes = (uint64_t)length;
    parsed_out->payload_bytes = appliance_support_bundle_load_le64(
        bytes + SUPPORT_BUNDLE_EXPORTER_HEADER_PAYLOAD_BYTES_OFFSET);
    parsed_out->config_included = appliance_support_bundle_load_le32(
        manifest + SUPPORT_BUNDLE_MANIFEST_CONFIG_INCLUDED_OFFSET);
    parsed_out->config_excluded = appliance_support_bundle_load_le32(
        manifest + SUPPORT_BUNDLE_MANIFEST_CONFIG_EXCLUDED_OFFSET);
    appliance_support_bundle_copy_bytes(
        parsed_out->manifest_digest, digest, SHA256_DIGEST_SIZE);
    (void)sha256_compute(
        bytes, length - SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE,
        parsed_out->bundle_digest);
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

static bool appliance_support_bundle_report_consistent(
    const struct appliance_support_bundle_redaction *report,
    const struct appliance_support_bundle_parsed *parsed)
{
    return report->version == APPLIANCE_SUPPORT_BUNDLE_VERSION &&
           report->policy_applied &&
           report->leaked == 0U &&
           report->declared_keys == DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT &&
           appliance_support_bundle_policy_matches(report->policy_id) &&
           report->included == parsed->config_included &&
           report->withheld == parsed->config_excluded;
}

enum appliance_support_bundle_status appliance_support_bundle_bind(
    const void *exported,
    size_t exported_length,
    const struct appliance_support_bundle_redaction *report,
    struct appliance_support_bundle_binding *binding_out)
{
    struct appliance_support_bundle_parsed parsed;
    enum appliance_support_bundle_status status;

    if (exported == NULL || report == NULL || binding_out == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    status = appliance_support_bundle_parse(
        (const unsigned char *)exported, exported_length, &parsed);
    if (status != APPLIANCE_SUPPORT_BUNDLE_OK) {
        return status;
    }
    if (!appliance_support_bundle_report_consistent(report, &parsed)) {
        return APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED;
    }

    appliance_support_bundle_zero_bytes(
        (unsigned char *)binding_out, sizeof(*binding_out));
    binding_out->version = APPLIANCE_SUPPORT_BUNDLE_VERSION;
    binding_out->exporter_version = parsed.exporter_version;
    binding_out->manifest_version = parsed.manifest_version;
    binding_out->entry_count = parsed.entry_count;
    binding_out->bundle_bytes = parsed.bundle_bytes;
    binding_out->payload_bytes = parsed.payload_bytes;
    appliance_support_bundle_copy_bytes(
        binding_out->bundle_digest, parsed.bundle_digest,
        SHA256_DIGEST_SIZE);
    appliance_support_bundle_copy_bytes(
        binding_out->manifest_digest, parsed.manifest_digest,
        SHA256_DIGEST_SIZE);
    binding_out->redaction = *report;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

static void appliance_support_bundle_write_header(
    unsigned char *output,
    uint32_t value)
{
    appliance_support_bundle_store_le32(output, value);
}

enum appliance_support_bundle_status appliance_support_bundle_encode(
    const struct appliance_support_bundle_binding *binding,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char *output = buffer;

    if (binding == NULL || buffer == NULL || length_out == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    if (binding->version != APPLIANCE_SUPPORT_BUNDLE_VERSION) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION;
    }
    if (capacity < APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE) {
        return APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL;
    }

    appliance_support_bundle_zero_bytes(
        output, APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE);
    appliance_support_bundle_copy_bytes(
        output, appliance_support_bundle_magic,
        APPLIANCE_SUPPORT_BUNDLE_MAGIC_SIZE);
    appliance_support_bundle_copy_bytes(
        output + APPLIANCE_SUPPORT_BUNDLE_MAGIC_SIZE,
        (const unsigned char *)APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID,
        APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID_LENGTH);
    appliance_support_bundle_write_header(
        output + 22U, binding->version);
    appliance_support_bundle_write_header(
        output + 26U, binding->exporter_version);
    appliance_support_bundle_write_header(
        output + 30U, binding->manifest_version);
    appliance_support_bundle_write_header(
        output + 34U, binding->entry_count);
    appliance_support_bundle_store_le64(output + 38U,
                                        binding->bundle_bytes);
    appliance_support_bundle_store_le64(output + 46U,
                                        binding->payload_bytes);
    appliance_support_bundle_copy_bytes(
        output + 54U, binding->bundle_digest, SHA256_DIGEST_SIZE);
    appliance_support_bundle_copy_bytes(
        output + 86U, binding->manifest_digest, SHA256_DIGEST_SIZE);
    appliance_support_bundle_copy_bytes(
        output + 118U,
        (const unsigned char *)binding->redaction.policy_id,
        SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH);
    appliance_support_bundle_write_header(
        output + 129U, binding->redaction.declared_keys);
    appliance_support_bundle_write_header(
        output + 133U, binding->redaction.config_entries);
    appliance_support_bundle_write_header(
        output + 137U, binding->redaction.included);
    appliance_support_bundle_write_header(
        output + 141U, binding->redaction.withheld);
    appliance_support_bundle_write_header(
        output + 145U, binding->redaction.leaked);
    appliance_support_bundle_write_header(
        output + 149U,
        binding->redaction.policy_applied
            ? APPLIANCE_SUPPORT_BUNDLE_PURPOSE
            : 0U);
    *length_out = APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

enum appliance_support_bundle_status appliance_support_bundle_digest(
    const struct appliance_support_bundle_binding *binding,
    uint8_t digest_out[APPLIANCE_SUPPORT_BUNDLE_DIGEST_SIZE])
{
    unsigned char encoding[APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE];
    size_t encoding_length = 0U;

    if (binding == NULL || digest_out == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    if (appliance_support_bundle_encode(
            binding, encoding, sizeof(encoding), &encoding_length) !=
        APPLIANCE_SUPPORT_BUNDLE_OK) {
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    (void)sha256_compute(encoding, encoding_length, digest_out);
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

static void appliance_support_bundle_reset_result(
    struct appliance_support_bundle *bundle,
    enum appliance_support_bundle_state state,
    enum appliance_support_bundle_status status)
{
    bundle->version = APPLIANCE_SUPPORT_BUNDLE_VERSION;
    bundle->state = state;
    bundle->scheme = APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE;
    bundle->last_status = status;
    bundle->verified = false;
    appliance_support_bundle_zero_bytes(
        bundle->mac, APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE);
}

enum appliance_support_bundle_status appliance_support_bundle_seal(
    struct appliance_support_bundle *bundle,
    const void *exported,
    size_t exported_length,
    const struct appliance_support_bundle_redaction *report,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *key,
    size_t key_length)
{
    struct appliance_support_bundle_binding binding;
    unsigned char encoding[APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE];
    size_t encoding_length = 0U;
    enum appliance_support_bundle_status status;

    if (bundle == NULL || exported == NULL || report == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    appliance_support_bundle_zero_bytes(
        (unsigned char *)bundle, sizeof(*bundle));
    status = appliance_support_bundle_bind(
        exported, exported_length, report, &binding);
    if (status == APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED) {
        appliance_support_bundle_reset_result(
            bundle, APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED,
            APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED);
        bundle->refused_count = 1U;
        return APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED;
    }
    if (status != APPLIANCE_SUPPORT_BUNDLE_OK) {
        appliance_support_bundle_reset_result(
            bundle, APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE, status);
        return status;
    }

    appliance_support_bundle_reset_result(
        bundle, APPLIANCE_SUPPORT_BUNDLE_STATE_UNSIGNED,
        APPLIANCE_SUPPORT_BUNDLE_UNSIGNED);
    bundle->binding = binding;
    (void)appliance_support_bundle_digest(&binding, bundle->digest);

    if (key == NULL || key_length == 0U) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY;
        bundle->unsigned_count = 1U;
        return APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY;
    }
    if (mac == NULL) {
        bundle->state = APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE;
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE;
        bundle->unsigned_count = 1U;
        return APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE;
    }
    if (appliance_support_bundle_encode(
            &binding, encoding, sizeof(encoding), &encoding_length) !=
        APPLIANCE_SUPPORT_BUNDLE_OK) {
        bundle->state = APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE;
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    status = mac(context, key, key_length, encoding, encoding_length,
                 bundle->mac);
    if (status != APPLIANCE_SUPPORT_BUNDLE_OK) {
        bundle->state = APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE;
        bundle->scheme = APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE;
        bundle->last_status = status;
        appliance_support_bundle_zero_bytes(
            bundle->mac, APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE);
        if (status == APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE) {
            bundle->unsigned_count = 1U;
        }
        return status;
    }

    bundle->state = APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED;
    bundle->scheme = APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256;
    bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_OK;
    bundle->sealed_count = 1U;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

static bool appliance_support_bundle_binding_equal(
    const struct appliance_support_bundle_binding *left,
    const struct appliance_support_bundle_binding *right)
{
    return left->version == right->version &&
           left->exporter_version == right->exporter_version &&
           left->manifest_version == right->manifest_version &&
           left->entry_count == right->entry_count &&
           left->bundle_bytes == right->bundle_bytes &&
           left->payload_bytes == right->payload_bytes &&
           appliance_support_bundle_bytes_equal(
               left->bundle_digest, right->bundle_digest,
               SHA256_DIGEST_SIZE) &&
           appliance_support_bundle_bytes_equal(
               left->manifest_digest, right->manifest_digest,
               SHA256_DIGEST_SIZE) &&
           left->redaction.version == right->redaction.version &&
           left->redaction.declared_keys ==
               right->redaction.declared_keys &&
           left->redaction.config_entries ==
               right->redaction.config_entries &&
           left->redaction.included == right->redaction.included &&
           left->redaction.withheld == right->redaction.withheld &&
           left->redaction.leaked == right->redaction.leaked &&
           left->redaction.policy_applied ==
               right->redaction.policy_applied &&
           appliance_support_bundle_policy_matches(
               left->redaction.policy_id);
}

enum appliance_support_bundle_status appliance_support_bundle_verify(
    struct appliance_support_bundle *bundle,
    const void *exported,
    size_t exported_length,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *key,
    size_t key_length)
{
    struct appliance_support_bundle_binding derived;
    unsigned char encoding[APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE];
    unsigned char expected_mac[APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE];
    size_t encoding_length = 0U;
    enum appliance_support_bundle_status status;

    if (bundle == NULL || exported == NULL || mac == NULL ||
        key == NULL || key_length == 0U) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    bundle->verified = false;
    if (bundle->version != APPLIANCE_SUPPORT_BUNDLE_VERSION) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION;
        return APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION;
    }
    if (bundle->state != APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED ||
        bundle->scheme !=
            APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_UNSIGNED;
        return APPLIANCE_SUPPORT_BUNDLE_UNSIGNED;
    }

    status = appliance_support_bundle_bind(
        exported, exported_length, &bundle->binding.redaction, &derived);
    if (status == APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED;
        return APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED;
    }
    if (status != APPLIANCE_SUPPORT_BUNDLE_OK) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (!appliance_support_bundle_binding_equal(&derived,
                                                &bundle->binding)) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    if (appliance_support_bundle_encode(
            &bundle->binding, encoding, sizeof(encoding),
            &encoding_length) != APPLIANCE_SUPPORT_BUNDLE_OK) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
        return APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING;
    }
    status = mac(context, key, key_length, encoding, encoding_length,
                 expected_mac);
    if (status != APPLIANCE_SUPPORT_BUNDLE_OK) {
        bundle->last_status = status;
        return status;
    }
    if (!appliance_support_bundle_bytes_equal_ct(
            expected_mac, bundle->mac,
            APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE)) {
        bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_MISMATCH;
        return APPLIANCE_SUPPORT_BUNDLE_MISMATCH;
    }

    bundle->verified = true;
    bundle->last_status = APPLIANCE_SUPPORT_BUNDLE_OK;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

bool appliance_support_bundle_is_sealed(
    const struct appliance_support_bundle *bundle)
{
    size_t index;
    bool mac_present = false;

    if (!appliance_support_bundle_result_valid(bundle) ||
        bundle->state != APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED ||
        bundle->scheme !=
            APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256) {
        return false;
    }
    for (index = 0U; index < APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE; ++index) {
        if (bundle->mac[index] != 0U) {
            mac_present = true;
        }
    }
    return mac_present;
}

uint32_t appliance_support_bundle_withheld(
    const struct appliance_support_bundle *bundle)
{
    if (!appliance_support_bundle_result_valid(bundle)) {
        return 0U;
    }
    return bundle->binding.redaction.withheld;
}

enum appliance_support_bundle_state appliance_support_bundle_state(
    const struct appliance_support_bundle *bundle)
{
    if (!appliance_support_bundle_result_valid(bundle)) {
        return APPLIANCE_SUPPORT_BUNDLE_STATE_NONE;
    }
    return bundle->state;
}

enum appliance_support_bundle_status appliance_support_bundle_last_status(
    const struct appliance_support_bundle *bundle)
{
    if (!appliance_support_bundle_result_valid(bundle)) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    return bundle->last_status;
}

struct appliance_support_bundle_render_buffer {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_support_bundle_render_text(
    struct appliance_support_bundle_render_buffer *buffer,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (buffer->length + 1U < buffer->capacity) {
            buffer->data[buffer->length] = text[index];
            ++buffer->length;
        } else {
            buffer->overflow = true;
        }
        ++index;
    }
}

static void appliance_support_bundle_render_u32(
    struct appliance_support_bundle_render_buffer *buffer,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        appliance_support_bundle_render_text(buffer, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        appliance_support_bundle_render_text(buffer, single);
    }
}

static void appliance_support_bundle_render_hex8(
    struct appliance_support_bundle_render_buffer *buffer,
    const uint8_t bytes[8])
{
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        char pair[3];

        pair[0] = hex[(bytes[index] >> 4U) & 0x0FU];
        pair[1] = hex[bytes[index] & 0x0FU];
        pair[2] = '\0';
        appliance_support_bundle_render_text(buffer, pair);
    }
}

static const char *appliance_support_bundle_redaction_string(
    const struct appliance_support_bundle *bundle)
{
    if (!appliance_support_bundle_result_valid(bundle) ||
        bundle->state == APPLIANCE_SUPPORT_BUNDLE_STATE_NONE ||
        bundle->state == APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE) {
        return "UNAVAILABLE";
    }
    if (bundle->state ==
            APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED ||
        bundle->binding.redaction.leaked != 0U ||
        !bundle->binding.redaction.policy_applied) {
        return "FAILED";
    }
    return "OK";
}

enum appliance_support_bundle_status appliance_support_bundle_render(
    const struct appliance_support_bundle *bundle,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_support_bundle_render_buffer buffer;
    char staging[APPLIANCE_SUPPORT_BUNDLE_RENDER_MAX];

    if (bundle == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_SUPPORT_BUNDLE_NULL;
    }
    buffer.data = staging;
    buffer.capacity = sizeof(staging);
    buffer.length = 0U;
    buffer.overflow = false;

    appliance_support_bundle_render_text(&buffer, "support_bundle v=1");
    appliance_support_bundle_render_text(&buffer, " state=");
    appliance_support_bundle_render_text(
        &buffer, appliance_support_bundle_state_string(bundle->state));
    appliance_support_bundle_render_text(&buffer, " scheme=");
    appliance_support_bundle_render_text(
        &buffer, appliance_support_bundle_scheme_string(bundle->scheme));
    appliance_support_bundle_render_text(&buffer, " sections=");
    appliance_support_bundle_render_u32(&buffer, bundle->binding.entry_count);
    appliance_support_bundle_render_text(&buffer, " entries=");
    appliance_support_bundle_render_u32(
        &buffer, bundle->binding.redaction.config_entries);
    appliance_support_bundle_render_text(&buffer, " included=");
    appliance_support_bundle_render_u32(
        &buffer, bundle->binding.redaction.included);
    appliance_support_bundle_render_text(&buffer, " withheld=");
    appliance_support_bundle_render_u32(
        &buffer, bundle->binding.redaction.withheld);
    appliance_support_bundle_render_text(&buffer, " leaked=");
    appliance_support_bundle_render_u32(
        &buffer, bundle->binding.redaction.leaked);
    appliance_support_bundle_render_text(&buffer, " redaction=");
    appliance_support_bundle_render_text(
        &buffer, appliance_support_bundle_redaction_string(bundle));
    appliance_support_bundle_render_text(&buffer, " verified=");
    appliance_support_bundle_render_text(
        &buffer, bundle->verified ? "YES" : "NO");
    appliance_support_bundle_render_text(&buffer, " bundle=");
    appliance_support_bundle_render_hex8(&buffer,
                                         bundle->binding.bundle_digest);
    appliance_support_bundle_render_text(&buffer, " seal=");
    if (appliance_support_bundle_is_sealed(bundle)) {
        appliance_support_bundle_render_hex8(&buffer, bundle->digest);
    } else {
        appliance_support_bundle_render_text(&buffer, "NONE");
    }

    if (buffer.overflow || buffer.length + 1U > capacity) {
        *output_length = 0U;
        return APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL;
    }
    appliance_support_bundle_copy_bytes(
        (unsigned char *)output, (const unsigned char *)staging,
        buffer.length);
    output[buffer.length] = '\0';
    *output_length = buffer.length;
    return APPLIANCE_SUPPORT_BUNDLE_OK;
}

const char *appliance_support_bundle_state_string(
    enum appliance_support_bundle_state state)
{
    switch (state) {
    case APPLIANCE_SUPPORT_BUNDLE_STATE_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_BUNDLE_STATE_UNSIGNED:
        return "UNSIGNED";
    case APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED:
        return "SEALED";
    case APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED:
        return "REDACTION_FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_bundle_status_string(
    enum appliance_support_bundle_status status)
{
    switch (status) {
    case APPLIANCE_SUPPORT_BUNDLE_OK:
        return "OK";
    case APPLIANCE_SUPPORT_BUNDLE_NULL:
        return "NULL";
    case APPLIANCE_SUPPORT_BUNDLE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING:
        return "BAD_BINDING";
    case APPLIANCE_SUPPORT_BUNDLE_TOO_SMALL:
        return "TOO_SMALL";
    case APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED:
        return "REDACTION_FAILED";
    case APPLIANCE_SUPPORT_BUNDLE_UNSIGNED:
        return "UNSIGNED";
    case APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY:
        return "NO_SEAL_KEY";
    case APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE:
        return "MAC_UNAVAILABLE";
    case APPLIANCE_SUPPORT_BUNDLE_MISMATCH:
        return "MISMATCH";
    case APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_bundle_scheme_string(
    enum appliance_support_bundle_scheme scheme)
{
    switch (scheme) {
    case APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256:
        return "HMAC_SHA256";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_bundle_schema_id(void)
{
    return APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID;
}
