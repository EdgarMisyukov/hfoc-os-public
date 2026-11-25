#include "release_profile.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum release_profile_status release_profile_validate_impl(
    const struct release_profile *profile)
{
    if (profile == NULL) {
        return RELEASE_PROFILE_NULL;
    }
    if (profile->version != RELEASE_PROFILE_VERSION) {
        return RELEASE_PROFILE_BAD_VERSION;
    }
    if (memcmp(profile->release_id, RELEASE_PROFILE_ID,
               RELEASE_PROFILE_ID_LENGTH) != 0 ||
        profile->release_id[RELEASE_PROFILE_ID_LENGTH] != '\0') {
        return RELEASE_PROFILE_BAD_RELEASE_ID;
    }
    if (memcmp(profile->architecture, RELEASE_PROFILE_ARCH,
               RELEASE_PROFILE_ARCH_LENGTH) != 0 ||
        profile->architecture[RELEASE_PROFILE_ARCH_LENGTH] != '\0') {
        return RELEASE_PROFILE_BAD_ARCHITECTURE;
    }
    if (profile->image_generation != RELEASE_PROFILE_IMAGE_GENERATION ||
        profile->policy_version != RELEASE_PROFILE_POLICY_VERSION ||
        profile->config_schema_version !=
            RELEASE_PROFILE_CONFIG_SCHEMA_VERSION ||
        profile->state_schema_version !=
            RELEASE_PROFILE_STATE_SCHEMA_VERSION ||
        profile->image_manifest_version !=
            RELEASE_PROFILE_IMAGE_MANIFEST_VERSION ||
        profile->update_compatibility_version !=
            RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION ||
        profile->integration_schema_version !=
            RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION ||
        profile->integration_transport_version !=
            RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION) {
        return RELEASE_PROFILE_BAD_STATE;
    }
    return RELEASE_PROFILE_OK;
}

enum release_profile_status release_profile_init(
    struct release_profile *profile)
{
    if (profile == NULL) {
        return RELEASE_PROFILE_NULL;
    }

    memset(profile, 0, sizeof(*profile));
    profile->version = RELEASE_PROFILE_VERSION;
    memcpy(profile->release_id, RELEASE_PROFILE_ID,
           RELEASE_PROFILE_ID_LENGTH);
    memcpy(profile->architecture, RELEASE_PROFILE_ARCH,
           RELEASE_PROFILE_ARCH_LENGTH);
    profile->image_generation = RELEASE_PROFILE_IMAGE_GENERATION;
    profile->policy_version = RELEASE_PROFILE_POLICY_VERSION;
    profile->config_schema_version =
        RELEASE_PROFILE_CONFIG_SCHEMA_VERSION;
    profile->state_schema_version = RELEASE_PROFILE_STATE_SCHEMA_VERSION;
    profile->image_manifest_version =
        RELEASE_PROFILE_IMAGE_MANIFEST_VERSION;
    profile->update_compatibility_version =
        RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION;
    profile->integration_schema_version =
        RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION;
    profile->integration_transport_version =
        RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION;
    return RELEASE_PROFILE_OK;
}

enum release_profile_status release_profile_validate(
    const struct release_profile *profile)
{
    return release_profile_validate_impl(profile);
}

enum release_profile_status release_profile_check(
    const struct release_profile *profile,
    const struct release_profile_request *request)
{
    enum release_profile_status status;

    if (profile == NULL || request == NULL) {
        return RELEASE_PROFILE_NULL;
    }
    status = release_profile_validate_impl(profile);
    if (status != RELEASE_PROFILE_OK) {
        return status;
    }
    if (request->architecture == NULL) {
        return RELEASE_PROFILE_NULL;
    }
    if (request->architecture_length != RELEASE_PROFILE_ARCH_LENGTH ||
        memcmp(request->architecture, RELEASE_PROFILE_ARCH,
               RELEASE_PROFILE_ARCH_LENGTH) != 0) {
        return RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE;
    }
    if (request->image_generation != RELEASE_PROFILE_IMAGE_GENERATION) {
        return RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION;
    }
    if (request->policy_version != RELEASE_PROFILE_POLICY_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION;
    }
    if (request->config_schema_version !=
        RELEASE_PROFILE_CONFIG_SCHEMA_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION;
    }
    if (request->state_schema_version !=
        RELEASE_PROFILE_STATE_SCHEMA_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION;
    }
    if (request->image_manifest_version !=
        RELEASE_PROFILE_IMAGE_MANIFEST_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION;
    }
    if (request->update_compatibility_version !=
        RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION;
    }
    if (request->integration_schema_version !=
        RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION;
    }
    if (request->integration_transport_version !=
        RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION) {
        return RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION;
    }
    return RELEASE_PROFILE_OK;
}

const char *release_profile_status_string(
    enum release_profile_status status)
{
    switch (status) {
    case RELEASE_PROFILE_OK:
        return "RELEASE_PROFILE_OK";
    case RELEASE_PROFILE_NULL:
        return "RELEASE_PROFILE_NULL";
    case RELEASE_PROFILE_BAD_VERSION:
        return "RELEASE_PROFILE_BAD_VERSION";
    case RELEASE_PROFILE_BAD_STATE:
        return "RELEASE_PROFILE_BAD_STATE";
    case RELEASE_PROFILE_BAD_RELEASE_ID:
        return "RELEASE_PROFILE_BAD_RELEASE_ID";
    case RELEASE_PROFILE_BAD_ARCHITECTURE:
        return "RELEASE_PROFILE_BAD_ARCHITECTURE";
    case RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE:
        return "RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE";
    case RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION:
        return "RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION";
    case RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION";
    default:
        return NULL;
    }
}
