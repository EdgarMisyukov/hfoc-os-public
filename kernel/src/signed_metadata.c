#include "signed_metadata.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const unsigned char signed_metadata_magic[
    SIGNED_METADATA_MAGIC_SIZE] = SIGNED_METADATA_MAGIC_BYTES;

static void signed_metadata_store_le32(unsigned char *bytes,
                                       uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static uint32_t signed_metadata_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static bool signed_metadata_bytes_equal(const unsigned char *left,
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

static bool signed_metadata_zero_range(const unsigned char *bytes,
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

static void signed_metadata_copy_bytes(unsigned char *destination,
                                       const unsigned char *source,
                                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static const struct trust_anchor *signed_metadata_find_anchor(
    const struct verification_policy_snapshot *policy,
    uint32_t signer_id)
{
    uint32_t index;

    if (signer_id < TRUST_ANCHOR_ID_MIN ||
        signer_id > TRUST_ANCHOR_ID_MAX) {
        return NULL;
    }
    for (index = 0U; index < policy->anchor_count; ++index) {
        const struct trust_anchor *const anchor = &policy->anchors[index];

        if (anchor->id == signer_id &&
            anchor->state == TRUST_ANCHOR_STATE_ACTIVE) {
            return anchor;
        }
    }
    return NULL;
}

enum signed_metadata_status signed_metadata_seal(
    const struct verification_policy *policy,
    uint32_t purpose,
    uint32_t signer_id,
    const void *metadata,
    size_t metadata_length,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    const struct verification_policy_snapshot *active;
    const struct trust_anchor *signer;
    unsigned char *bytes;
    size_t envelope_length;
    size_t metadata_offset = SIGNED_METADATA_HEADER_SIZE;
    size_t signature_offset;
    size_t index;

    if (policy == NULL || metadata == NULL ||
        buffer == NULL || length_out == NULL) {
        return SIGNED_METADATA_NULL;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return SIGNED_METADATA_NO_POLICY;
    }
    if (!key_use_purpose_valid(purpose)) {
        return SIGNED_METADATA_INVALID_PURPOSE;
    }
    if (metadata_length < SIGNED_METADATA_MIN_METADATA_LENGTH ||
        metadata_length > SIGNED_METADATA_MAX_METADATA_LENGTH) {
        return SIGNED_METADATA_INVALID_METADATA;
    }
    signer = signed_metadata_find_anchor(active, signer_id);
    if (signer == NULL) {
        return SIGNED_METADATA_UNKNOWN_SIGNER;
    }
    if (!key_use_purpose_is_accepted(
            signer->accepted_purposes, purpose)) {
        return SIGNED_METADATA_PURPOSE_DENIED;
    }

    envelope_length = SIGNED_METADATA_HEADER_SIZE +
                      metadata_length + SIGNED_METADATA_DIGEST_SIZE;
    if (capacity < envelope_length) {
        return SIGNED_METADATA_BUFFER_TOO_SMALL;
    }

    bytes = (unsigned char *)buffer;
    signature_offset = envelope_length - SIGNED_METADATA_DIGEST_SIZE;
    signed_metadata_copy_bytes(bytes + metadata_offset,
                               (const unsigned char *)metadata,
                               metadata_length);
    signed_metadata_copy_bytes(bytes, signed_metadata_magic,
                               SIGNED_METADATA_MAGIC_SIZE);
    bytes[SIGNED_METADATA_MAGIC_SIZE] =
        SIGNED_METADATA_FORMAT_VERSION;
    bytes[SIGNED_METADATA_MAGIC_SIZE + 1U] = (unsigned char)signer_id;
    bytes[SIGNED_METADATA_PURPOSE_OFFSET] = (unsigned char)purpose;
    for (index = SIGNED_METADATA_PURPOSE_OFFSET + 1U;
         index < 16U; ++index) {
        bytes[index] = 0U;
    }
    signed_metadata_store_le32(bytes + 16U,
                               (uint32_t)metadata_length);
    for (index = 20U; index < 32U; ++index) {
        bytes[index] = 0U;
    }
    (void)sha256_compute(
        bytes, signature_offset,
        bytes + signature_offset);
    *length_out = envelope_length;
    return SIGNED_METADATA_OK;
}

enum signed_metadata_status signed_metadata_verify(
    const struct verification_policy *policy,
    uint32_t purpose,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout)
{
    const struct verification_policy_snapshot *active;
    const struct trust_anchor *signer;
    const unsigned char *bytes;
    unsigned char digest[SIGNED_METADATA_DIGEST_SIZE];
    uint32_t envelope_purpose;
    uint32_t signer_id;
    uint32_t metadata_length;
    size_t metadata_offset = SIGNED_METADATA_HEADER_SIZE;
    size_t signature_offset;
    size_t expected_total;

    if (policy == NULL || envelope == NULL || layout == NULL) {
        return SIGNED_METADATA_NULL;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return SIGNED_METADATA_NO_POLICY;
    }
    if (!key_use_purpose_valid(purpose)) {
        return SIGNED_METADATA_INVALID_PURPOSE;
    }
    if (envelope_length < SIGNED_METADATA_MIN_ENVELOPE_LENGTH) {
        return SIGNED_METADATA_TOO_SHORT;
    }
    if (envelope_length > SIGNED_METADATA_MAX_ENVELOPE_LENGTH) {
        return SIGNED_METADATA_TOO_LONG;
    }

    bytes = (const unsigned char *)envelope;
    if (!signed_metadata_bytes_equal(
            bytes, signed_metadata_magic,
            SIGNED_METADATA_MAGIC_SIZE)) {
        return SIGNED_METADATA_CORRUPT;
    }
    if (bytes[SIGNED_METADATA_MAGIC_SIZE] !=
        SIGNED_METADATA_FORMAT_VERSION) {
        return SIGNED_METADATA_UNSUPPORTED;
    }

    signer_id = bytes[SIGNED_METADATA_MAGIC_SIZE + 1U];
    envelope_purpose = bytes[SIGNED_METADATA_PURPOSE_OFFSET];
    if (!key_use_purpose_valid(envelope_purpose)) {
        return SIGNED_METADATA_INVALID_PURPOSE;
    }
    signer = signed_metadata_find_anchor(active, signer_id);
    if (signer == NULL) {
        return SIGNED_METADATA_UNKNOWN_SIGNER;
    }
    if (!key_use_purpose_is_accepted(
            signer->accepted_purposes, envelope_purpose) ||
        envelope_purpose != purpose) {
        return SIGNED_METADATA_PURPOSE_DENIED;
    }
    if (!signed_metadata_zero_range(
            bytes, SIGNED_METADATA_PURPOSE_OFFSET + 1U, 5U) ||
        !signed_metadata_zero_range(bytes, 20U, 12U)) {
        return SIGNED_METADATA_CORRUPT;
    }

    metadata_length = signed_metadata_load_le32(bytes + 16U);
    if (metadata_length < SIGNED_METADATA_MIN_METADATA_LENGTH ||
        metadata_length > SIGNED_METADATA_MAX_METADATA_LENGTH) {
        return SIGNED_METADATA_CORRUPT;
    }
    expected_total = SIGNED_METADATA_HEADER_SIZE +
                     metadata_length + SIGNED_METADATA_DIGEST_SIZE;
    if (envelope_length != expected_total) {
        return SIGNED_METADATA_CORRUPT;
    }

    signature_offset = metadata_offset + metadata_length;
    if (sha256_compute(bytes, signature_offset, digest) != SHA256_OK ||
        !signed_metadata_bytes_equal(
            digest, bytes + signature_offset,
            SIGNED_METADATA_DIGEST_SIZE)) {
        return SIGNED_METADATA_BAD_SIGNATURE;
    }

    layout->valid = true;
    layout->purpose = envelope_purpose;
    layout->signer_id = signer_id;
    layout->metadata_length = metadata_length;
    layout->metadata_offset = metadata_offset;
    layout->signature_offset = signature_offset;
    layout->envelope_length = envelope_length;
    return SIGNED_METADATA_OK;
}

const char *signed_metadata_status_string(
    enum signed_metadata_status status)
{
    switch (status) {
    case SIGNED_METADATA_OK:
        return "SIGNED_METADATA_OK";
    case SIGNED_METADATA_NULL:
        return "SIGNED_METADATA_NULL";
    case SIGNED_METADATA_NO_POLICY:
        return "SIGNED_METADATA_NO_POLICY";
    case SIGNED_METADATA_UNSUPPORTED:
        return "SIGNED_METADATA_UNSUPPORTED";
    case SIGNED_METADATA_TOO_SHORT:
        return "SIGNED_METADATA_TOO_SHORT";
    case SIGNED_METADATA_BUFFER_TOO_SMALL:
        return "SIGNED_METADATA_BUFFER_TOO_SMALL";
    case SIGNED_METADATA_TOO_LONG:
        return "SIGNED_METADATA_TOO_LONG";
    case SIGNED_METADATA_CORRUPT:
        return "SIGNED_METADATA_CORRUPT";
    case SIGNED_METADATA_INVALID_METADATA:
        return "SIGNED_METADATA_INVALID_METADATA";
    case SIGNED_METADATA_UNKNOWN_SIGNER:
        return "SIGNED_METADATA_UNKNOWN_SIGNER";
    case SIGNED_METADATA_BAD_SIGNATURE:
        return "SIGNED_METADATA_BAD_SIGNATURE";
    case SIGNED_METADATA_INVALID_PURPOSE:
        return "SIGNED_METADATA_INVALID_PURPOSE";
    case SIGNED_METADATA_PURPOSE_DENIED:
        return "SIGNED_METADATA_PURPOSE_DENIED";
    default:
        return NULL;
    }
}
