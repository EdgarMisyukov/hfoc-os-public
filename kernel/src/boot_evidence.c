#include "boot_evidence.h"

#include "config_kv.h"
#include "image_policy.h"
#include "sha256.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void boot_evidence_zero_bytes(uint8_t *bytes, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void boot_evidence_copy_bytes(uint8_t *destination,
                                     const uint8_t *source,
                                     size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool boot_evidence_bytes_equal(const uint8_t *left,
                                      const uint8_t *right,
                                      size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void boot_evidence_store_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t boot_evidence_load_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void boot_evidence_encode(
    const struct boot_evidence *evidence,
    uint8_t record[BOOT_EVIDENCE_RECORD_SIZE])
{
    static const uint8_t magic[8] = {
        'H', 'F', 'O', 'C', 'E', 'V', '0', '1',
    };

    boot_evidence_zero_bytes(record, BOOT_EVIDENCE_RECORD_SIZE);
    boot_evidence_copy_bytes(record, magic, 8U);
    boot_evidence_store_u32(record + 8U, evidence->version);
    boot_evidence_store_u32(record + 12U,
                            evidence->policy_generation);
    boot_evidence_store_u32(record + 16U, evidence->image_id);
    boot_evidence_copy_bytes(record + 24U,
                             evidence->measurement,
                             SHA256_DIGEST_SIZE);
    boot_evidence_copy_bytes(record + BOOT_EVIDENCE_DIGEST_OFFSET,
                             evidence->digest,
                             SHA256_DIGEST_SIZE);
}

static bool boot_evidence_magic_valid(
    const uint8_t record[BOOT_EVIDENCE_RECORD_SIZE])
{
    static const uint8_t magic[8] = {
        'H', 'F', 'O', 'C', 'E', 'V', '0', '1',
    };

    return boot_evidence_bytes_equal(record, magic, 8U);
}

static bool boot_evidence_reserved_valid(
    const uint8_t record[BOOT_EVIDENCE_RECORD_SIZE])
{
    uint32_t index;

    for (index = 20U; index < 24U; ++index) {
        if (record[index] != 0U) {
            return false;
        }
    }
    for (index = 56U; index < 64U; ++index) {
        if (record[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool boot_evidence_record_valid(
    const uint8_t record[BOOT_EVIDENCE_RECORD_SIZE])
{
    return boot_evidence_magic_valid(record) &&
           boot_evidence_load_u32(record + 8U) ==
               BOOT_EVIDENCE_VERSION &&
           boot_evidence_reserved_valid(record);
}

static bool boot_evidence_digest_valid(
    const struct boot_evidence *evidence)
{
    uint8_t record[BOOT_EVIDENCE_RECORD_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    enum sha256_status status;

    if (!boot_evidence_is_published(evidence)) {
        return false;
    }
    boot_evidence_encode(evidence, record);
    status = sha256_compute(record, BOOT_EVIDENCE_DIGEST_INPUT_SIZE,
                            digest);
    if (status != SHA256_OK) {
        return false;
    }
    return boot_evidence_bytes_equal(digest, evidence->digest,
                                     SHA256_DIGEST_SIZE);
}

enum boot_evidence_status boot_evidence_init(
    struct boot_evidence *evidence,
    const struct verification_policy *policy,
    const struct image_policy_binding *binding,
    const uint8_t measurement[SHA256_DIGEST_SIZE])
{
    const struct verification_policy_snapshot *active;
    const struct system_image_manifest *manifest;
    struct boot_evidence draft;
    uint8_t record[BOOT_EVIDENCE_RECORD_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    enum sha256_status status;

    if (evidence == NULL || policy == NULL || binding == NULL ||
        measurement == NULL) {
        return BOOT_EVIDENCE_NULL;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return BOOT_EVIDENCE_NO_POLICY;
    }
    manifest = image_policy_manifest(binding);
    if (manifest == NULL) {
        return BOOT_EVIDENCE_NO_IMAGE;
    }
    if (binding->policy_generation != active->generation) {
        return BOOT_EVIDENCE_POLICY_MISMATCH;
    }

    boot_evidence_zero_bytes((uint8_t *)&draft, sizeof(draft));
    draft.version = BOOT_EVIDENCE_VERSION;
    draft.policy_generation = active->generation;
    draft.image_id = manifest->image_id;
    boot_evidence_copy_bytes(draft.measurement, measurement,
                             SHA256_DIGEST_SIZE);
    draft.published = true;

    boot_evidence_encode(&draft, record);
    status = sha256_compute(record, BOOT_EVIDENCE_DIGEST_INPUT_SIZE,
                            digest);
    if (status != SHA256_OK) {
        return BOOT_EVIDENCE_BAD_STATE;
    }
    boot_evidence_copy_bytes(draft.digest, digest, SHA256_DIGEST_SIZE);
    draft.digest_match = true;
    *evidence = draft;
    return BOOT_EVIDENCE_OK;
}

enum boot_evidence_status boot_evidence_save(
    const struct boot_evidence *evidence,
    struct config_kv *audit_state)
{
    uint8_t record[BOOT_EVIDENCE_RECORD_SIZE];
    enum config_kv_status status;
    bool replaced = false;

    if (evidence == NULL || audit_state == NULL) {
        return BOOT_EVIDENCE_NULL;
    }
    if (!boot_evidence_digest_valid(evidence)) {
        return BOOT_EVIDENCE_BAD_STATE;
    }

    boot_evidence_encode(evidence, record);
    status = config_kv_put(
        audit_state, CONFIG_KV_KIND_AUDIT,
        BOOT_EVIDENCE_KEY, BOOT_EVIDENCE_KEY_LEN,
        record, BOOT_EVIDENCE_RECORD_SIZE, &replaced);
    if (status != CONFIG_KV_OK) {
        return BOOT_EVIDENCE_STORAGE_FAILED;
    }
    return BOOT_EVIDENCE_OK;
}

enum boot_evidence_status boot_evidence_load(
    struct boot_evidence *evidence,
    const struct config_kv *audit_state)
{
    uint8_t record[BOOT_EVIDENCE_RECORD_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    struct boot_evidence decoded;
    enum config_kv_status status;
    enum sha256_status digest_status;
    size_t value_length = 0U;
    uint64_t revision = 0U;
    bool match;

    if (evidence == NULL || audit_state == NULL) {
        return BOOT_EVIDENCE_NULL;
    }
    boot_evidence_zero_bytes(record, sizeof(record));
    status = config_kv_get(
        audit_state, CONFIG_KV_KIND_AUDIT,
        BOOT_EVIDENCE_KEY, BOOT_EVIDENCE_KEY_LEN,
        record, sizeof(record), &value_length, &revision);
    switch (status) {
    case CONFIG_KV_OK:
        break;
    case CONFIG_KV_NULL:
        return BOOT_EVIDENCE_NULL;
    case CONFIG_KV_NOT_FOUND:
        return BOOT_EVIDENCE_NOT_FOUND;
    case CONFIG_KV_TRUNCATED:
        return BOOT_EVIDENCE_INVALID_RECORD;
    default:
        return BOOT_EVIDENCE_STORAGE_FAILED;
    }
    if (value_length != BOOT_EVIDENCE_RECORD_SIZE ||
        !boot_evidence_record_valid(record)) {
        return BOOT_EVIDENCE_INVALID_RECORD;
    }

    digest_status = sha256_compute(
        record, BOOT_EVIDENCE_DIGEST_INPUT_SIZE, digest);
    if (digest_status != SHA256_OK) {
        return BOOT_EVIDENCE_BAD_STATE;
    }
    match = boot_evidence_bytes_equal(
        digest, record + BOOT_EVIDENCE_DIGEST_OFFSET,
        SHA256_DIGEST_SIZE);

    boot_evidence_zero_bytes((uint8_t *)&decoded, sizeof(decoded));
    decoded.version = boot_evidence_load_u32(record + 8U);
    decoded.policy_generation = boot_evidence_load_u32(record + 12U);
    decoded.image_id = boot_evidence_load_u32(record + 16U);
    boot_evidence_copy_bytes(decoded.measurement, record + 24U,
                             SHA256_DIGEST_SIZE);
    boot_evidence_copy_bytes(
        decoded.digest, record + BOOT_EVIDENCE_DIGEST_OFFSET,
        SHA256_DIGEST_SIZE);
    decoded.digest_match = match;
    decoded.published = true;
    *evidence = decoded;
    return match ? BOOT_EVIDENCE_OK : BOOT_EVIDENCE_DIGEST_MISMATCH;
}

enum boot_evidence_status boot_evidence_check_policy(
    const struct boot_evidence *evidence,
    const struct verification_policy *policy)
{
    const struct verification_policy_snapshot *active;

    if (evidence == NULL || policy == NULL) {
        return BOOT_EVIDENCE_NULL;
    }
    if (!boot_evidence_is_published(evidence)) {
        return BOOT_EVIDENCE_BAD_STATE;
    }
    if (!boot_evidence_digest_valid(evidence)) {
        return BOOT_EVIDENCE_DIGEST_MISMATCH;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return BOOT_EVIDENCE_NO_POLICY;
    }
    if (evidence->policy_generation != active->generation) {
        return BOOT_EVIDENCE_POLICY_MISMATCH;
    }
    return BOOT_EVIDENCE_OK;
}

bool boot_evidence_is_published(
    const struct boot_evidence *evidence)
{
    return evidence != NULL &&
           evidence->version == BOOT_EVIDENCE_VERSION &&
           evidence->published;
}

const char *boot_evidence_status_string(
    enum boot_evidence_status status)
{
    switch (status) {
    case BOOT_EVIDENCE_OK:
        return "BOOT_EVIDENCE_OK";
    case BOOT_EVIDENCE_NULL:
        return "BOOT_EVIDENCE_NULL";
    case BOOT_EVIDENCE_BAD_STATE:
        return "BOOT_EVIDENCE_BAD_STATE";
    case BOOT_EVIDENCE_NO_POLICY:
        return "BOOT_EVIDENCE_NO_POLICY";
    case BOOT_EVIDENCE_NO_IMAGE:
        return "BOOT_EVIDENCE_NO_IMAGE";
    case BOOT_EVIDENCE_POLICY_MISMATCH:
        return "BOOT_EVIDENCE_POLICY_MISMATCH";
    case BOOT_EVIDENCE_INVALID_RECORD:
        return "BOOT_EVIDENCE_INVALID_RECORD";
    case BOOT_EVIDENCE_DIGEST_MISMATCH:
        return "BOOT_EVIDENCE_DIGEST_MISMATCH";
    case BOOT_EVIDENCE_STORAGE_FAILED:
        return "BOOT_EVIDENCE_STORAGE_FAILED";
    case BOOT_EVIDENCE_NOT_FOUND:
        return "BOOT_EVIDENCE_NOT_FOUND";
    default:
        return NULL;
    }
}
