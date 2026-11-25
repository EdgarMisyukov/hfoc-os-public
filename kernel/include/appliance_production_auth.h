#ifndef HFOC_APPLIANCE_PRODUCTION_AUTH_H
#define HFOC_APPLIANCE_PRODUCTION_AUTH_H

#include "appliance_admin_auth.h"
#include "appliance_management_authorization.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-365 production auth/authz request integration.
 *
 * This caller-owned, allocation-free composition binds the existing ITEM-320
 * administrative authentication boundary (`appliance_admin_auth`) to the
 * existing ITEM-321 management authorization matrix
 * (`appliance_management_authorization`). It is the production request gate
 * for the read-only management plane: the public discovery target stays
 * anonymous, every other management target requires an exact bearer session
 * with the administrator role and the status-read capability, and an unknown
 * target or method is denied by default. When no credential has been
 * published the composition stays unavailable and fails every request closed.
 *
 * The module owns no storage, network, route dispatch, or session secret
 * beyond the existing admin-auth and authorization boundaries. It never
 * fabricates availability: only a published credential that has been enabled
 * makes `appliance_production_auth_available()` true and the rendered record
 * publish `auth=AVAILABLE`.
 */

#define APPLIANCE_PRODUCTION_AUTH_VERSION 1U
#define APPLIANCE_PRODUCTION_AUTH_OUTPUT_MAX 192U

enum appliance_production_auth_state {
    APPLIANCE_PRODUCTION_AUTH_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_AUTH_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_AUTH_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL = 3,
    APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE = 4,
    APPLIANCE_PRODUCTION_AUTH_STATE_STOPPED = 5,
    APPLIANCE_PRODUCTION_AUTH_STATE_FAILED = 6,
};

enum appliance_production_auth_status {
    APPLIANCE_PRODUCTION_AUTH_OK = 0,
    APPLIANCE_PRODUCTION_AUTH_NULL = 1,
    APPLIANCE_PRODUCTION_AUTH_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_AUTH_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_AUTH_ALREADY_INITIALIZED = 4,
    APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE = 5,
    APPLIANCE_PRODUCTION_AUTH_ALLOWED = 6,
    APPLIANCE_PRODUCTION_AUTH_DENIED = 7,
    APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED = 8,
    APPLIANCE_PRODUCTION_AUTH_BUFFER_TOO_SMALL = 9,
};

struct appliance_production_auth {
    bool initialized;
    uint32_t version;
    enum appliance_production_auth_state state;
    enum appliance_production_auth_status last_status;
    struct appliance_admin_auth admin;
    struct appliance_management_authorization_policy policy;
    uint32_t allowed_count;
    uint32_t denied_count;
    uint32_t anonymous_count;
    uint32_t unavailable_count;
};

enum appliance_production_auth_status appliance_production_auth_init(
    struct appliance_production_auth *auth);
enum appliance_production_auth_status
appliance_production_auth_publish_credential(
    struct appliance_production_auth *auth,
    const struct appliance_admin_auth_credential *credential);
enum appliance_production_auth_status appliance_production_auth_stop(
    struct appliance_production_auth *auth);
enum appliance_production_auth_status appliance_production_auth_reset(
    struct appliance_production_auth *auth);
bool appliance_production_auth_available(
    const struct appliance_production_auth *auth);
enum appliance_production_auth_status
appliance_production_auth_authorize_request(
    struct appliance_production_auth *auth,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    uint64_t now_tick,
    struct appliance_admin_auth_decision *decision_out,
    enum appliance_management_authorization_route *route_out);
enum appliance_production_auth_status
appliance_production_auth_authorize_mutation(
    struct appliance_production_auth *auth,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    uint64_t now_tick,
    struct appliance_admin_auth_decision *decision_out,
    enum appliance_management_authorization_mutation *mutation_out);
enum appliance_production_auth_status appliance_production_auth_render(
    const struct appliance_production_auth *auth,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_production_auth_state_string(
    enum appliance_production_auth_state state);

#endif
