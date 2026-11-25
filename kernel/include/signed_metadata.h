#ifndef HFOC_SIGNED_METADATA_H
#define HFOC_SIGNED_METADATA_H

#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGNED_METADATA_MAGIC_SIZE 8U
#define SIGNED_METADATA_FORMAT_VERSION 2U
#define SIGNED_METADATA_HEADER_SIZE 32U
#define SIGNED_METADATA_PURPOSE_OFFSET 10U
#define SIGNED_METADATA_DIGEST_SIZE 32U
#define SIGNED_METADATA_MIN_METADATA_LENGTH 1U
#define SIGNED_METADATA_MAX_METADATA_LENGTH 65536U
#define SIGNED_METADATA_MIN_ENVELOPE_LENGTH \
    (SIGNED_METADATA_HEADER_SIZE + SIGNED_METADATA_DIGEST_SIZE)
#define SIGNED_METADATA_MAX_ENVELOPE_LENGTH \
    (SIGNED_METADATA_HEADER_SIZE + \
     SIGNED_METADATA_MAX_METADATA_LENGTH + SIGNED_METADATA_DIGEST_SIZE)
#define SIGNED_METADATA_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'S', 'I', 'G', 'N' }

enum signed_metadata_status {
    SIGNED_METADATA_OK = 0,
    SIGNED_METADATA_NULL = 1,
    SIGNED_METADATA_NO_POLICY = 2,
    SIGNED_METADATA_UNSUPPORTED = 3,
    SIGNED_METADATA_TOO_SHORT = 4,
    SIGNED_METADATA_BUFFER_TOO_SMALL = 5,
    SIGNED_METADATA_TOO_LONG = 6,
    SIGNED_METADATA_CORRUPT = 7,
    SIGNED_METADATA_INVALID_METADATA = 8,
    SIGNED_METADATA_UNKNOWN_SIGNER = 9,
    SIGNED_METADATA_BAD_SIGNATURE = 10,
    SIGNED_METADATA_INVALID_PURPOSE = 11,
    SIGNED_METADATA_PURPOSE_DENIED = 12,
};

struct signed_metadata_layout {
    bool valid;
    uint32_t purpose;
    uint32_t signer_id;
    uint32_t metadata_length;
    size_t metadata_offset;
    size_t signature_offset;
    size_t envelope_length;
};

enum signed_metadata_status signed_metadata_seal(
    const struct verification_policy *policy,
    uint32_t purpose,
    uint32_t signer_id,
    const void *metadata,
    size_t metadata_length,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum signed_metadata_status signed_metadata_verify(
    const struct verification_policy *policy,
    uint32_t purpose,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout);
const char *signed_metadata_status_string(
    enum signed_metadata_status status);

#endif
