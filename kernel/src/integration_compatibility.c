#include "integration_compatibility.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum integration_compatibility_status
integration_compatibility_policy_validate_impl(
    const struct integration_compatibility_policy *policy)
{
    if (policy == NULL) {
        return INTEGRATION_COMPATIBILITY_NULL;
    }
    if (policy->version != INTEGRATION_COMPATIBILITY_VERSION) {
        return INTEGRATION_COMPATIBILITY_BAD_VERSION;
    }
    if (policy->schema_version_min <
            INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN ||
        policy->schema_version_max >
            INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX ||
        policy->schema_version_min > policy->schema_version_max ||
        policy->transport_version_min <
            INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN ||
        policy->transport_version_max >
            INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX ||
        policy->transport_version_min > policy->transport_version_max) {
        return INTEGRATION_COMPATIBILITY_BAD_STATE;
    }
    if (policy->deprecation_state !=
            INTEGRATION_COMPATIBILITY_DEPRECATION_ACTIVE &&
        policy->deprecation_state !=
            INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED) {
        return INTEGRATION_COMPATIBILITY_BAD_STATE;
    }
    return INTEGRATION_COMPATIBILITY_OK;
}

enum integration_compatibility_status integration_compatibility_policy_init(
    struct integration_compatibility_policy *policy)
{
    if (policy == NULL) {
        return INTEGRATION_COMPATIBILITY_NULL;
    }

    memset(policy, 0, sizeof(*policy));
    policy->version = INTEGRATION_COMPATIBILITY_VERSION;
    policy->schema_version_min =
        INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN;
    policy->schema_version_max =
        INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX;
    policy->transport_version_min =
        INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN;
    policy->transport_version_max =
        INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX;
    policy->deprecation_state =
        INTEGRATION_COMPATIBILITY_DEPRECATION_ACTIVE;
    return INTEGRATION_COMPATIBILITY_OK;
}

enum integration_compatibility_status integration_compatibility_policy_validate(
    const struct integration_compatibility_policy *policy)
{
    return integration_compatibility_policy_validate_impl(policy);
}

enum integration_compatibility_status integration_compatibility_policy_deprecate(
    struct integration_compatibility_policy *policy)
{
    enum integration_compatibility_status status;

    if (policy == NULL) {
        return INTEGRATION_COMPATIBILITY_NULL;
    }
    status = integration_compatibility_policy_validate_impl(policy);
    if (status != INTEGRATION_COMPATIBILITY_OK) {
        return status;
    }
    if (policy->deprecation_state ==
        INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED) {
        return INTEGRATION_COMPATIBILITY_BAD_STATE;
    }

    policy->deprecation_state =
        INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED;
    return INTEGRATION_COMPATIBILITY_OK;
}

enum integration_compatibility_status integration_compatibility_check(
    const struct integration_compatibility_policy *policy,
    const struct integration_compatibility_request *request)
{
    enum integration_compatibility_status status;

    if (policy == NULL || request == NULL) {
        return INTEGRATION_COMPATIBILITY_NULL;
    }
    status = integration_compatibility_policy_validate_impl(policy);
    if (status != INTEGRATION_COMPATIBILITY_OK) {
        return status;
    }
    if (policy->deprecation_state ==
        INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED) {
        return INTEGRATION_COMPATIBILITY_DEPRECATED;
    }
    if (request->schema_version < policy->schema_version_min ||
        request->schema_version > policy->schema_version_max) {
        return INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION;
    }
    if (request->transport_version < policy->transport_version_min ||
        request->transport_version > policy->transport_version_max) {
        return INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION;
    }
    return INTEGRATION_COMPATIBILITY_OK;
}

enum integration_compatibility_status integration_compatibility_session_validate(
    const struct integration_compatibility_policy *policy,
    const struct integration_session_request *session_request,
    struct integration_session_nonce_registry *registry)
{
    struct integration_compatibility_request request;
    enum integration_compatibility_status status;
    enum integration_session_status session_status;

    if (policy == NULL || session_request == NULL || registry == NULL) {
        return INTEGRATION_COMPATIBILITY_NULL;
    }

    request.schema_version = session_request->schema_version;
    request.transport_version = session_request->transport_version;
    status = integration_compatibility_check(policy, &request);
    if (status != INTEGRATION_COMPATIBILITY_OK) {
        return status;
    }

    session_status = integration_session_handshake_validate(
        session_request, registry);
    if (session_status != INTEGRATION_SESSION_OK) {
        return INTEGRATION_COMPATIBILITY_SESSION_REJECTED;
    }
    return INTEGRATION_COMPATIBILITY_OK;
}

const char *integration_compatibility_status_string(
    enum integration_compatibility_status status)
{
    switch (status) {
    case INTEGRATION_COMPATIBILITY_OK:
        return "INTEGRATION_COMPATIBILITY_OK";
    case INTEGRATION_COMPATIBILITY_NULL:
        return "INTEGRATION_COMPATIBILITY_NULL";
    case INTEGRATION_COMPATIBILITY_BAD_VERSION:
        return "INTEGRATION_COMPATIBILITY_BAD_VERSION";
    case INTEGRATION_COMPATIBILITY_BAD_STATE:
        return "INTEGRATION_COMPATIBILITY_BAD_STATE";
    case INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION:
        return "INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION";
    case INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION:
        return "INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION";
    case INTEGRATION_COMPATIBILITY_DEPRECATED:
        return "INTEGRATION_COMPATIBILITY_DEPRECATED";
    case INTEGRATION_COMPATIBILITY_SESSION_REJECTED:
        return "INTEGRATION_COMPATIBILITY_SESSION_REJECTED";
    default:
        return NULL;
    }
}
