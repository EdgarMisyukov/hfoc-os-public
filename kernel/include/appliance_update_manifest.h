#ifndef HFOC_APPLIANCE_UPDATE_MANIFEST_H
#define HFOC_APPLIANCE_UPDATE_MANIFEST_H

#include "release_profile.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_UPDATE_MANIFEST_VERSION 1U
#define APPLIANCE_UPDATE_MANIFEST_INPUT_MAX 16384U
#define APPLIANCE_UPDATE_MANIFEST_LINE_MAX 256U
#define APPLIANCE_UPDATE_MANIFEST_KEY_MAX 40U
#define APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH 40U
#define APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH 64U
#define APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_INPUT_MAX \
    UPDATE_COMPATIBILITY_INPUT_MAX

enum appliance_update_manifest_status {
    APPLIANCE_UPDATE_MANIFEST_OK = 0,
    APPLIANCE_UPDATE_MANIFEST_NULL = 1,
    APPLIANCE_UPDATE_MANIFEST_TOO_LONG = 2,
    APPLIANCE_UPDATE_MANIFEST_BAD_VERSION = 3,
    APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD = 4,
    APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD = 5,
    APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD = 6,
    APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD = 7,
    APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE = 8,
    APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID = 9,
    APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH = 10,
    APPLIANCE_UPDATE_MANIFEST_BAD_REVISION = 11,
    APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST = 12,
    APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE = 13,
    APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE = 14,
    APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE = 15,
    APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET = 16,
    APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED = 17,
    APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST = 18,
};

struct appliance_update_manifest {
    uint32_t update_package_version;
    char release_id[RELEASE_PROFILE_ID_LENGTH + 1U];
    char architecture[RELEASE_PROFILE_ARCH_LENGTH + 1U];
    char source_revision[APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH + 1U];
    char metadata_digest[APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH + 1U];
    char payload_digest[APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH + 1U];
    struct update_compatibility_manifest compatibility;
};

enum appliance_update_manifest_status appliance_update_manifest_parse(
    const char *input,
    size_t input_length,
    struct appliance_update_manifest *manifest_out);
enum appliance_update_manifest_status appliance_update_manifest_check(
    const struct appliance_update_manifest *manifest,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target,
    enum update_compatibility_status *compatibility_status_out);
const char *appliance_update_manifest_status_string(
    enum appliance_update_manifest_status status);

#endif
