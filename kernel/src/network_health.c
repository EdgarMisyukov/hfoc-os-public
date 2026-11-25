#include "network_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void network_health_zero_bytes(unsigned char *bytes,
                                      size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static bool network_health_device_valid(
    const struct network_device_selection *selection)
{
    return selection != NULL &&
           selection->published &&
           selection->version == NETWORK_DEVICE_SELECTION_VERSION &&
           selection->generation >= NETWORK_DEVICE_GENERATION_MIN &&
           selection->generation <= NETWORK_DEVICE_GENERATION_MAX &&
           network_device_snapshot_validate(&selection->device) ==
               NETWORK_DEVICE_OK;
}

static bool network_health_link_valid(
    const struct network_link_selection *selection)
{
    return selection != NULL &&
           selection->published &&
           selection->version == NETWORK_LINK_SELECTION_VERSION &&
           selection->generation >= NETWORK_LINK_GENERATION_MIN &&
           selection->generation <= NETWORK_LINK_GENERATION_MAX &&
           network_link_state_validate(&selection->link) ==
               NETWORK_LINK_OK;
}

static bool network_health_link_consistent(
    const struct network_device_selection *device,
    const struct network_link_selection *link)
{
    return link->link.device_id == device->device.device_id &&
           (device->device.capability_mask &
            NETWORK_DEVICE_CAPABILITY_LINK_STATE) != 0U;
}

static bool network_health_config_valid(
    const struct network_config_transaction *config)
{
    if (config == NULL ||
        config->version != NETWORK_CONFIG_TRANSACTION_VERSION ||
        !config->has_active ||
        network_ipv4_config_validate(&config->active) !=
            NETWORK_IPV4_OK) {
        return false;
    }
    return !config->has_pending ||
           network_ipv4_config_validate(&config->pending) ==
               NETWORK_IPV4_OK;
}

static bool network_health_admission_valid(
    const struct network_admission_selection *admission)
{
    return network_admission_selection_published(admission);
}

enum network_health_status network_health_evaluate(
    const struct network_health_input *input,
    struct network_health_verdict *verdict_out)
{
    enum health_check_result_state result =
        HEALTH_CHECK_RESULT_PASS;
    enum network_health_fault fault =
        NETWORK_HEALTH_FAULT_NONE;

    if (input == NULL || verdict_out == NULL) {
        return NETWORK_HEALTH_NULL;
    }
    if (input->version != NETWORK_HEALTH_VERSION) {
        return NETWORK_HEALTH_BAD_VERSION;
    }
    if (input->device == NULL ||
        input->link == NULL ||
        input->config == NULL ||
        input->admission == NULL) {
        return NETWORK_HEALTH_NULL;
    }
    if (!network_health_device_valid(input->device) ||
        !network_health_link_valid(input->link) ||
        !network_health_link_consistent(
            input->device, input->link) ||
        !network_health_config_valid(input->config) ||
        !network_health_admission_valid(input->admission)) {
        return NETWORK_HEALTH_BAD_STATE;
    }

    network_health_zero_bytes(
        (unsigned char *)verdict_out, sizeof(*verdict_out));
    verdict_out->version = NETWORK_HEALTH_VERSION;
    verdict_out->device_id = input->device->device.device_id;
    verdict_out->link_state = input->link->link.state;
    verdict_out->link_generation = input->link->link.generation;
    verdict_out->has_active_config = input->config->has_active;
    verdict_out->has_pending_config = input->config->has_pending;
    verdict_out->admission_published = true;
    verdict_out->admission_mode =
        input->admission->policy.mode;
    verdict_out->local_control_available = true;

    switch (input->link->link.state) {
    case NETWORK_LINK_UP:
        result = HEALTH_CHECK_RESULT_PASS;
        break;
    case NETWORK_LINK_INITIALIZING:
        result = HEALTH_CHECK_RESULT_UNKNOWN;
        break;
    case NETWORK_LINK_DOWN:
    case NETWORK_LINK_FAILED:
        result = HEALTH_CHECK_RESULT_FAIL;
        fault = NETWORK_HEALTH_FAULT_LINK;
        break;
    default:
        return NETWORK_HEALTH_BAD_STATE;
    }

    if (input->config->has_pending &&
        result != HEALTH_CHECK_RESULT_FAIL) {
        result = HEALTH_CHECK_RESULT_WARN;
        if (fault == NETWORK_HEALTH_FAULT_NONE) {
            fault = NETWORK_HEALTH_FAULT_CONFIG;
        }
    }

    if (input->admission->policy.mode ==
            NETWORK_ADMISSION_MODE_DENY_ALL &&
        result == HEALTH_CHECK_RESULT_PASS) {
        result = HEALTH_CHECK_RESULT_UNKNOWN;
    }

    verdict_out->result = result;
    verdict_out->fault = fault;
    if (result == HEALTH_CHECK_RESULT_FAIL) {
        verdict_out->healthy = false;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_CRITICAL;
    } else if (result == HEALTH_CHECK_RESULT_WARN) {
        verdict_out->healthy = false;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_WARNING;
    } else {
        verdict_out->healthy =
            result == HEALTH_CHECK_RESULT_PASS;
        verdict_out->severity = HEALTH_CHECK_SEVERITY_INFO;
    }
    return NETWORK_HEALTH_OK;
}

const char *network_health_status_string(
    enum network_health_status status)
{
    switch (status) {
    case NETWORK_HEALTH_OK:
        return "NETWORK_HEALTH_OK";
    case NETWORK_HEALTH_NULL:
        return "NETWORK_HEALTH_NULL";
    case NETWORK_HEALTH_BAD_VERSION:
        return "NETWORK_HEALTH_BAD_VERSION";
    case NETWORK_HEALTH_BAD_STATE:
        return "NETWORK_HEALTH_BAD_STATE";
    default:
        return NULL;
    }
}

const char *network_health_fault_string(
    enum network_health_fault fault)
{
    switch (fault) {
    case NETWORK_HEALTH_FAULT_NONE:
        return "NETWORK_HEALTH_FAULT_NONE";
    case NETWORK_HEALTH_FAULT_LINK:
        return "NETWORK_HEALTH_FAULT_LINK";
    case NETWORK_HEALTH_FAULT_CONFIG:
        return "NETWORK_HEALTH_FAULT_CONFIG";
    case NETWORK_HEALTH_FAULT_ADMISSION:
        return "NETWORK_HEALTH_FAULT_ADMISSION";
    default:
        return NULL;
    }
}
