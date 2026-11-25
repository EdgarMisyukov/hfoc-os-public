#include "installer_media_verification.h"

#include "signed_metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char installer_media_magic[
    INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE] =
    INSTALLER_MEDIA_MAGIC_BYTES;

static void installer_media_store_le32(unsigned char *bytes,
                                       uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void installer_media_store_le64(unsigned char *bytes,
                                       uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }
}

static uint32_t installer_media_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t installer_media_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static bool installer_media_bytes_equal(const unsigned char *left,
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

static bool installer_media_zero_range(const unsigned char *bytes,
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

static enum installer_media_verification_status
installer_media_map_envelope_status(enum signed_metadata_status status)
{
    switch (status) {
    case SIGNED_METADATA_OK:
        return INSTALLER_MEDIA_OK;
    case SIGNED_METADATA_NO_POLICY:
    case SIGNED_METADATA_UNKNOWN_SIGNER:
    case SIGNED_METADATA_INVALID_PURPOSE:
    case SIGNED_METADATA_PURPOSE_DENIED:
        return INSTALLER_MEDIA_POLICY_MISMATCH;
    case SIGNED_METADATA_NULL:
        return INSTALLER_MEDIA_NULL;
    default:
        return INSTALLER_MEDIA_ENVELOPE_INVALID;
    }
}

static enum installer_media_verification_status
installer_media_check_manifest(
    const unsigned char *metadata,
    size_t metadata_length,
    struct installer_media_verification_report *report,
    size_t *signature_offset,
    size_t *signature_length)
{
    enum trust_anchor_algorithm algorithm;
    uint32_t release_id;
    uint64_t media_length;
    uint64_t source_length;
    uint32_t declared_signature_length;

    if (metadata_length < INSTALLER_MEDIA_MANIFEST_MIN_LENGTH ||
        metadata_length > INSTALLER_MEDIA_MANIFEST_MAX_LENGTH) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    if (!installer_media_bytes_equal(
            metadata, installer_media_magic,
            INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE) ||
        metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE] !=
            INSTALLER_MEDIA_MANIFEST_FORMAT_VERSION ||
        !installer_media_zero_range(
            metadata, INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE + 2U, 2U)) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    algorithm = (enum trust_anchor_algorithm)
        metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE + 1U];
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ED25519 &&
        algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED;
    }
    release_id = installer_media_load_le32(metadata + 12U);
    if (release_id < INSTALLER_MEDIA_RELEASE_ID_MIN) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    media_length = installer_media_load_le64(metadata + 16U);
    if (media_length == 0U) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    source_length = installer_media_load_le64(metadata + 56U);
    if (source_length == 0U) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    declared_signature_length =
        (uint32_t)metadata_length -
        INSTALLER_MEDIA_MANIFEST_HEADER_SIZE;
    if (declared_signature_length < INSTALLER_MEDIA_SIGNATURE_MIN ||
        declared_signature_length > INSTALLER_MEDIA_SIGNATURE_MAX) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }

    report->algorithm = algorithm;
    report->release_id = release_id;
    report->media_length = media_length;
    report->source_length = source_length;
    memcpy(report->media_digest, metadata + 24U, SHA256_DIGEST_SIZE);
    memcpy(report->source_digest, metadata + 64U, SHA256_DIGEST_SIZE);
    *signature_offset = INSTALLER_MEDIA_MANIFEST_HEADER_SIZE;
    *signature_length = declared_signature_length;
    return INSTALLER_MEDIA_OK;
}

enum installer_media_verification_status
installer_media_source_digest(
    const struct installer_media_source *source,
    unsigned char digest[SHA256_DIGEST_SIZE],
    uint64_t *length_out)
{
    struct sha256 context;
    uint64_t total = 0U;

    if (source == NULL || digest == NULL || length_out == NULL) {
        return INSTALLER_MEDIA_NULL;
    }
    if (source->boot_payload == NULL ||
        source->boot_payload_size == 0U ||
        source->limine_config == NULL ||
        source->limine_config_size == 0U ||
        source->kernel == NULL ||
        source->kernel_size == 0U) {
        return INSTALLER_MEDIA_SOURCE_INVALID;
    }
    memset(&context, 0, sizeof(context));
    if (sha256_init(&context) != SHA256_OK ||
        sha256_update(&context, source->boot_payload,
                      source->boot_payload_size) != SHA256_OK ||
        sha256_update(&context, source->limine_config,
                      source->limine_config_size) != SHA256_OK ||
        sha256_update(&context, source->kernel,
                      source->kernel_size) != SHA256_OK ||
        sha256_final(&context, digest) != SHA256_OK) {
        return INSTALLER_MEDIA_SOURCE_INVALID;
    }
    total = (uint64_t)source->boot_payload_size;
    total += (uint64_t)source->limine_config_size;
    total += (uint64_t)source->kernel_size;
    *length_out = total;
    return INSTALLER_MEDIA_OK;
}

enum installer_media_verification_status
installer_media_manifest_seal(
    const struct verification_policy *policy,
    uint32_t release_id,
    uint32_t signer_id,
    enum trust_anchor_algorithm algorithm,
    const unsigned char media_digest[SHA256_DIGEST_SIZE],
    uint64_t media_length,
    const unsigned char source_digest[SHA256_DIGEST_SIZE],
    uint64_t source_length,
    const unsigned char *signature,
    size_t signature_length,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char metadata[INSTALLER_MEDIA_MANIFEST_MAX_LENGTH];
    size_t metadata_length;
    enum signed_metadata_status envelope_status;

    if (policy == NULL || media_digest == NULL ||
        source_digest == NULL || signature == NULL ||
        buffer == NULL || length_out == NULL) {
        return INSTALLER_MEDIA_NULL;
    }
    if (release_id < INSTALLER_MEDIA_RELEASE_ID_MIN ||
        media_length == 0U || source_length == 0U ||
        signature_length < INSTALLER_MEDIA_SIGNATURE_MIN ||
        signature_length > INSTALLER_MEDIA_SIGNATURE_MAX ||
        (algorithm != TRUST_ANCHOR_ALGORITHM_ED25519 &&
         algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256)) {
        return INSTALLER_MEDIA_MANIFEST_INVALID;
    }
    metadata_length =
        INSTALLER_MEDIA_MANIFEST_HEADER_SIZE + signature_length;
    if (capacity < metadata_length) {
        return INSTALLER_MEDIA_CAPACITY;
    }
    memset(metadata, 0, metadata_length);
    memcpy(metadata, installer_media_magic,
           INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE);
    metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE] =
        INSTALLER_MEDIA_MANIFEST_FORMAT_VERSION;
    metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE + 1U] =
        (unsigned char)algorithm;
    installer_media_store_le32(metadata + 12U, release_id);
    installer_media_store_le64(metadata + 16U, media_length);
    memcpy(metadata + 24U, media_digest, SHA256_DIGEST_SIZE);
    installer_media_store_le64(metadata + 56U, source_length);
    memcpy(metadata + 64U, source_digest, SHA256_DIGEST_SIZE);
    memcpy(metadata + INSTALLER_MEDIA_MANIFEST_HEADER_SIZE,
           signature, signature_length);

    envelope_status = signed_metadata_seal(
        policy, KEY_USE_PURPOSE_IMAGE, signer_id, metadata,
        metadata_length, buffer, capacity, length_out);
    switch (envelope_status) {
    case SIGNED_METADATA_OK:
        return INSTALLER_MEDIA_OK;
    case SIGNED_METADATA_BUFFER_TOO_SMALL:
        return INSTALLER_MEDIA_CAPACITY;
    default:
        return installer_media_map_envelope_status(envelope_status);
    }
}

enum installer_media_verification_status
installer_media_verification_run(
    const struct verification_policy *policy,
    const struct installer_media_verifier *verifier,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const void *media,
    size_t media_length,
    const struct installer_media_source *source,
    struct installer_media_verification_report *report)
{
    struct signed_metadata_layout layout;
    const unsigned char *metadata;
    enum signed_metadata_status envelope_status;
    enum installer_media_verification_status status;
    unsigned char digest[SHA256_DIGEST_SIZE];
    uint64_t source_length = 0U;
    size_t signature_offset = 0U;
    size_t signature_length = 0U;

    if (report == NULL) {
        return INSTALLER_MEDIA_NULL;
    }
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_MEDIA_VERIFICATION_VERSION;
    if (policy == NULL || envelope == NULL || media == NULL ||
        media_length == 0U || source == NULL ||
        expected_release_id < INSTALLER_MEDIA_RELEASE_ID_MIN) {
        return INSTALLER_MEDIA_NULL;
    }

    memset(&layout, 0, sizeof(layout));
    envelope_status = signed_metadata_verify(
        policy, KEY_USE_PURPOSE_IMAGE, envelope, envelope_length,
        &layout);
    if (envelope_status != SIGNED_METADATA_OK) {
        return installer_media_map_envelope_status(envelope_status);
    }
    report->envelope_verified = true;
    report->signer_id = layout.signer_id;

    metadata = (const unsigned char *)envelope + layout.metadata_offset;
    status = installer_media_check_manifest(
        metadata, layout.metadata_length, report,
        &signature_offset, &signature_length);
    if (status != INSTALLER_MEDIA_OK) {
        return status;
    }

    if (verifier == NULL || verifier->verify == NULL) {
        return INSTALLER_MEDIA_VERIFIER_UNAVAILABLE;
    }
    switch (verifier->verify(
                verifier->context, report->algorithm,
                report->signer_id, report->media_digest,
                metadata + signature_offset, signature_length)) {
    case INSTALLER_MEDIA_SIGNATURE_OK:
        report->signature_verified = true;
        break;
    case INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM:
        return INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED;
    case INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE:
        return INSTALLER_MEDIA_VERIFIER_UNAVAILABLE;
    default:
        return INSTALLER_MEDIA_SIGNATURE_INVALID;
    }

    if (media_length != report->media_length) {
        return INSTALLER_MEDIA_LENGTH_MISMATCH;
    }
    report->length_verified = true;
    if (sha256_compute(media, media_length, digest) != SHA256_OK) {
        return INSTALLER_MEDIA_DIGEST_MISMATCH;
    }
    if (!installer_media_bytes_equal(
            digest, report->media_digest, SHA256_DIGEST_SIZE)) {
        return INSTALLER_MEDIA_DIGEST_MISMATCH;
    }
    report->digest_verified = true;

    status = installer_media_source_digest(
        source, digest, &source_length);
    if (status != INSTALLER_MEDIA_OK) {
        return status;
    }
    if (source_length != report->source_length) {
        return INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH;
    }
    if (!installer_media_bytes_equal(
            digest, report->source_digest, SHA256_DIGEST_SIZE)) {
        return INSTALLER_MEDIA_SOURCE_DIGEST_MISMATCH;
    }
    report->source_verified = true;

    if (report->release_id != expected_release_id) {
        return INSTALLER_MEDIA_RELEASE_MISMATCH;
    }
    report->release_verified = true;
    report->write_authorized = true;
    return INSTALLER_MEDIA_OK;
}

bool installer_media_verification_authorizes_write(
    const struct installer_media_verification_report *report)
{
    return report != NULL &&
           report->version == INSTALLER_MEDIA_VERIFICATION_VERSION &&
           report->envelope_verified &&
           report->signature_verified &&
           report->digest_verified &&
           report->release_verified &&
           report->length_verified &&
           report->source_verified &&
           report->write_authorized;
}

const char *installer_media_verification_status_string(
    enum installer_media_verification_status status)
{
    switch (status) {
    case INSTALLER_MEDIA_OK:
        return "INSTALLER_MEDIA_OK";
    case INSTALLER_MEDIA_NULL:
        return "INSTALLER_MEDIA_NULL";
    case INSTALLER_MEDIA_BAD_VERSION:
        return "INSTALLER_MEDIA_BAD_VERSION";
    case INSTALLER_MEDIA_ENVELOPE_INVALID:
        return "INSTALLER_MEDIA_ENVELOPE_INVALID";
    case INSTALLER_MEDIA_POLICY_MISMATCH:
        return "INSTALLER_MEDIA_POLICY_MISMATCH";
    case INSTALLER_MEDIA_MANIFEST_INVALID:
        return "INSTALLER_MEDIA_MANIFEST_INVALID";
    case INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED:
        return "INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED";
    case INSTALLER_MEDIA_VERIFIER_UNAVAILABLE:
        return "INSTALLER_MEDIA_VERIFIER_UNAVAILABLE";
    case INSTALLER_MEDIA_SIGNATURE_INVALID:
        return "INSTALLER_MEDIA_SIGNATURE_INVALID";
    case INSTALLER_MEDIA_DIGEST_MISMATCH:
        return "INSTALLER_MEDIA_DIGEST_MISMATCH";
    case INSTALLER_MEDIA_LENGTH_MISMATCH:
        return "INSTALLER_MEDIA_LENGTH_MISMATCH";
    case INSTALLER_MEDIA_RELEASE_MISMATCH:
        return "INSTALLER_MEDIA_RELEASE_MISMATCH";
    case INSTALLER_MEDIA_SOURCE_INVALID:
        return "INSTALLER_MEDIA_SOURCE_INVALID";
    case INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH:
        return "INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH";
    case INSTALLER_MEDIA_SOURCE_DIGEST_MISMATCH:
        return "INSTALLER_MEDIA_SOURCE_DIGEST_MISMATCH";
    case INSTALLER_MEDIA_CAPACITY:
        return "INSTALLER_MEDIA_CAPACITY";
    default:
        return NULL;
    }
}

const char *installer_media_signature_status_string(
    enum installer_media_signature_status status)
{
    switch (status) {
    case INSTALLER_MEDIA_SIGNATURE_OK:
        return "INSTALLER_MEDIA_SIGNATURE_OK";
    case INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM:
        return "INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM";
    case INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE:
        return "INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE";
    case INSTALLER_MEDIA_SIGNATURE_MISMATCH:
        return "INSTALLER_MEDIA_SIGNATURE_MISMATCH";
    default:
        return NULL;
    }
}
