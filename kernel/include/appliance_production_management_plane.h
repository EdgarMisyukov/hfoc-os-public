#ifndef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_H
#define HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_H

#include "appliance_production_auth.h"
#include "appliance_production_management.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-366 read-only management plane.
 *
 * This caller-owned, allocation-free composition binds the ITEM-354
 * production management snapshot (`appliance_production_management`) to the
 * ITEM-365 production request gate (`appliance_production_auth`) so the
 * ordinary production boot can admit read-only management requests without
 * introducing a second device registry, route table, or mutation path.
 *
 * The plane borrows both compositions; it owns no storage, network, TLS,
 * session secret, or credential. It never mutates appliance state: every
 * request that is not an authorized read of a known management target is
 * denied by default, and a missing credential or unpublished management
 * evidence keeps the plane UNAVAILABLE and fails closed. Readiness is
 * published honestly: READY only when management evidence is published and
 * the request gate is available, DEGRADED when the published management
 * snapshot itself is degraded, UNAVAILABLE otherwise.
 */

#define APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_VERSION 1U
#define APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OUTPUT_MAX \
    APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX

enum appliance_production_management_plane_state {
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_READY = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_STOPPED = 6,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_STATE_FAILED = 7,
};

enum appliance_production_management_plane_status {
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_NULL = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_DENIED = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_ALLOWED = 6,
    APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_BUFFER_TOO_SMALL = 7,
};

struct appliance_production_management_plane {
    bool initialized;
    uint32_t version;
    enum appliance_production_management_plane_state state;
    enum appliance_production_management_plane_status last_status;
    struct appliance_production_management *management;
    struct appliance_production_auth *auth;
    uint32_t allowed_count;
    uint32_t denied_count;
    uint32_t unavailable_count;
};

enum appliance_production_management_plane_status
appliance_production_management_plane_init(
    struct appliance_production_management_plane *plane,
    struct appliance_production_management *management,
    struct appliance_production_auth *auth);
bool appliance_production_management_plane_ready(
    const struct appliance_production_management_plane *plane);
enum appliance_production_management_plane_status
appliance_production_management_plane_refresh(
    struct appliance_production_management_plane *plane);
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
    size_t *output_length);
enum appliance_production_management_plane_status
appliance_production_management_plane_stop(
    struct appliance_production_management_plane *plane);
enum appliance_production_management_plane_status
appliance_production_management_plane_fail_closed(
    struct appliance_production_management_plane *plane);
enum appliance_production_management_plane_state
appliance_production_management_plane_state(
    const struct appliance_production_management_plane *plane);
uint32_t appliance_production_management_plane_allowed_count(
    const struct appliance_production_management_plane *plane);
uint32_t appliance_production_management_plane_denied_count(
    const struct appliance_production_management_plane *plane);
uint32_t appliance_production_management_plane_unavailable_count(
    const struct appliance_production_management_plane *plane);
const char *appliance_production_management_plane_state_string(
    enum appliance_production_management_plane_state state);
const char *appliance_production_management_plane_status_string(
    enum appliance_production_management_plane_status status);

#endif
