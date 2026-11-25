#include "boot_chain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char boot_chain_magic[BOOT_CHAIN_MANIFEST_MAGIC_SIZE] =
    BOOT_CHAIN_MAGIC_BYTES;

static const char *const boot_chain_status_names[] = {
    "OK",
    "NULL",
    "BAD_VERSION",
    "ENVELOPE_INVALID",
    "POLICY_MISMATCH",
    "MANIFEST_INVALID",
    "ALGORITHM_UNSUPPORTED",
    "VERIFIER_UNAVAILABLE",
    "SIGNATURE_INVALID",
    "SIGNATURE_MISMATCH",
    "RELEASE_MISMATCH",
    "COMPONENT_MISSING",
    "COMPONENT_UNDECLARED",
    "COMPONENT_LENGTH_MISMATCH",
    "COMPONENT_DIGEST_MISMATCH",
    "COMPONENT_LIMIT",
    "CAPACITY",
    "MEASUREMENT_FAILED",
};

static uint32_t boot_chain_read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t boot_chain_read_u64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static void boot_chain_write_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void boot_chain_write_u64(unsigned char *bytes, uint64_t value)
{
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] =
            (unsigned char)((value >> (index * 8U)) & 0xFFU);
    }
}

static bool boot_chain_component_kind_valid(uint32_t kind)
{
    return kind >= BOOT_CHAIN_COMPONENT_BOOT_PAYLOAD &&
           kind <= BOOT_CHAIN_COMPONENT_KERNEL;
}

static enum boot_chain_status boot_chain_component_validate(
    const struct boot_chain_component *component)
{
    if (component == NULL) {
        return BOOT_CHAIN_NULL;
    }
    if (!boot_chain_component_kind_valid(component->kind)) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (component->length == 0U || component->data == NULL) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    return BOOT_CHAIN_OK;
}

static enum boot_chain_status boot_chain_build_body(
    uint32_t release_id,
    uint32_t signer_id,
    enum trust_anchor_algorithm algorithm,
    const struct boot_chain_component *components,
    size_t component_count,
    unsigned char body[BOOT_CHAIN_MANIFEST_BODY_SIZE])
{
    size_t index;

    if (components == NULL || component_count == 0U) {
        return BOOT_CHAIN_NULL;
    }
    if (component_count > BOOT_CHAIN_COMPONENT_CAPACITY) {
        return BOOT_CHAIN_COMPONENT_LIMIT;
    }
    if (release_id < BOOT_CHAIN_RELEASE_ID_MIN) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return BOOT_CHAIN_ALGORITHM_UNSUPPORTED;
    }

    memset(body, 0, BOOT_CHAIN_MANIFEST_BODY_SIZE);
    memcpy(body, boot_chain_magic, sizeof(boot_chain_magic));
    body[8] = (unsigned char)BOOT_CHAIN_MANIFEST_FORMAT_VERSION;
    body[9] = (unsigned char)algorithm;
    boot_chain_write_u32(body + 12U, release_id);
    boot_chain_write_u32(body + 16U, signer_id);
    boot_chain_write_u32(body + 20U, (uint32_t)component_count);

    for (index = 0U; index < component_count; ++index) {
        unsigned char *entry =
            body + BOOT_CHAIN_MANIFEST_HEADER_SIZE +
            (index * BOOT_CHAIN_COMPONENT_ENTRY_SIZE);
        enum boot_chain_status status =
            boot_chain_component_validate(&components[index]);

        if (status != BOOT_CHAIN_OK) {
            return status;
        }
        if (sha256_compute(components[index].data,
                           components[index].length,
                           entry + 16U) != SHA256_OK) {
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
        boot_chain_write_u32(entry, components[index].kind);
        boot_chain_write_u64(entry + 8U,
                             (uint64_t)components[index].length);
    }
    return BOOT_CHAIN_OK;
}

enum boot_chain_status boot_chain_chain_digest(
    uint32_t release_id,
    uint32_t signer_id,
    const struct boot_chain_component *components,
    size_t component_count,
    unsigned char digest_out[SHA256_DIGEST_SIZE])
{
    unsigned char body[BOOT_CHAIN_MANIFEST_BODY_SIZE];
    enum boot_chain_status status;

    if (digest_out == NULL) {
        return BOOT_CHAIN_NULL;
    }
    status = boot_chain_build_body(release_id, signer_id,
                                   TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
                                   components, component_count, body);
    if (status != BOOT_CHAIN_OK) {
        return status;
    }
    if (sha256_compute(body, BOOT_CHAIN_MANIFEST_BODY_SIZE,
                       digest_out) != SHA256_OK) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    return BOOT_CHAIN_OK;
}

enum boot_chain_status boot_chain_manifest_seal(
    const struct verification_policy *policy,
    uint32_t release_id,
    uint32_t signer_id,
    enum trust_anchor_algorithm algorithm,
    const struct boot_chain_component *components,
    size_t component_count,
    const unsigned char *signature,
    size_t signature_length,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char manifest[BOOT_CHAIN_MANIFEST_SIZE];
    enum boot_chain_status status;

    if (policy == NULL || signature == NULL || buffer == NULL ||
        length_out == NULL) {
        return BOOT_CHAIN_NULL;
    }
    *length_out = 0U;
    if (signature_length != BOOT_CHAIN_SIGNATURE_SIZE) {
        return BOOT_CHAIN_SIGNATURE_INVALID;
    }
    if (capacity < BOOT_CHAIN_MANIFEST_SIZE) {
        return BOOT_CHAIN_CAPACITY;
    }

    status = boot_chain_build_body(release_id, signer_id, algorithm,
                                   components, component_count, manifest);
    if (status != BOOT_CHAIN_OK) {
        return status;
    }
    memcpy(manifest + BOOT_CHAIN_MANIFEST_BODY_SIZE, signature,
           BOOT_CHAIN_SIGNATURE_SIZE);

    if (signed_metadata_seal(policy, KEY_USE_PURPOSE_IMAGE, signer_id,
                             manifest, sizeof(manifest), buffer, capacity,
                             length_out) != SIGNED_METADATA_OK) {
        *length_out = 0U;
        return BOOT_CHAIN_ENVELOPE_INVALID;
    }
    return BOOT_CHAIN_OK;
}

static enum boot_chain_status boot_chain_parse_manifest(
    const unsigned char *manifest,
    size_t manifest_length,
    struct boot_chain_verify_report *report)
{
    const unsigned char *entry;
    uint32_t declared_count;
    size_t index;

    if (manifest_length != BOOT_CHAIN_MANIFEST_SIZE) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (memcmp(manifest, boot_chain_magic, sizeof(boot_chain_magic)) != 0) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (manifest[8] != (unsigned char)BOOT_CHAIN_MANIFEST_FORMAT_VERSION) {
        return BOOT_CHAIN_BAD_VERSION;
    }
    report->algorithm = (enum trust_anchor_algorithm)manifest[9];
    if (report->algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return BOOT_CHAIN_ALGORITHM_UNSUPPORTED;
    }
    if (manifest[10] != 0U || manifest[11] != 0U) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    report->release_id = boot_chain_read_u32(manifest + 12U);
    if (report->release_id < BOOT_CHAIN_RELEASE_ID_MIN) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    report->signer_id = boot_chain_read_u32(manifest + 16U);
    if (report->signer_id < TRUST_ANCHOR_ID_MIN ||
        report->signer_id > TRUST_ANCHOR_ID_MAX) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    declared_count = boot_chain_read_u32(manifest + 20U);
    if (declared_count == 0U) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (declared_count > BOOT_CHAIN_COMPONENT_CAPACITY) {
        return BOOT_CHAIN_COMPONENT_LIMIT;
    }
    for (index = 24U; index < BOOT_CHAIN_MANIFEST_HEADER_SIZE; ++index) {
        if (manifest[index] != 0U) {
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
    }

    report->component_count = (size_t)declared_count;
    for (index = 0U; index < report->component_count; ++index) {
        struct boot_chain_component_record *record =
            &report->components[index];

        entry = manifest + BOOT_CHAIN_MANIFEST_HEADER_SIZE +
                (index * BOOT_CHAIN_COMPONENT_ENTRY_SIZE);
        record->kind = boot_chain_read_u32(entry);
        if (!boot_chain_component_kind_valid(record->kind)) {
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
        if (boot_chain_read_u32(entry + 4U) != 0U) {
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
        record->declared_length = boot_chain_read_u64(entry + 8U);
        if (record->declared_length == 0U) {
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
        memcpy(record->digest, entry + 16U, SHA256_DIGEST_SIZE);
        record->matched = false;
        if (index > 0U &&
            report->components[index - 1U].kind >= record->kind) {
            /* Declared components are strictly ordered by kind, so a
             * manifest can never declare the same component twice. */
            return BOOT_CHAIN_MANIFEST_INVALID;
        }
    }
    return BOOT_CHAIN_OK;
}

static const struct boot_chain_component *boot_chain_find_component(
    const struct boot_chain_component *components,
    size_t component_count,
    uint32_t kind)
{
    size_t index;

    for (index = 0U; index < component_count; ++index) {
        if (components[index].kind == kind) {
            return &components[index];
        }
    }
    return NULL;
}

static enum boot_chain_status boot_chain_check_components(
    const struct boot_chain_verify_report *report,
    const struct boot_chain_component *components,
    size_t component_count)
{
    size_t index;
    size_t other;

    if (component_count > BOOT_CHAIN_COMPONENT_CAPACITY) {
        return BOOT_CHAIN_COMPONENT_LIMIT;
    }
    for (index = 0U; index < report->component_count; ++index) {
        const struct boot_chain_component_record *record =
            &report->components[index];
        const struct boot_chain_component *component =
            boot_chain_find_component(components, component_count,
                                      record->kind);
        unsigned char digest[SHA256_DIGEST_SIZE];

        if (component == NULL || component->data == NULL ||
            component->length == 0U) {
            return BOOT_CHAIN_COMPONENT_MISSING;
        }
        if ((uint64_t)component->length != record->declared_length) {
            return BOOT_CHAIN_COMPONENT_LENGTH_MISMATCH;
        }
        if (sha256_compute(component->data, component->length, digest) !=
            SHA256_OK) {
            return BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH;
        }
        if (memcmp(digest, record->digest, SHA256_DIGEST_SIZE) != 0) {
            return BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH;
        }
    }
    /* Every supplied component must be declared: an undeclared extra
     * component is not covered by the signature. */
    for (other = 0U; other < component_count; ++other) {
        bool declared = false;

        for (index = 0U; index < report->component_count; ++index) {
            if (report->components[index].kind ==
                components[other].kind) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            return BOOT_CHAIN_COMPONENT_UNDECLARED;
        }
    }
    return BOOT_CHAIN_OK;
}

enum boot_chain_status boot_chain_verify(
    const struct verification_policy *policy,
    const struct image_signature_store *store,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const struct boot_chain_component *components,
    size_t component_count,
    struct boot_chain_verify_report *report)
{
    struct signed_metadata_layout layout;
    const unsigned char *manifest;
    enum boot_chain_status status;
    enum image_signature_status signature_status;

    if (policy == NULL || store == NULL || envelope == NULL ||
        report == NULL) {
        return BOOT_CHAIN_NULL;
    }
    memset(report, 0, sizeof(*report));
    report->version = BOOT_CHAIN_VERSION;

    if (signed_metadata_verify(policy, KEY_USE_PURPOSE_IMAGE, envelope,
                               envelope_length, &layout) !=
        SIGNED_METADATA_OK) {
        return BOOT_CHAIN_ENVELOPE_INVALID;
    }
    if (layout.metadata_length != BOOT_CHAIN_MANIFEST_SIZE ||
        layout.purpose != KEY_USE_PURPOSE_IMAGE) {
        return BOOT_CHAIN_POLICY_MISMATCH;
    }
    report->envelope_verified = true;
    manifest = (const unsigned char *)envelope + layout.metadata_offset;

    status = boot_chain_parse_manifest(manifest,
                                       (size_t)layout.metadata_length,
                                       report);
    if (status != BOOT_CHAIN_OK) {
        return status;
    }
    if (sha256_compute(manifest, BOOT_CHAIN_MANIFEST_BODY_SIZE,
                       report->chain_digest) != SHA256_OK) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }

    /* The signature covers the manifest body; the signature bytes follow
     * the body inside the metadata. */
    signature_status = image_signature_verify(
        (void *)store, report->algorithm, report->signer_id,
        report->chain_digest,
        manifest + BOOT_CHAIN_MANIFEST_BODY_SIZE,
        BOOT_CHAIN_SIGNATURE_SIZE);
    if (signature_status == IMAGE_SIGNATURE_UNAVAILABLE) {
        return BOOT_CHAIN_VERIFIER_UNAVAILABLE;
    }
    if (signature_status == IMAGE_SIGNATURE_UNKNOWN_SIGNER) {
        return BOOT_CHAIN_POLICY_MISMATCH;
    }
    if (signature_status == IMAGE_SIGNATURE_MISMATCH) {
        return BOOT_CHAIN_SIGNATURE_MISMATCH;
    }
    if (signature_status != IMAGE_SIGNATURE_OK) {
        return BOOT_CHAIN_SIGNATURE_INVALID;
    }
    report->signature_verified = true;

    if (expected_release_id != 0U &&
        report->release_id != expected_release_id) {
        return BOOT_CHAIN_RELEASE_MISMATCH;
    }
    report->release_verified = true;

    status = boot_chain_check_components(report, components,
                                         component_count);
    if (status != BOOT_CHAIN_OK) {
        return status;
    }
    {
        size_t index;

        for (index = 0U; index < report->component_count; ++index) {
            report->components[index].matched = true;
        }
    }
    report->components_verified = true;
    report->trusted = true;
    return BOOT_CHAIN_OK;
}

bool boot_chain_report_is_trusted(
    const struct boot_chain_verify_report *report)
{
    return report != NULL && report->version == BOOT_CHAIN_VERSION &&
           report->envelope_verified && report->signature_verified &&
           report->release_verified && report->components_verified &&
           report->trusted;
}

enum boot_chain_status boot_chain_measure(
    const struct boot_chain_verify_report *report,
    struct boot_measurements *measurements)
{
    size_t index;

    if (report == NULL || measurements == NULL) {
        return BOOT_CHAIN_NULL;
    }
    if (!boot_chain_report_is_trusted(report)) {
        return BOOT_CHAIN_MANIFEST_INVALID;
    }
    if (boot_measurements_record(measurements, BOOT_CHAIN_CHAIN_REGISTER,
                                 0U, 0U,
                                 report->chain_digest) !=
        BOOT_MEASUREMENTS_OK) {
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }
    for (index = 0U; index < report->component_count; ++index) {
        const struct boot_chain_component_record *record =
            &report->components[index];

        if (record->kind >= BOOT_MEASUREMENTS_REGISTER_COUNT) {
            continue;
        }
        if (boot_measurements_record(measurements, record->kind,
                                     record->kind,
                                     record->declared_length,
                                     record->digest) !=
            BOOT_MEASUREMENTS_OK) {
            return BOOT_CHAIN_MEASUREMENT_FAILED;
        }
    }
    if (boot_measurements_publish(measurements) != BOOT_MEASUREMENTS_OK) {
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }
    return BOOT_CHAIN_OK;
}

const char *boot_chain_status_string(enum boot_chain_status status)
{
    if ((size_t)status >=
        sizeof(boot_chain_status_names) /
            sizeof(boot_chain_status_names[0])) {
        return "UNKNOWN";
    }
    return boot_chain_status_names[status];
}

const char *boot_chain_component_kind_string(uint32_t kind)
{
    switch (kind) {
    case BOOT_CHAIN_COMPONENT_BOOT_PAYLOAD:
        return "BOOT_PAYLOAD";
    case BOOT_CHAIN_COMPONENT_LOADER_CONFIG:
        return "LOADER_CONFIG";
    case BOOT_CHAIN_COMPONENT_KERNEL:
        return "KERNEL";
    default:
        return "UNKNOWN";
    }
}
