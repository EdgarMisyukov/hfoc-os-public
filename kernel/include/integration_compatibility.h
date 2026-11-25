#ifndef HFOC_INTEGRATION_COMPATIBILITY_H
#define HFOC_INTEGRATION_COMPATIBILITY_H

#include "integration_boundary.h"
#include "integration_session.h"

#include <stdint.h>

#define INTEGRATION_COMPATIBILITY_VERSION 1U
#define INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MIN \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN
#define INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX
#define INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MIN \
    INTEGRATION_BOUNDARY_TRANSPORT_VERSION
#define INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX \
    INTEGRATION_BOUNDARY_TRANSPORT_VERSION
#define INTEGRATION_COMPATIBILITY_DEPRECATION_ACTIVE 1U
#define INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED 2U

enum integration_compatibility_status {
    INTEGRATION_COMPATIBILITY_OK = 0,
    INTEGRATION_COMPATIBILITY_NULL = 1,
    INTEGRATION_COMPATIBILITY_BAD_VERSION = 2,
    INTEGRATION_COMPATIBILITY_BAD_STATE = 3,
    INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION = 4,
    INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION = 5,
    INTEGRATION_COMPATIBILITY_DEPRECATED = 6,
    INTEGRATION_COMPATIBILITY_SESSION_REJECTED = 7,
};

struct integration_compatibility_policy {
    uint32_t version;
    uint32_t schema_version_min;
    uint32_t schema_version_max;
    uint32_t transport_version_min;
    uint32_t transport_version_max;
    uint32_t deprecation_state;
};

struct integration_compatibility_request {
    uint32_t schema_version;
    uint32_t transport_version;
};

enum integration_compatibility_status integration_compatibility_policy_init(
    struct integration_compatibility_policy *policy);
enum integration_compatibility_status integration_compatibility_policy_validate(
    const struct integration_compatibility_policy *policy);
enum integration_compatibility_status integration_compatibility_policy_deprecate(
    struct integration_compatibility_policy *policy);
enum integration_compatibility_status integration_compatibility_check(
    const struct integration_compatibility_policy *policy,
    const struct integration_compatibility_request *request);
enum integration_compatibility_status integration_compatibility_session_validate(
    const struct integration_compatibility_policy *policy,
    const struct integration_session_request *session_request,
    struct integration_session_nonce_registry *registry);
const char *integration_compatibility_status_string(
    enum integration_compatibility_status status);

#endif
