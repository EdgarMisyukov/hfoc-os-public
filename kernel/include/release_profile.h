#ifndef HFOC_RELEASE_PROFILE_H
#define HFOC_RELEASE_PROFILE_H

#include "config_schema.h"
#include "integration_boundary.h"
#include "system_image.h"
#include "update_compatibility.h"

#include <stddef.h>
#include <stdint.h>

#define RELEASE_PROFILE_VERSION 1U
#define RELEASE_PROFILE_ID "HFOC-APPLIANCE-V1"
#define RELEASE_PROFILE_ID_LENGTH 17U
#define RELEASE_PROFILE_ARCH SYSTEM_IMAGE_ARCH_VALUE
#define RELEASE_PROFILE_ARCH_LENGTH 6U
#define RELEASE_PROFILE_IMAGE_GENERATION 1U
#define RELEASE_PROFILE_POLICY_VERSION 1U
#define RELEASE_PROFILE_CONFIG_SCHEMA_VERSION CONFIG_SCHEMA_VERSION
#define RELEASE_PROFILE_STATE_SCHEMA_VERSION 1U
#define RELEASE_PROFILE_IMAGE_MANIFEST_VERSION \
    SYSTEM_IMAGE_MANIFEST_VERSION
#define RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION \
    UPDATE_COMPATIBILITY_VERSION
#define RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX
#define RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION \
    INTEGRATION_BOUNDARY_TRANSPORT_VERSION

enum release_profile_status {
    RELEASE_PROFILE_OK = 0,
    RELEASE_PROFILE_NULL = 1,
    RELEASE_PROFILE_BAD_VERSION = 2,
    RELEASE_PROFILE_BAD_STATE = 3,
    RELEASE_PROFILE_BAD_RELEASE_ID = 4,
    RELEASE_PROFILE_BAD_ARCHITECTURE = 5,
    RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE = 6,
    RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION = 7,
    RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION = 8,
    RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION = 9,
    RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION = 10,
    RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION = 11,
    RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION = 12,
    RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION = 13,
    RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION = 14,
};

struct release_profile {
    uint32_t version;
    char release_id[RELEASE_PROFILE_ID_LENGTH + 1U];
    char architecture[RELEASE_PROFILE_ARCH_LENGTH + 1U];
    uint32_t image_generation;
    uint32_t policy_version;
    uint32_t config_schema_version;
    uint32_t state_schema_version;
    uint32_t image_manifest_version;
    uint32_t update_compatibility_version;
    uint32_t integration_schema_version;
    uint32_t integration_transport_version;
};

struct release_profile_request {
    const char *architecture;
    size_t architecture_length;
    uint32_t image_generation;
    uint32_t policy_version;
    uint32_t config_schema_version;
    uint32_t state_schema_version;
    uint32_t image_manifest_version;
    uint32_t update_compatibility_version;
    uint32_t integration_schema_version;
    uint32_t integration_transport_version;
};

enum release_profile_status release_profile_init(
    struct release_profile *profile);
enum release_profile_status release_profile_validate(
    const struct release_profile *profile);
enum release_profile_status release_profile_check(
    const struct release_profile *profile,
    const struct release_profile_request *request);
const char *release_profile_status_string(
    enum release_profile_status status);

#endif
