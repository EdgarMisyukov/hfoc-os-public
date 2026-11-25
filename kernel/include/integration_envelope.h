#ifndef HFOC_INTEGRATION_ENVELOPE_H
#define HFOC_INTEGRATION_ENVELOPE_H

#include "integration_boundary.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTEGRATION_ENVELOPE_VERSION 1U
#define INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN
#define INTEGRATION_ENVELOPE_SCHEMA_VERSION_MAX \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX
#define INTEGRATION_ENVELOPE_ID_SIZE 16U
#define INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE 32U
#define INTEGRATION_ENVELOPE_DIGEST_SIZE SHA256_DIGEST_SIZE
#define INTEGRATION_ENVELOPE_HEADER_SIZE 88U
#define INTEGRATION_ENVELOPE_RESERVED_SIZE 24U
#define INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MIN 1U
#define INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX 4096U
#define INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH \
    (INTEGRATION_ENVELOPE_HEADER_SIZE + INTEGRATION_ENVELOPE_DIGEST_SIZE)
#define INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH \
    (INTEGRATION_ENVELOPE_HEADER_SIZE + \
     INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX + \
     INTEGRATION_ENVELOPE_DIGEST_SIZE)
#define INTEGRATION_ENVELOPE_PURPOSE_MIN 1U
#define INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF 1U
#define INTEGRATION_ENVELOPE_PURPOSE_MAX 1U
#define INTEGRATION_ENVELOPE_ID_BYTES \
    { 'H', 'F', 'O', 'C', 'A', 'C', 'C', 'T', \
      'H', 'A', 'N', 'D', 'O', 'F', 'F', '1' }

enum integration_envelope_status {
    INTEGRATION_ENVELOPE_OK = 0,
    INTEGRATION_ENVELOPE_NULL = 1,
    INTEGRATION_ENVELOPE_TOO_SHORT = 2,
    INTEGRATION_ENVELOPE_TOO_LONG = 3,
    INTEGRATION_ENVELOPE_BAD_ID = 4,
    INTEGRATION_ENVELOPE_UNSUPPORTED_ENVELOPE_VERSION = 5,
    INTEGRATION_ENVELOPE_UNSUPPORTED_SCHEMA_VERSION = 6,
    INTEGRATION_ENVELOPE_INVALID_PURPOSE = 7,
    INTEGRATION_ENVELOPE_INVALID_CORRELATION_ID = 8,
    INTEGRATION_ENVELOPE_INVALID_PAYLOAD_LENGTH = 9,
    INTEGRATION_ENVELOPE_CORRUPT_RESERVED = 10,
    INTEGRATION_ENVELOPE_BAD_DIGEST = 11,
    INTEGRATION_ENVELOPE_BUFFER_TOO_SMALL = 12,
    INTEGRATION_ENVELOPE_BAD_STATE = 13,
};

struct integration_envelope_layout {
    bool valid;
    uint32_t envelope_version;
    uint32_t schema_version;
    uint32_t purpose;
    uint32_t payload_length;
    size_t payload_offset;
    size_t digest_offset;
    size_t envelope_length;
};

enum integration_envelope_status integration_envelope_seal(
    uint32_t schema_version,
    uint32_t purpose,
    const uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE],
    const void *payload,
    size_t payload_length,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum integration_envelope_status integration_envelope_validate(
    const void *envelope,
    size_t envelope_length,
    struct integration_envelope_layout *layout);
const char *integration_envelope_status_string(
    enum integration_envelope_status status);

#endif
