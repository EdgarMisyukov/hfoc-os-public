#include "appliance_production_auth.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void appliance_production_auth_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_production_auth_state_valid(
    enum appliance_production_auth_state state)
{
    return state >= APPLIANCE_PRODUCTION_AUTH_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
}

static bool appliance_production_auth_status_valid(
    enum appliance_production_auth_status status)
{
    return status >= APPLIANCE_PRODUCTION_AUTH_OK &&
           status <= APPLIANCE_PRODUCTION_AUTH_BUFFER_TOO_SMALL;
}

static bool appliance_production_auth_initialized(
    const struct appliance_production_auth *auth)
{
    return auth != NULL &&
           auth->initialized &&
           auth->version == APPLIANCE_PRODUCTION_AUTH_VERSION &&
           appliance_production_auth_state_valid(auth->state) &&
           appliance_production_auth_status_valid(auth->last_status);
}

bool appliance_production_auth_available(
    const struct appliance_production_auth *auth)
{
    return appliance_production_auth_initialized(auth) &&
           auth->state == APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE &&
           auth->admin.state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE &&
           auth->admin.credential_published &&
           auth->policy.initialized &&
           auth->policy.matrix.initialized;
}

enum appliance_production_auth_status appliance_production_auth_init(
    struct appliance_production_auth *auth)
{
    if (auth == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (auth->initialized) {
        return APPLIANCE_PRODUCTION_AUTH_ALREADY_INITIALIZED;
    }
    appliance_production_auth_zero_bytes(auth, sizeof(*auth));
    auth->version = APPLIANCE_PRODUCTION_AUTH_VERSION;
    auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_PREPARED;
    auth->last_status = APPLIANCE_PRODUCTION_AUTH_OK;
    if (appliance_admin_auth_init(&auth->admin) != APPLIANCE_ADMIN_AUTH_OK) {
        auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (appliance_management_authorization_policy_init(&auth->policy) !=
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK) {
        auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL;
    auth->initialized = true;
    return APPLIANCE_PRODUCTION_AUTH_OK;
}

enum appliance_production_auth_status
appliance_production_auth_publish_credential(
    struct appliance_production_auth *auth,
    const struct appliance_admin_auth_credential *credential)
{
    if (auth == NULL || credential == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (auth->state == APPLIANCE_PRODUCTION_AUTH_STATE_STOPPED ||
        auth->state == APPLIANCE_PRODUCTION_AUTH_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (auth->state == APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE) {
        /*
         * A live credential can only be rotated through the reset path, which
         * revokes every open session before the replacement verifier is
         * accepted.
         */
        if (appliance_admin_auth_reset(&auth->admin) !=
            APPLIANCE_ADMIN_AUTH_OK) {
            auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
            auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
            return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        }
    }
    if (appliance_admin_auth_credential_publish(&auth->admin, credential) !=
        APPLIANCE_ADMIN_AUTH_OK) {
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (appliance_admin_auth_enable(&auth->admin) != APPLIANCE_ADMIN_AUTH_OK) {
        auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE;
    auth->last_status = APPLIANCE_PRODUCTION_AUTH_OK;
    return APPLIANCE_PRODUCTION_AUTH_OK;
}

enum appliance_production_auth_status appliance_production_auth_stop(
    struct appliance_production_auth *auth)
{
    if (auth == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (appliance_admin_auth_disable(&auth->admin) != APPLIANCE_ADMIN_AUTH_OK) {
        auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_STOPPED;
    auth->last_status = APPLIANCE_PRODUCTION_AUTH_OK;
    return APPLIANCE_PRODUCTION_AUTH_OK;
}

enum appliance_production_auth_status appliance_production_auth_reset(
    struct appliance_production_auth *auth)
{
    if (auth == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    if (appliance_admin_auth_reset(&auth->admin) != APPLIANCE_ADMIN_AUTH_OK) {
        auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_FAILED;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    auth->state = APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL;
    auth->last_status = APPLIANCE_PRODUCTION_AUTH_OK;
    return APPLIANCE_PRODUCTION_AUTH_OK;
}

static enum appliance_production_auth_status
appliance_production_auth_authentication_denied(
    struct appliance_production_auth *auth,
    enum appliance_management_authorization_status authorization_status)
{
    ++auth->denied_count;
    if (authorization_status ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED ||
        authorization_status ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE ||
        authorization_status ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY) {
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_DENIED;
        return APPLIANCE_PRODUCTION_AUTH_DENIED;
    }
    auth->last_status = APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED;
    return APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED;
}

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
    enum appliance_management_authorization_route *route_out)
{
    enum appliance_admin_auth_status authentication_status;
    enum appliance_management_authorization_status authorization_status;

    if (auth == NULL || decision_out == NULL || route_out == NULL ||
        target == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    appliance_production_auth_zero_bytes(
        decision_out, sizeof(*decision_out));
    *route_out = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    if (!appliance_production_auth_available(auth)) {
        ++auth->unavailable_count;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
    }

    authentication_status = appliance_admin_auth_authenticate_request(
        &auth->admin,
        authorization_header,
        authorization_header_length,
        now_tick,
        decision_out);
    if (authentication_status != APPLIANCE_ADMIN_AUTH_OK &&
        authentication_status != APPLIANCE_ADMIN_AUTH_HEADER_MISSING &&
        authentication_status != APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_TOKEN_INVALID &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_CLOSED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_REVOKED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED) {
        ++auth->unavailable_count;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
    }

    authorization_status = appliance_management_authorization_check_request(
        &auth->policy,
        decision_out,
        method,
        target,
        target_length,
        route_out);
    if (authorization_status == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK) {
        ++auth->allowed_count;
        if (!decision_out->authenticated) {
            ++auth->anonymous_count;
        }
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_ALLOWED;
        return APPLIANCE_PRODUCTION_AUTH_ALLOWED;
    }
    return appliance_production_auth_authentication_denied(
        auth, authorization_status);
}

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
    enum appliance_management_authorization_mutation *mutation_out)
{
    enum appliance_admin_auth_status authentication_status;
    enum appliance_management_authorization_status authorization_status;

    if (auth == NULL || decision_out == NULL || mutation_out == NULL ||
        target == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    appliance_production_auth_zero_bytes(
        decision_out, sizeof(*decision_out));
    *mutation_out = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;

    if (!appliance_production_auth_available(auth)) {
        ++auth->unavailable_count;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
    }

    authentication_status = appliance_admin_auth_authenticate_request(
        &auth->admin,
        authorization_header,
        authorization_header_length,
        now_tick,
        decision_out);
    if (authentication_status != APPLIANCE_ADMIN_AUTH_OK &&
        authentication_status != APPLIANCE_ADMIN_AUTH_HEADER_MISSING &&
        authentication_status != APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_TOKEN_INVALID &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_CLOSED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_SESSION_REVOKED &&
        authentication_status != APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED) {
        ++auth->unavailable_count;
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE;
    }

    authorization_status =
        appliance_management_authorization_check_mutation_request(
            &auth->policy,
            decision_out,
            method,
            target,
            target_length,
            mutation_out);
    if (authorization_status == APPLIANCE_MANAGEMENT_AUTHORIZATION_OK) {
        ++auth->allowed_count;
        if (!decision_out->authenticated) {
            ++auth->anonymous_count;
        }
        auth->last_status = APPLIANCE_PRODUCTION_AUTH_ALLOWED;
        return APPLIANCE_PRODUCTION_AUTH_ALLOWED;
    }
    *mutation_out = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    return appliance_production_auth_authentication_denied(
        auth, authorization_status);
}

static bool appliance_production_auth_append(
    char *rendered,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset + length > APPLIANCE_PRODUCTION_AUTH_OUTPUT_MAX) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        rendered[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_production_auth_append_text(
    char *rendered,
    size_t *offset,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_production_auth_append(
        rendered, offset, text, length);
}

static bool appliance_production_auth_append_u32(
    char *rendered,
    size_t *offset,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        return appliance_production_auth_append(rendered, offset, "0", 1U);
    }
    while (value > 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        if (!appliance_production_auth_append(
                rendered, offset, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

const char *appliance_production_auth_state_string(
    enum appliance_production_auth_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_AUTH_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_AUTH_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_AUTH_STATE_AWAITING_CREDENTIAL:
        return "AWAITING_CREDENTIAL";
    case APPLIANCE_PRODUCTION_AUTH_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_PRODUCTION_AUTH_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_AUTH_STATE_FAILED:
        return "FAILED";
    case APPLIANCE_PRODUCTION_AUTH_STATE_NONE:
    default:
        return "NONE";
    }
}

enum appliance_production_auth_status appliance_production_auth_render(
    const struct appliance_production_auth *auth,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_PRODUCTION_AUTH_OUTPUT_MAX + 1U];
    size_t offset = 0U;
    const char *state;
    const char *authorization;

    if (auth == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_AUTH_NULL;
    }
    if (!appliance_production_auth_initialized(auth)) {
        return APPLIANCE_PRODUCTION_AUTH_BAD_STATE;
    }
    appliance_production_auth_zero_bytes(rendered, sizeof(rendered));
    state = appliance_production_auth_state_string(auth->state);
    authorization = appliance_production_auth_available(auth)
        ? "AVAILABLE"
        : "UNAVAILABLE";
    if (state == NULL ||
        !appliance_production_auth_append_text(
            rendered, &offset, "production_auth v=1 state=") ||
        !appliance_production_auth_append_text(rendered, &offset, state) ||
        !appliance_production_auth_append_text(
            rendered, &offset, " auth=") ||
        !appliance_production_auth_append_text(
            rendered, &offset, authorization) ||
        !appliance_production_auth_append_text(
            rendered, &offset, " allowed=") ||
        !appliance_production_auth_append_u32(
            rendered, &offset, auth->allowed_count) ||
        !appliance_production_auth_append_text(
            rendered, &offset, " denied=") ||
        !appliance_production_auth_append_u32(
            rendered, &offset, auth->denied_count) ||
        !appliance_production_auth_append_text(
            rendered, &offset, " anonymous=") ||
        !appliance_production_auth_append_u32(
            rendered, &offset, auth->anonymous_count) ||
        !appliance_production_auth_append_text(
            rendered, &offset, " unavailable=") ||
        !appliance_production_auth_append_u32(
            rendered, &offset, auth->unavailable_count) ||
        !appliance_production_auth_append(
            rendered, &offset, "\n", 1U)) {
        return APPLIANCE_PRODUCTION_AUTH_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_PRODUCTION_AUTH_BUFFER_TOO_SMALL;
    }
    appliance_production_auth_zero_bytes(output, capacity);
    {
        size_t index;

        for (index = 0U; index < offset; ++index) {
            output[index] = rendered[index];
        }
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_PRODUCTION_AUTH_OK;
}
