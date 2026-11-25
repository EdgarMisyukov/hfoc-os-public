#include "integration_envelope.h"

#include "sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char integration_envelope_id[
    INTEGRATION_ENVELOPE_ID_SIZE] = INTEGRATION_ENVELOPE_ID_BYTES;

static void integration_envelope_store_le32(unsigned char *bytes,
                                            uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static uint32_t integration_envelope_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static bool integration_envelope_bytes_equal(
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

static bool integration_envelope_zero_range(
    const unsigned char *bytes,
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

static bool integration_envelope_correlation_all_zero(
    const uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE])
{
    return integration_envelope_zero_range(
        correlation_id, 0U, INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE);
}

enum integration_envelope_status integration_envelope_seal(
    uint32_t schema_version,
    uint32_t purpose,
    const uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE],
    const void *payload,
    size_t payload_length,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    unsigned char *bytes;
    unsigned char digest[INTEGRATION_ENVELOPE_DIGEST_SIZE];
    size_t digest_offset;
    size_t envelope_length;

    if (correlation_id == NULL || payload == NULL ||
        buffer == NULL || length_out == NULL) {
        return INTEGRATION_ENVELOPE_NULL;
    }
    if (schema_version < INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN ||
        schema_version > INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX) {
        return INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION;
    }
    if (purpose < INTEGRATION_ENVELOPE_PURPOSE_MIN ||
        purpose > INTEGRATION_ENVELOPE_PURPOSE_MAX) {
        return INTEGRATION_ENVELOPE_INVALID_PURPOSE;
    }
    if (integration_envelope_correlation_all_zero(correlation_id)) {
        return INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID;
    }
    if (payload_length < INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MIN ||
        payload_length > INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX) {
        return INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH;
    }

    digest_offset = INTEGRATION_ENVELOPE_HEADER_SIZE + payload_length;
    envelope_length = digest_offset + INTEGRATION_ENVELOPE_DIGEST_SIZE;
    if (capacity < envelope_length) {
        return INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL;
    }
    bytes = (unsigned char *)buffer;
    memset(bytes, 0, INTEGRATION_ENVELOPE_HEADER_SIZE);
    memcpy(bytes, integration_envelope_id,
           INTEGRATION_ENVELOPE_ID_SIZE);
    bytes[INTEGRATION_ENVELOPE_ID_SIZE] = INTEGRATION_ENVELOPE_VERSION;
    integration_envelope_store_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 4U, schema_version);
    integration_envelope_store_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 8U, purpose);
    integration_envelope_store_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 12U,
        (uint32_t)payload_length);
    memcpy(bytes + INTEGRATION_ENVELOPE_ID_SIZE + 16U,
           correlation_id, INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE);
    memcpy(bytes + INTEGRATION_ENVELOPE_HEADER_SIZE,
           payload, payload_length);
    if (sha256_compute(bytes, digest_offset, digest) != SHA256_OK) {
        return INTEGRATION_ENVELOPE_BAD_STATE;
    }
    memcpy(bytes + digest_offset, digest,
           INTEGRATION_ENVELOPE_DIGEST_SIZE);
    *length_out = envelope_length;
    return INTEGRATION_ENVELOPE_OK;
}

enum integration_envelope_status integration_envelope_validate(
    const void *envelope,
    size_t envelope_length,
    struct integration_envelope_layout *layout)
{
    const unsigned char *bytes;
    unsigned char digest[INTEGRATION_ENVELOPE_DIGEST_SIZE];
    uint32_t envelope_version;
    uint32_t schema_version;
    uint32_t purpose;
    uint32_t payload_length;
    size_t digest_offset;
    size_t expected_length;

    if (envelope == NULL || layout == NULL) {
        return INTEGRATION_ENVELOPE_NULL;
    }
    if (envelope_length < INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH) {
        return INTEGRATION_ENVELOPE_TOO_SHORT;
    }
    if (envelope_length > INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH) {
        return INTEGRATION_ENVELOPE_TOO_LONG;
    }

    bytes = (const unsigned char *)envelope;
    if (!integration_envelope_bytes_equal(
            bytes, integration_envelope_id,
            INTEGRATION_ENVELOPE_ID_SIZE)) {
        return INTEGRATION_ENVELOPE_BAD_ID;
    }
    envelope_version = bytes[INTEGRATION_ENVELOPE_ID_SIZE];
    if (envelope_version != INTEGRATION_ENVELOPE_VERSION) {
        return INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION;
    }
    schema_version = integration_envelope_load_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 4U);
    if (schema_version < INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN ||
        schema_version > INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX) {
        return INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION;
    }
    purpose = integration_envelope_load_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 8U);
    if (purpose < INTEGRATION_ENVELOPE_PURPOSE_MIN ||
        purpose > INTEGRATION_ENVELOPE_PURPOSE_MAX) {
        return INTEGRATION_ENVELOPE_INVALID_PURPOSE;
    }
    if (!integration_envelope_zero_range(
            bytes, INTEGRATION_ENVELOPE_HEADER_SIZE -
                       INTEGRATION_ENVELOPE_RESERVED_SIZE,
            INTEGRATION_ENVELOPE_RESERVED_SIZE)) {
        return INTEGRATION_ENVELOPE_CORRUPT_RESERVED;
    }

    payload_length = integration_envelope_load_le32(
        bytes + INTEGRATION_ENVELOPE_ID_SIZE + 12U);
    if (payload_length < INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MIN ||
        payload_length > INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX) {
        return INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH;
    }
    digest_offset = INTEGRATION_ENVELOPE_HEADER_SIZE + payload_length;
    expected_length = digest_offset + INTEGRATION_ENVELOPE_DIGEST_SIZE;
    if (envelope_length != expected_length) {
        return INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH;
    }
    if (integration_envelope_correlation_all_zero(
            bytes + INTEGRATION_ENVELOPE_ID_SIZE + 16U)) {
        return INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID;
    }
    if (sha256_compute(bytes, digest_offset, digest) != SHA256_OK ||
        !integration_envelope_bytes_equal(
            digest, bytes + digest_offset,
            INTEGRATION_ENVELOPE_DIGEST_SIZE)) {
        return INTEGRATION_ENVELOPE_BAD_DIGEST;
    }

    layout->valid = true;
    layout->envelope_version = envelope_version;
    layout->schema_version = schema_version;
    layout->purpose = purpose;
    layout->payload_length = payload_length;
    layout->payload_offset = INTEGRATION_ENVELOPE_HEADER_SIZE;
    layout->digest_offset = digest_offset;
    layout->envelope_length = envelope_length;
    return INTEGRATION_ENVELOPE_OK;
}

const char *integration_envelope_status_string(
    enum integration_envelope_status status)
{
    switch (status) {
    case INTEGRATION_ENVELOPE_OK:
        return "INTEGRATION_ENVELOPE_OK";
    case INTEGRATION_ENVELOPE_NULL:
        return "INTEGRATION_ENVELOPE_NULL";
    case INTEGRATION_ENVELOPE_TOO_SHORT:
        return "INTEGRATION_ENVELOPE_TOO_SHORT";
    case INTEGRATION_ENVELOPE_TOO_LONG:
        return "INTEGRATION_ENVELOPE_TOO_LONG";
    case INTEGRATION_ENVELOPE_BAD_ID:
        return "INTEGRATION_ENVELOPE_BAD_ID";
    case INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION:
        return "INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION";
    case INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION:
        return "INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION";
    case INTEGRATION_ENVELOPE_INVALID_PURPOSE:
        return "INTEGRATION_ENVELOPE_INVALID_PURPOSE";
    case INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID:
        return "INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID";
    case INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH:
        return "INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH";
    case INTEGRATION_ENVELOPE_CORRUPT_RESERVED:
        return "INTEGRATION_ENVELOPE_CORRUPT_RESERVED";
    case INTEGRATION_ENVELOPE_BAD_DIGEST:
        return "INTEGRATION_ENVELOPE_BAD_DIGEST";
    case INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL:
        return "INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL";
    case INTEGRATION_ENVELOPE_BAD_STATE:
        return "INTEGRATION_ENVELOPE_BAD_STATE";
    default:
        return NULL;
    }
}
