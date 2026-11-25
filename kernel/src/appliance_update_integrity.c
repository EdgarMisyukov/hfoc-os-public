#include "appliance_update_integrity.h"

#include "key_use.h"
#include "signed_metadata.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool appliance_update_integrity_mem_equal(
    const unsigned char *left,
    const unsigned char *right,
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

static size_t appliance_update_integrity_strlen(
    const char *text,
    size_t maximum)
{
    size_t length = 0U;

    if (text == NULL) {
        return maximum + 1U;
    }
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    if (text[length] != '\0') {
        return maximum + 1U;
    }
    return length;
}

static bool appliance_update_integrity_text_equal(
    const char *text,
    const char *expected)
{
    size_t index = 0U;

    while (expected[index] != '\0') {
        if (text[index] != expected[index]) {
            return false;
        }
        ++index;
    }
    return text[index] == '\0';
}

static bool appliance_update_integrity_lower_hex_text(
    const char *text,
    size_t length,
    size_t expected_length)
{
    size_t index;

    if (length != expected_length) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if ((value < (unsigned char)'0' || value > (unsigned char)'9') &&
            (value < (unsigned char)'a' || value > (unsigned char)'f')) {
            return false;
        }
    }
    return true;
}

static bool appliance_update_integrity_manifest_valid(
    const struct appliance_update_manifest *manifest)
{
    const size_t release_length = appliance_update_integrity_strlen(
        manifest->release_id, sizeof(manifest->release_id));
    const size_t architecture_length = appliance_update_integrity_strlen(
        manifest->architecture, sizeof(manifest->architecture));
    const size_t revision_length = appliance_update_integrity_strlen(
        manifest->source_revision, sizeof(manifest->source_revision));
    const size_t metadata_digest_length =
        appliance_update_integrity_strlen(
            manifest->metadata_digest,
            sizeof(manifest->metadata_digest));
    const size_t payload_digest_length =
        appliance_update_integrity_strlen(
            manifest->payload_digest,
            sizeof(manifest->payload_digest));

    if (manifest == NULL ||
        manifest->update_package_version !=
            APPLIANCE_UPDATE_MANIFEST_VERSION ||
        release_length != RELEASE_PROFILE_ID_LENGTH ||
        !appliance_update_integrity_text_equal(
            manifest->release_id, RELEASE_PROFILE_ID) ||
        architecture_length != RELEASE_PROFILE_ARCH_LENGTH ||
        !appliance_update_integrity_text_equal(
            manifest->architecture, RELEASE_PROFILE_ARCH) ||
        !appliance_update_integrity_lower_hex_text(
            manifest->source_revision,
            revision_length,
            APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH) ||
        !appliance_update_integrity_lower_hex_text(
            manifest->metadata_digest,
            metadata_digest_length,
            APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH) ||
        !appliance_update_integrity_lower_hex_text(
            manifest->payload_digest,
            payload_digest_length,
            APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH) ||
        update_compatibility_manifest_validate(
            &manifest->compatibility) != UPDATE_COMPATIBILITY_OK) {
        return false;
    }
    return true;
}

static void appliance_update_integrity_zero_bytes(
    unsigned char *destination,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
}

static void appliance_update_integrity_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_update_integrity_hex_value(
    unsigned char value,
    unsigned char *nibble_out)
{
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        *nibble_out = (unsigned char)(value - (unsigned char)'0');
        return true;
    }
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f') {
        *nibble_out = (unsigned char)(
            value - (unsigned char)'a' + 10U);
        return true;
    }
    return false;
}

static bool appliance_update_integrity_hex_to_bytes(
    const char *hex,
    size_t hex_length,
    unsigned char *bytes_out,
    size_t expected_bytes)
{
    size_t index;

    if (hex_length != expected_bytes * 2U) {
        return false;
    }
    for (index = 0U; index < expected_bytes; ++index) {
        unsigned char high;
        unsigned char low;

        if (!appliance_update_integrity_hex_value(
                (unsigned char)hex[index * 2U], &high) ||
            !appliance_update_integrity_hex_value(
                (unsigned char)hex[index * 2U + 1U], &low)) {
            return false;
        }
        bytes_out[index] = (unsigned char)((high << 4U) | low);
    }
    return true;
}

static enum appliance_update_integrity_status
appliance_update_integrity_hash(
    const void *data,
    size_t data_length,
    unsigned char digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    enum sha256_status hash_status;

    hash_status = sha256_init(&context);
    if (hash_status != SHA256_OK) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_STATE;
    }
    hash_status = sha256_update(&context, data, data_length);
    if (hash_status != SHA256_OK) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_STATE;
    }
    hash_status = sha256_final(&context, digest);
    if (hash_status != SHA256_OK) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_STATE;
    }
    return APPLIANCE_UPDATE_INTEGRITY_OK;
}

static enum appliance_update_integrity_status
appliance_update_integrity_verify_hex_digest(
    const char *expected_hex,
    const unsigned char *measured,
    size_t digest_length)
{
    unsigned char expected[SHA256_DIGEST_SIZE];

    if (!appliance_update_integrity_hex_to_bytes(
            expected_hex,
            APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH,
            expected,
            digest_length) ||
        !appliance_update_integrity_mem_equal(
            expected, measured, digest_length)) {
        return APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH;
    }
    return APPLIANCE_UPDATE_INTEGRITY_OK;
}

static enum appliance_update_integrity_status
appliance_update_integrity_map_signed_metadata(
    enum signed_metadata_status status)
{
    switch (status) {
    case SIGNED_METADATA_OK:
        return APPLIANCE_UPDATE_INTEGRITY_OK;
    case SIGNED_METADATA_NO_POLICY:
        return APPLIANCE_UPDATE_INTEGRITY_NO_POLICY;
    case SIGNED_METADATA_UNKNOWN_SIGNER:
        return APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER;
    case SIGNED_METADATA_BAD_SIGNATURE:
        return APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE;
    case SIGNED_METADATA_PURPOSE_DENIED:
        return APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED;
    default:
        return APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE;
    }
}

enum appliance_update_integrity_status appliance_update_integrity_init(
    struct appliance_update_integrity_gate *gate)
{
    if (gate == NULL) {
        return APPLIANCE_UPDATE_INTEGRITY_NULL;
    }
    if (gate->version == APPLIANCE_UPDATE_INTEGRITY_VERSION ||
        gate->verified) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_STATE;
    }
    appliance_update_integrity_zero_bytes(
        (unsigned char *)gate, sizeof(*gate));
    gate->version = APPLIANCE_UPDATE_INTEGRITY_VERSION;
    return APPLIANCE_UPDATE_INTEGRITY_OK;
}

enum appliance_update_integrity_status appliance_update_integrity_verify(
    struct appliance_update_integrity_gate *gate,
    const struct appliance_update_manifest *manifest,
    const struct verification_policy *policy,
    const void *metadata,
    size_t metadata_length,
    const void *payload,
    size_t payload_length,
    const void *envelope,
    size_t envelope_length)
{
    const struct verification_policy_snapshot *active;
    const unsigned char *envelope_bytes = (const unsigned char *)envelope;
    unsigned char metadata_digest[SHA256_DIGEST_SIZE];
    unsigned char payload_digest[SHA256_DIGEST_SIZE];
    unsigned char payload_binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct signed_metadata_layout layout;
    struct appliance_update_integrity_gate draft;
    enum appliance_update_integrity_status integrity_status;
    enum signed_metadata_status signed_status;
    size_t index;

    if (gate == NULL || manifest == NULL || policy == NULL ||
        metadata == NULL || payload == NULL || envelope == NULL) {
        return APPLIANCE_UPDATE_INTEGRITY_NULL;
    }
    if (gate->version != APPLIANCE_UPDATE_INTEGRITY_VERSION ||
        gate->verified) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_STATE;
    }
    if (!appliance_update_integrity_manifest_valid(manifest)) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return APPLIANCE_UPDATE_INTEGRITY_NO_POLICY;
    }
    if (metadata_length == 0U ||
        metadata_length > APPLIANCE_UPDATE_INTEGRITY_METADATA_MAX) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA;
    }
    if (payload_length == 0U ||
        payload_length > APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_MAX) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD;
    }

    integrity_status = appliance_update_integrity_hash(
        metadata, metadata_length, metadata_digest);
    if (integrity_status != APPLIANCE_UPDATE_INTEGRITY_OK) {
        return integrity_status;
    }
    if (appliance_update_integrity_verify_hex_digest(
            manifest->metadata_digest,
            metadata_digest,
            SHA256_DIGEST_SIZE) != APPLIANCE_UPDATE_INTEGRITY_OK) {
        return APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH;
    }

    integrity_status = appliance_update_integrity_hash(
        payload, payload_length, payload_digest);
    if (integrity_status != APPLIANCE_UPDATE_INTEGRITY_OK) {
        return integrity_status;
    }
    if (appliance_update_integrity_verify_hex_digest(
            manifest->payload_digest,
            payload_digest,
            SHA256_DIGEST_SIZE) != APPLIANCE_UPDATE_INTEGRITY_OK) {
        return APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH;
    }

    signed_status = signed_metadata_verify(
        policy, KEY_USE_PURPOSE_UPDATE,
        envelope, envelope_length, &layout);
    integrity_status =
        appliance_update_integrity_map_signed_metadata(signed_status);
    if (integrity_status != APPLIANCE_UPDATE_INTEGRITY_OK) {
        return integrity_status;
    }

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        payload_binding[index] = metadata_digest[index];
        payload_binding[SHA256_DIGEST_SIZE + index] =
            payload_digest[index];
    }
    if (!layout.valid ||
        layout.metadata_length !=
            APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE ||
        !appliance_update_integrity_mem_equal(
            envelope_bytes + layout.metadata_offset,
            payload_binding,
            APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE)) {
        return APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE;
    }

    appliance_update_integrity_zero_bytes(
        (unsigned char *)&draft, sizeof(draft));
    draft.version = APPLIANCE_UPDATE_INTEGRITY_VERSION;
    draft.verified = true;
    draft.policy_generation = active->generation;
    draft.signer_id = layout.signer_id;
    draft.metadata_length = (uint32_t)metadata_length;
    draft.payload_length = (uint32_t)payload_length;
    appliance_update_integrity_copy_bytes(
        draft.metadata_digest, metadata_digest,
        SHA256_DIGEST_SIZE);
    appliance_update_integrity_copy_bytes(
        draft.payload_digest, payload_digest,
        SHA256_DIGEST_SIZE);
    *gate = draft;
    return APPLIANCE_UPDATE_INTEGRITY_OK;
}

bool appliance_update_integrity_is_verified(
    const struct appliance_update_integrity_gate *gate)
{
    return gate != NULL &&
           gate->version == APPLIANCE_UPDATE_INTEGRITY_VERSION &&
           gate->verified;
}

const char *appliance_update_integrity_status_string(
    enum appliance_update_integrity_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_INTEGRITY_OK:
        return "APPLIANCE_UPDATE_INTEGRITY_OK";
    case APPLIANCE_UPDATE_INTEGRITY_NULL:
        return "APPLIANCE_UPDATE_INTEGRITY_NULL";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_STATE:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_STATE";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST";
    case APPLIANCE_UPDATE_INTEGRITY_NO_POLICY:
        return "APPLIANCE_UPDATE_INTEGRITY_NO_POLICY";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD";
    case APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH:
        return "APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH";
    case APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH:
        return "APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE";
    case APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER:
        return "APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER";
    case APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE:
        return "APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE";
    case APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED:
        return "APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED";
    default:
        return NULL;
    }
}
