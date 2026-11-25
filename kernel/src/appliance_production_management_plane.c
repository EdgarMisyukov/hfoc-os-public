#include "appliance_production_management_plane.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_production_management_plane_state_valid(
    enum appliance_production_management_plane_state state)
{
    return state >= APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED;
}

static bool appliance_production_management_plane_status_valid(
    enum appliance_production_management_plane_status status)
{
    return status >= APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK &&
           status <= APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BUFFER_TOO_SMALL;
}

static bool appliance_production_management_plane_initialized(
    const struct appliance_production_management_plane *plane)
{
    return plane != NULL &&
           plane->initialized &&
           plane->version == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_VERSION &&
           appliance_production_management_plane_state_valid(plane->state) &&
           appliance_production_management_plane_status_valid(
               plane->last_status) &&
           plane->management != NULL &&
           plane->auth != NULL;
}

static enum appliance_production_management_plane_state
appliance_production_management_plane_derive_state(
    const struct appliance_production_management_plane *plane,
    bool *ready_out)
{
    struct appliance_production_management_snapshot snapshot;

    *ready_out = false;
    if (!appliance_production_management_published(plane->management) ||
        !appliance_production_auth_available(plane->auth)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNAVAILABLE;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_production_management_snapshot(
            plane->management, &snapshot) !=
            APPLIANCE_PRODUCTION_MANAGEMENT_OK ||
        !snapshot.published) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNAVAILABLE;
    }
    *ready_out = true;
    if (snapshot.state == APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED ||
        snapshot.health != APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_PASS) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED;
    }
    return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_READY;
}

enum appliance_production_management_plane_status
appliance_production_management_plane_init(
    struct appliance_production_management_plane *plane,
    struct appliance_production_management *management,
    struct appliance_production_auth *auth)
{
    if (plane == NULL || management == NULL || auth == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL;
    }
    memset(plane, 0, sizeof(*plane));
    plane->initialized = true;
    plane->version = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_VERSION;
    plane->state = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED;
    plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK;
    plane->management = management;
    plane->auth = auth;
    return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK;
}

enum appliance_production_management_plane_status
appliance_production_management_plane_refresh(
    struct appliance_production_management_plane *plane)
{
    bool ready = false;

    if (!appliance_production_management_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_STATE;
    }
    plane->state = appliance_production_management_plane_derive_state(
        plane, &ready);
    plane->last_status = ready
        ? APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK
        : APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE;
    return plane->last_status;
}

bool appliance_production_management_plane_ready(
    const struct appliance_production_management_plane *plane)
{
    bool ready = false;

    if (!appliance_production_management_plane_initialized(plane)) {
        return false;
    }
    (void)appliance_production_management_plane_derive_state(plane, &ready);
    return ready;
}

enum appliance_production_management_plane_status
appliance_production_management_plane_admit(
    struct appliance_production_management_plane *plane,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_admin_auth_decision decision;
    struct appliance_production_management_snapshot snapshot;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    enum appliance_production_auth_status auth_status;
    enum appliance_production_management_status render_status;
    bool ready = false;
    size_t rendered = 0U;

    if (!appliance_production_management_plane_initialized(plane) ||
        output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED) {
        plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_STATE;
        return plane->last_status;
    }

    plane->state = appliance_production_management_plane_derive_state(
        plane, &ready);
    if (!ready) {
        ++plane->unavailable_count;
        plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE;
        return plane->last_status;
    }

    memset(&decision, 0, sizeof(decision));
    auth_status = appliance_production_auth_authorize_request(
        plane->auth,
        authorization_header,
        authorization_header_length,
        method,
        target,
        target_length,
        now_tick,
        &decision,
        &route);
    if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED) {
        ++plane->denied_count;
        if (auth_status == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE) {
            ++plane->unavailable_count;
            plane->last_status =
                APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE;
            return plane->last_status;
        }
        plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED;
        return plane->last_status;
    }

    if (route != APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY &&
        route != APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS &&
        route != APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS &&
        route != APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS) {
        ++plane->denied_count;
        plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED;
        return plane->last_status;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_production_management_snapshot(
            plane->management, &snapshot) !=
            APPLIANCE_PRODUCTION_MANAGEMENT_OK ||
        !snapshot.published) {
        ++plane->unavailable_count;
        plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE;
        return plane->last_status;
    }
    *output_length = 0U;
    render_status = appliance_production_management_render(
        &snapshot, output, capacity, &rendered);
    if (render_status != APPLIANCE_PRODUCTION_MANAGEMENT_OK) {
        plane->last_status = render_status ==
                APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL
            ? APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BUFFER_TOO_SMALL
            : APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_STATE;
        return plane->last_status;
    }

    *output_length = rendered;
    ++plane->allowed_count;
    plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED;
    return plane->last_status;
}

enum appliance_production_management_plane_status
appliance_production_management_plane_stop(
    struct appliance_production_management_plane *plane)
{
    if (!appliance_production_management_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL;
    }
    plane->state = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED;
    plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK;
    return plane->last_status;
}

enum appliance_production_management_plane_status
appliance_production_management_plane_fail_closed(
    struct appliance_production_management_plane *plane)
{
    if (!appliance_production_management_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL;
    }
    plane->state = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED;
    plane->last_status = APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE;
    return plane->last_status;
}

enum appliance_production_management_plane_state
appliance_production_management_plane_state(
    const struct appliance_production_management_plane *plane)
{
    enum appliance_production_management_plane_state derived;
    bool ready = false;

    if (!appliance_production_management_plane_initialized(plane)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNINITIALIZED;
    }
    if (plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED ||
        plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED) {
        return plane->state;
    }
    derived = appliance_production_management_plane_derive_state(plane, &ready);
    if (derived == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNAVAILABLE &&
        plane->state == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED &&
        plane->last_status == APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED;
    }
    return derived;
}

uint32_t appliance_production_management_plane_allowed_count(
    const struct appliance_production_management_plane *plane)
{
    if (!appliance_production_management_plane_initialized(plane)) {
        return 0U;
    }
    return plane->allowed_count;
}

uint32_t appliance_production_management_plane_denied_count(
    const struct appliance_production_management_plane *plane)
{
    if (!appliance_production_management_plane_initialized(plane)) {
        return 0U;
    }
    return plane->denied_count;
}

uint32_t appliance_production_management_plane_unavailable_count(
    const struct appliance_production_management_plane *plane)
{
    if (!appliance_production_management_plane_initialized(plane)) {
        return 0U;
    }
    return plane->unavailable_count;
}

const char *appliance_production_management_plane_state_string(
    enum appliance_production_management_plane_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED:
        return "PREPARED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_management_plane_status_string(
    enum appliance_production_management_plane_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED:
        return "ALLOWED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}
