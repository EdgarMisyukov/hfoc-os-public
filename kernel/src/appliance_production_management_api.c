#include "appliance_production_management_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The link generation the read-only diagnostics publish.  The production
 * network owner binds its first live interface with generation 1
 * (`APPLIANCE_PRODUCTION_NETWORK_GENERATION`); this module keeps the same
 * value without linking the lwIP-facing network composition into every host
 * proof.
 */
#define APPLIANCE_PRODUCTION_MANAGEMENT_API_LINK_GENERATION 1U

static bool appliance_production_management_api_target_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *path,
    uint32_t path_length)
{
    return target != NULL && target_length == path_length &&
           memcmp(target, path, path_length) == 0;
}

static bool appliance_production_management_api_is_system(
    const uint8_t *target,
    uint32_t target_length)
{
    return appliance_production_management_api_target_equal(
        target,
        target_length,
        APPLIANCE_SYSTEM_API_PATH,
        APPLIANCE_SYSTEM_API_PATH_LENGTH);
}

static bool appliance_production_management_api_is_network(
    const uint8_t *target,
    uint32_t target_length)
{
    return appliance_production_management_api_target_equal(
               target,
               target_length,
               APPLIANCE_NETWORK_API_CONFIG_PATH,
               APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH) ||
           appliance_production_management_api_target_equal(
               target,
               target_length,
               APPLIANCE_NETWORK_API_STATUS_PATH,
               APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH);
}

static bool appliance_production_management_api_is_service(
    const uint8_t *target,
    uint32_t target_length)
{
    return appliance_production_management_api_target_equal(
        target,
        target_length,
        APPLIANCE_SERVICE_API_PATH,
        APPLIANCE_SERVICE_API_PATH_LENGTH);
}

static bool appliance_production_management_api_is_health(
    const uint8_t *target,
    uint32_t target_length)
{
    return appliance_production_management_api_target_equal(
        target,
        target_length,
        APPLIANCE_HEALTH_API_PATH,
        APPLIANCE_HEALTH_API_PATH_LENGTH);
}

enum appliance_production_management_api_status
appliance_production_management_api_publish(
    struct appliance_production_management_api *owner,
    const struct appliance_production_management_api_evidence *evidence)
{
    if (owner == NULL || evidence == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL;
    }
    if (evidence->version !=
            APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION ||
        evidence->identity == NULL ||
        (evidence->config_hostname == NULL &&
         evidence->config_hostname_length != 0U)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->version = APPLIANCE_PRODUCTION_MANAGEMENT_API_VERSION;
    owner->evidence = *evidence;
    owner->published = true;
    return APPLIANCE_PRODUCTION_MANAGEMENT_API_OK;
}

bool appliance_production_management_api_published(
    const struct appliance_production_management_api *owner)
{
    return owner != NULL && owner->published &&
           owner->version == APPLIANCE_PRODUCTION_MANAGEMENT_API_VERSION;
}

bool appliance_production_management_api_owns(
    const struct appliance_production_management_api *owner,
    const uint8_t *target,
    uint32_t target_length)
{
    if (!appliance_production_management_api_published(owner) ||
        target == NULL) {
        return false;
    }
    return appliance_production_management_api_is_system(
               target, target_length) ||
           appliance_production_management_api_is_network(
               target, target_length) ||
           appliance_production_management_api_is_service(
               target, target_length) ||
           appliance_production_management_api_is_health(
               target, target_length);
}

static void appliance_production_management_api_set_domain(
    struct appliance_health_input *input,
    uint32_t index,
    enum appliance_health_domain domain,
    enum appliance_health_state state,
    enum appliance_health_fault fault,
    uint64_t ticks)
{
    input->domains[index].version = APPLIANCE_HEALTH_VERSION;
    input->domains[index].domain = domain;
    input->domains[index].state = state;
    input->domains[index].fault = fault;
    input->domains[index].last_checked_ticks = ticks;
}

static void appliance_production_management_api_health_input(
    const struct appliance_production_management_api_evidence *evidence,
    struct appliance_health_input *input)
{
    const uint64_t ticks = evidence->uptime_ticks;

    memset(input, 0, sizeof(*input));
    input->version = APPLIANCE_HEALTH_VERSION;
    input->generation = ticks;
    input->critical = false;

    switch (evidence->boot_readiness) {
    case APPLIANCE_PRODUCTION_BOOT_READINESS_READY:
        appliance_production_management_api_set_domain(
            input, 0U, APPLIANCE_HEALTH_DOMAIN_BOOT,
            APPLIANCE_HEALTH_STATE_PASS, APPLIANCE_HEALTH_FAULT_NONE,
            ticks);
        break;
    case APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED:
        appliance_production_management_api_set_domain(
            input, 0U, APPLIANCE_HEALTH_DOMAIN_BOOT,
            APPLIANCE_HEALTH_STATE_WARN,
            APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE, ticks);
        break;
    default:
        appliance_production_management_api_set_domain(
            input, 0U, APPLIANCE_HEALTH_DOMAIN_BOOT,
            APPLIANCE_HEALTH_STATE_FAIL,
            APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE, ticks);
        break;
    }

    if (evidence->storage_ready) {
        appliance_production_management_api_set_domain(
            input, 1U, APPLIANCE_HEALTH_DOMAIN_STORAGE,
            APPLIANCE_HEALTH_STATE_PASS, APPLIANCE_HEALTH_FAULT_NONE,
            ticks);
    } else {
        appliance_production_management_api_set_domain(
            input, 1U, APPLIANCE_HEALTH_DOMAIN_STORAGE,
            APPLIANCE_HEALTH_STATE_FAIL,
            APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL, ticks);
    }

    switch (evidence->config_source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        appliance_production_management_api_set_domain(
            input, 2U, APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
            APPLIANCE_HEALTH_STATE_PASS, APPLIANCE_HEALTH_FAULT_NONE,
            ticks);
        break;
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        appliance_production_management_api_set_domain(
            input, 2U, APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
            APPLIANCE_HEALTH_STATE_WARN,
            APPLIANCE_HEALTH_CONFIG_FAULT_MISSING, ticks);
        break;
    default:
        appliance_production_management_api_set_domain(
            input, 2U, APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
            APPLIANCE_HEALTH_STATE_FAIL,
            APPLIANCE_HEALTH_CONFIG_FAULT_MISSING, ticks);
        break;
    }

    if (!evidence->network_link_up) {
        appliance_production_management_api_set_domain(
            input, 3U, APPLIANCE_HEALTH_DOMAIN_NETWORK,
            APPLIANCE_HEALTH_STATE_FAIL,
            APPLIANCE_HEALTH_NETWORK_FAULT_LINK, ticks);
    } else if (evidence->network_active == NULL) {
        appliance_production_management_api_set_domain(
            input, 3U, APPLIANCE_HEALTH_DOMAIN_NETWORK,
            APPLIANCE_HEALTH_STATE_WARN,
            APPLIANCE_HEALTH_NETWORK_FAULT_ADDRESS, ticks);
    } else {
        appliance_production_management_api_set_domain(
            input, 3U, APPLIANCE_HEALTH_DOMAIN_NETWORK,
            APPLIANCE_HEALTH_STATE_PASS, APPLIANCE_HEALTH_FAULT_NONE,
            ticks);
    }

    if (!evidence->service_present) {
        appliance_production_management_api_set_domain(
            input, 4U, APPLIANCE_HEALTH_DOMAIN_SERVICE,
            APPLIANCE_HEALTH_STATE_WARN,
            APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY, ticks);
    } else {
        switch (evidence->service_state) {
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING:
            appliance_production_management_api_set_domain(
                input, 4U, APPLIANCE_HEALTH_DOMAIN_SERVICE,
                APPLIANCE_HEALTH_STATE_PASS,
                APPLIANCE_HEALTH_FAULT_NONE, ticks);
            break;
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED:
            appliance_production_management_api_set_domain(
                input, 4U, APPLIANCE_HEALTH_DOMAIN_SERVICE,
                APPLIANCE_HEALTH_STATE_FAIL,
                APPLIANCE_HEALTH_SERVICE_FAULT_FAILED, ticks);
            break;
        default:
            appliance_production_management_api_set_domain(
                input, 4U, APPLIANCE_HEALTH_DOMAIN_SERVICE,
                APPLIANCE_HEALTH_STATE_WARN,
                APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY, ticks);
            break;
        }
    }

    if (evidence->https_listening) {
        appliance_production_management_api_set_domain(
            input, 5U, APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
            APPLIANCE_HEALTH_STATE_PASS, APPLIANCE_HEALTH_FAULT_NONE,
            ticks);
    } else {
        appliance_production_management_api_set_domain(
            input, 5U, APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
            APPLIANCE_HEALTH_STATE_WARN,
            APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER, ticks);
    }
}

static enum appliance_system_api_state
appliance_production_management_api_system_health(
    enum appliance_health_state state)
{
    switch (state) {
    case APPLIANCE_HEALTH_STATE_PASS:
        return APPLIANCE_SYSTEM_STATE_OK;
    case APPLIANCE_HEALTH_STATE_WARN:
        return APPLIANCE_SYSTEM_STATE_DEGRADED;
    case APPLIANCE_HEALTH_STATE_FAIL:
        return APPLIANCE_SYSTEM_STATE_FAILED;
    default:
        return APPLIANCE_SYSTEM_STATE_UNAVAILABLE;
    }
}

static enum appliance_system_api_state
appliance_production_management_api_config_state(
    enum appliance_config_validation_source source)
{
    switch (source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        return APPLIANCE_SYSTEM_STATE_OK;
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        return APPLIANCE_SYSTEM_STATE_DEGRADED;
    default:
        return APPLIANCE_SYSTEM_STATE_UNAVAILABLE;
    }
}

static bool appliance_production_management_api_system_snapshot(
    const struct appliance_production_management_api_evidence *evidence,
    struct appliance_system_api_snapshot *snapshot)
{
    struct appliance_health_input input;
    struct appliance_health_output output;
    uint32_t recovery_mode;

    appliance_production_management_api_health_input(evidence, &input);
    memset(&output, 0, sizeof(output));
    if (appliance_health_evaluate(&input, &output) !=
        APPLIANCE_HEALTH_STATUS_OK) {
        return false;
    }
    /*
     * The system vocabulary numbers recovery mode 0..2; the boot vocabulary
     * numbers FAIL_CLOSED=0, DEGRADED=1, NORMAL=2, so the two cannot be cast
     * into each other.
     */
    switch (evidence->boot_mode) {
    case APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED:
        recovery_mode = 1U;
        break;
    case APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED:
        recovery_mode = 2U;
        break;
    default:
        recovery_mode = 0U;
        break;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return appliance_system_api_snapshot_init(
               snapshot,
               evidence->identity,
               appliance_production_management_api_system_health(
                   output.state),
               evidence->uptime_ticks,
               appliance_production_management_api_config_state(
                   evidence->config_source),
               evidence->config_source,
               evidence->config_hostname,
               evidence->config_hostname_length,
               recovery_mode,
               0U) == APPLIANCE_SYSTEM_API_OK;
}

static bool appliance_production_management_api_network_snapshot(
    const struct appliance_production_management_api_evidence *evidence,
    struct appliance_network_api_snapshot *snapshot)
{
    struct appliance_network_diagnostics diagnostics;
    const struct appliance_network_diagnostics *diagnostics_ptr = NULL;

    memset(snapshot, 0, sizeof(*snapshot));
    if (evidence->network_desired == NULL) {
        return appliance_network_api_snapshot_init(
                   snapshot, NULL,
                   APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE, NULL) ==
               APPLIANCE_NETWORK_API_OK;
    }
    if (evidence->network_device != NULL) {
        struct network_link_state_record link;

        memset(&link, 0, sizeof(link));
        link.version = NETWORK_LINK_STATE_VERSION;
        link.device_id = evidence->network_device->device_id;
        link.state = evidence->network_link_up ? NETWORK_LINK_UP
                                               : NETWORK_LINK_DOWN;
        link.generation = APPLIANCE_PRODUCTION_MANAGEMENT_API_LINK_GENERATION;
        memset(&diagnostics, 0, sizeof(diagnostics));
        if (appliance_network_diagnostics_publish(
                &diagnostics,
                evidence->network_desired,
                &link,
                evidence->network_active,
                evidence->gateway_reachable
                    ? APPLIANCE_NETWORK_REACHABILITY_REACHABLE
                    : APPLIANCE_NETWORK_REACHABILITY_UNKNOWN,
                evidence->dns_reachable
                    ? APPLIANCE_NETWORK_REACHABILITY_REACHABLE
                    : APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK) {
            diagnostics_ptr = &diagnostics;
        }
    }
    return appliance_network_api_snapshot_init(
               snapshot,
               evidence->network_desired,
               evidence->network_config_source,
               diagnostics_ptr) == APPLIANCE_NETWORK_API_OK;
}

static bool appliance_production_management_api_service_snapshot(
    const struct appliance_production_management_api_evidence *evidence,
    struct appliance_service_api_snapshot *snapshot)
{
    struct appliance_service_manager_snapshot manager;

    memset(&manager, 0, sizeof(manager));
    manager.version = APPLIANCE_SERVICE_MANAGER_VERSION;
    manager.published = true;
    manager.startup_policy = APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK;
    manager.generation = 1U;
    manager.health_fault = SERVICE_HEALTH_FAULT_NONE;
    manager.health_result = HEALTH_CHECK_RESULT_UNKNOWN;
    manager.state = APPLIANCE_SERVICE_MANAGER_STATE_STOPPED;
    if (evidence->service_present) {
        manager.configured_count = 1U;
        switch (evidence->service_state) {
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING:
            manager.state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
            manager.active_count = 1U;
            manager.ready_count = 1U;
            manager.health_result = HEALTH_CHECK_RESULT_PASS;
            break;
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED:
            manager.state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
            manager.active_count = 1U;
            manager.pending_count = 1U;
            manager.health_result = HEALTH_CHECK_RESULT_WARN;
            manager.health_fault = SERVICE_HEALTH_FAULT_DEPENDENCY;
            break;
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_FAILED:
            manager.state = APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED;
            manager.active_count = 1U;
            manager.failed_count = 1U;
            manager.health_result = HEALTH_CHECK_RESULT_FAIL;
            manager.health_fault = SERVICE_HEALTH_FAULT_READINESS;
            break;
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE:
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK:
        case APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS:
            manager.state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
            manager.active_count = 1U;
            manager.pending_count = 1U;
            manager.health_result = HEALTH_CHECK_RESULT_WARN;
            manager.health_fault = SERVICE_HEALTH_FAULT_DEPENDENCY;
            break;
        default:
            break;
        }
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return appliance_service_api_snapshot_init(snapshot, &manager) ==
           APPLIANCE_SERVICE_API_OK;
}

static bool appliance_production_management_api_health_snapshot(
    const struct appliance_production_management_api_evidence *evidence,
    struct appliance_health_api_snapshot *snapshot)
{
    struct appliance_health_input input;

    appliance_production_management_api_health_input(evidence, &input);
    memset(snapshot, 0, sizeof(*snapshot));
    return appliance_health_api_snapshot_init(snapshot, &input) ==
           APPLIANCE_HEALTH_API_OK;
}

static void appliance_production_management_api_copy_response(
    struct appliance_production_management_api_result *result_out,
    const struct http_response *response,
    const uint8_t *body)
{
    uint32_t length = response->body_length;

    if (length > APPLIANCE_PRODUCTION_MANAGEMENT_API_BODY_MAX) {
        length = APPLIANCE_PRODUCTION_MANAGEMENT_API_BODY_MAX;
    }
    if (body != NULL && length != 0U) {
        memcpy(result_out->body, body, length);
    }
    result_out->response.status = response->status;
    result_out->response.content_type = response->content_type;
    result_out->response.body = length != 0U ? result_out->body : NULL;
    result_out->response.body_length = length;
}

enum appliance_production_management_api_status
appliance_production_management_api_dispatch(
    const struct appliance_production_management_api *owner,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    struct appliance_production_management_api_result *result_out)
{
    if (owner == NULL || target == NULL || result_out == NULL) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL;
    }
    if (!appliance_production_management_api_published(owner)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_STATE;
    }
    if (!appliance_production_management_api_owns(
            owner, target, target_length)) {
        return APPLIANCE_PRODUCTION_MANAGEMENT_API_NOT_OWNED;
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->version = APPLIANCE_PRODUCTION_MANAGEMENT_API_VERSION;

    if (appliance_production_management_api_is_system(
            target, target_length)) {
        struct appliance_system_api_snapshot snapshot;
        struct appliance_system_api_result result;
        enum appliance_system_api_status status;

        if (!appliance_production_management_api_system_snapshot(
                &owner->evidence, &snapshot)) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        memset(&result, 0, sizeof(result));
        status = appliance_system_api_dispatch(
            method, target, target_length, &snapshot, &result);
        if (status != APPLIANCE_SYSTEM_API_OK &&
            status != APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        appliance_production_management_api_copy_response(
            result_out, &result.response, result.body);
        return status == APPLIANCE_SYSTEM_API_OK
                   ? APPLIANCE_PRODUCTION_MANAGEMENT_API_OK
                   : APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED;
    }

    if (appliance_production_management_api_is_network(
            target, target_length)) {
        struct appliance_network_api_snapshot snapshot;
        struct appliance_network_api_result result;
        enum appliance_network_api_status status;

        if (!appliance_production_management_api_network_snapshot(
                &owner->evidence, &snapshot)) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        memset(&result, 0, sizeof(result));
        status = appliance_network_api_dispatch(
            method, target, target_length, &snapshot, &result);
        if (status != APPLIANCE_NETWORK_API_OK &&
            status != APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        appliance_production_management_api_copy_response(
            result_out, &result.response, result.body);
        return status == APPLIANCE_NETWORK_API_OK
                   ? APPLIANCE_PRODUCTION_MANAGEMENT_API_OK
                   : APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED;
    }

    if (appliance_production_management_api_is_service(
            target, target_length)) {
        struct appliance_service_api_snapshot snapshot;
        struct appliance_service_api_result result;
        enum appliance_service_api_status status;

        if (!appliance_production_management_api_service_snapshot(
                &owner->evidence, &snapshot)) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        memset(&result, 0, sizeof(result));
        status = appliance_service_api_dispatch(
            method, target, target_length, &snapshot, &result);
        if (status != APPLIANCE_SERVICE_API_OK &&
            status != APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        appliance_production_management_api_copy_response(
            result_out, &result.response, result.body);
        return status == APPLIANCE_SERVICE_API_OK
                   ? APPLIANCE_PRODUCTION_MANAGEMENT_API_OK
                   : APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED;
    }

    {
        struct appliance_health_api_snapshot snapshot;
        struct appliance_health_api_result result;
        enum appliance_health_api_status status;

        if (!appliance_production_management_api_health_snapshot(
                &owner->evidence, &snapshot)) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        memset(&result, 0, sizeof(result));
        status = appliance_health_api_dispatch(
            method, target, target_length, &snapshot, &result);
        if (status != APPLIANCE_HEALTH_API_OK &&
            status != APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED) {
            return APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE;
        }
        appliance_production_management_api_copy_response(
            result_out, &result.response, result.body);
        return status == APPLIANCE_HEALTH_API_OK
                   ? APPLIANCE_PRODUCTION_MANAGEMENT_API_OK
                   : APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED;
    }
}

const char *appliance_production_management_api_status_string(
    enum appliance_production_management_api_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_OK:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_OK";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_STATE:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_STATE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_NOT_OWNED:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_NOT_OWNED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED";
    case APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE:
        return "APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE";
    default:
        return NULL;
    }
}
