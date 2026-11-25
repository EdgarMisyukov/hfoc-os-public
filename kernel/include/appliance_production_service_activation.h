#ifndef HFOC_APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_H
#define HFOC_APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_H

#include "appliance_production_service_https.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_VERSION 1U
#define APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX 256U

/*
 * ITEM-364 ordinary-boot production service/HTTPS activation.
 *
 * The ITEM-353 composition boundary (appliance_production_service_https) is a
 * caller-owned state machine: it never touches the service manager, the HTTPS
 * lifecycle, storage, or the network. This module is the production caller. It
 * validates the persisted service evidence, evaluates the explicit
 * network/entropy/certificate readiness, drives the composition in the
 * required order, and publishes one bounded activation record that ordinary
 * boot can log.
 *
 * The activation is honest: it never fabricates network, entropy, or
 * certificate readiness. Missing evidence leaves the object in an explicit
 * awaiting state, a link loss drains and fails the composition closed, and the
 * module never accepts, copies, or renders credentials, private keys, or
 * certificate payloads. The rendered record only ever carries the fixed
 * "secrets=NONE" marker.
 *
 * The frozen baseline keeps the HTTPS lifecycle boundary host/QEMU-only, so
 * this production module drives the always-compiled ITEM-353 composition. The
 * host proof still exercises the real HTTPS lifecycle through the same
 * activation contract, while ordinary boot publishes the honest composition
 * state without linking the host-only boundary into the production kernel.
 */

enum appliance_production_service_activation_state {
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED = 1,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE = 2,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK = 3,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS = 4,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING = 5,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED = 6,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED = 7,
};

enum appliance_production_service_activation_status {
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK = 0,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL = 1,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED = 4,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE = 5,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK = 6,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS = 7,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_START_FAILED = 8,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STOP_FAILED = 9,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_RESTART_FAILED = 10,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST = 11,
    APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_BUFFER_TOO_SMALL = 12,
};

struct appliance_production_service_activation {
    bool initialized;
    bool configured;
    uint32_t version;
    enum appliance_production_service_activation_state state;
    enum appliance_production_service_activation_status last_status;
    struct appliance_production_service_https *composition;
    struct appliance_production_service_https_evidence evidence;
    uint64_t source_generation;
    bool network_active;
    bool entropy_ready;
    bool certificate_ready;
    bool clock_synchronized;
    bool https_enabled;
    uint32_t start_count;
    uint32_t restart_count;
    uint32_t stop_count;
    uint32_t degraded_count;
    uint32_t fail_count;
    bool secrets_logged;
};

struct appliance_production_service_activation_snapshot {
    bool published;
    uint32_t version;
    enum appliance_production_service_activation_state state;
    enum appliance_production_service_activation_status last_status;
    uint64_t source_generation;
    bool network_active;
    bool entropy_ready;
    bool certificate_ready;
    bool https_enabled;
    uint32_t start_count;
    uint32_t restart_count;
    uint32_t stop_count;
    uint32_t degraded_count;
    uint32_t fail_count;
    bool secrets_logged;
};

enum appliance_production_service_activation_status
appliance_production_service_activation_init(
    struct appliance_production_service_activation *activation,
    struct appliance_production_service_https *composition);
enum appliance_production_service_activation_status
appliance_production_service_activation_configure(
    struct appliance_production_service_activation *activation,
    const struct appliance_production_service_https_evidence *evidence);
enum appliance_production_service_activation_status
appliance_production_service_activation_evaluate(
    struct appliance_production_service_activation *activation,
    bool network_active,
    bool entropy_ready,
    bool certificate_ready,
    uint32_t now_ms);
/* ITEM-374: the clock gate.  TLS may start only after a validated time
 * sample; the legacy entry point above keeps the pre-clock behaviour for the
 * frozen proofs. */
enum appliance_production_service_activation_status
appliance_production_service_activation_evaluate_clock(
    struct appliance_production_service_activation *activation,
    bool network_active,
    bool entropy_ready,
    bool certificate_ready,
    bool clock_synchronized,
    uint32_t now_ms);
enum appliance_production_service_activation_status
appliance_production_service_activation_poll(
    struct appliance_production_service_activation *activation,
    bool network_active,
    uint32_t now_ms);
enum appliance_production_service_activation_status
appliance_production_service_activation_restart(
    struct appliance_production_service_activation *activation);
enum appliance_production_service_activation_status
appliance_production_service_activation_stop(
    struct appliance_production_service_activation *activation);
enum appliance_production_service_activation_status
appliance_production_service_activation_snapshot(
    const struct appliance_production_service_activation *activation,
    struct appliance_production_service_activation_snapshot *snapshot_out);
enum appliance_production_service_activation_status
appliance_production_service_activation_render(
    const struct appliance_production_service_activation_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_service_activation_state
appliance_production_service_activation_state(
    const struct appliance_production_service_activation *activation);
bool appliance_production_service_activation_running(
    const struct appliance_production_service_activation *activation);
bool appliance_production_service_activation_secrets_logged(
    const struct appliance_production_service_activation *activation);
const char *appliance_production_service_activation_state_string(
    enum appliance_production_service_activation_state state);
const char *appliance_production_service_activation_status_string(
    enum appliance_production_service_activation_status status);

#endif
