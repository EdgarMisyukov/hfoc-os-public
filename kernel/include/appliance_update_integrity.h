#ifndef HFOC_APPLIANCE_UPDATE_INTEGRITY_H
#define HFOC_APPLIANCE_UPDATE_INTEGRITY_H

#include "appliance_update_manifest.h"
#include "sha256.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_UPDATE_INTEGRITY_VERSION 1U
#define APPLIANCE_UPDATE_INTEGRITY_METADATA_MAX 16384U
#define APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_MAX 0x20000000U
#define APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE \
    (2U * SHA256_DIGEST_SIZE)

enum appliance_update_integrity_status {
    APPLIANCE_UPDATE_INTEGRITY_OK = 0,
    APPLIANCE_UPDATE_INTEGRITY_NULL = 1,
    APPLIANCE_UPDATE_INTEGRITY_BAD_STATE = 2,
    APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST = 3,
    APPLIANCE_UPDATE_INTEGRITY_NO_POLICY = 4,
    APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA = 5,
    APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD = 6,
    APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH = 7,
    APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH = 8,
    APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE = 9,
    APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER = 10,
    APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE = 11,
    APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED = 12,
};

struct appliance_update_integrity_gate {
    uint32_t version;
    bool verified;
    uint32_t policy_generation;
    uint32_t signer_id;
    uint32_t metadata_length;
    uint32_t payload_length;
    uint8_t metadata_digest[SHA256_DIGEST_SIZE];
    uint8_t payload_digest[SHA256_DIGEST_SIZE];
};

enum appliance_update_integrity_status appliance_update_integrity_init(
    struct appliance_update_integrity_gate *gate);
enum appliance_update_integrity_status appliance_update_integrity_verify(
    struct appliance_update_integrity_gate *gate,
    const struct appliance_update_manifest *manifest,
    const struct verification_policy *policy,
    const void *metadata,
    size_t metadata_length,
    const void *payload,
    size_t payload_length,
    const void *envelope,
    size_t envelope_length);
bool appliance_update_integrity_is_verified(
    const struct appliance_update_integrity_gate *gate);
const char *appliance_update_integrity_status_string(
    enum appliance_update_integrity_status status);

#endif
