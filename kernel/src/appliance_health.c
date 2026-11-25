#include "appliance_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool appliance_health_state_valid(
    enum appliance_health_state state)
{
    return state == APPLIANCE_HEALTH_STATE_UNKNOWN ||
           state == APPLIANCE_HEALTH_STATE_PASS ||
           state == APPLIANCE_HEALTH_STATE_WARN ||
           state == APPLIANCE_HEALTH_STATE_FAIL;
}

static bool appliance_health_domain_valid(
    enum appliance_health_domain domain)
{
    return domain >= APPLIANCE_HEALTH_DOMAIN_BOOT &&
           domain <= APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT;
}

static bool appliance_health_fault_valid_for_domain(
    enum appliance_health_domain domain,
    enum appliance_health_fault fault)
{
    switch (domain) {
    case APPLIANCE_HEALTH_DOMAIN_BOOT:
        return fault == APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE ||
               fault == APPLIANCE_HEALTH_BOOT_FAULT_PANIC;
    case APPLIANCE_HEALTH_DOMAIN_STORAGE:
        return fault == APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL ||
               fault == APPLIANCE_HEALTH_STORAGE_FAULT_CHECKPOINT ||
               fault == APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP;
    case APPLIANCE_HEALTH_DOMAIN_CONFIGURATION:
        return fault == APPLIANCE_HEALTH_CONFIG_FAULT_MISSING ||
               fault == APPLIANCE_HEALTH_CONFIG_FAULT_INVALID ||
               fault == APPLIANCE_HEALTH_CONFIG_FAULT_COMMIT;
    case APPLIANCE_HEALTH_DOMAIN_NETWORK:
        return fault == APPLIANCE_HEALTH_NETWORK_FAULT_LINK ||
               fault == APPLIANCE_HEALTH_NETWORK_FAULT_ADDRESS ||
               fault == APPLIANCE_HEALTH_NETWORK_FAULT_REACHABILITY;
    case APPLIANCE_HEALTH_DOMAIN_SERVICE:
        return fault == APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY ||
               fault == APPLIANCE_HEALTH_SERVICE_FAULT_STOPPED ||
               fault == APPLIANCE_HEALTH_SERVICE_FAULT_FAILED;
    case APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT:
        return fault == APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER ||
               fault == APPLIANCE_HEALTH_HTTPS_FAULT_AUTH ||
               fault == APPLIANCE_HEALTH_HTTPS_FAULT_UNAVAILABLE;
    default:
        return false;
    }
}

static bool appliance_health_started_valid(
    const struct appliance_health_input *input)
{
    return input != NULL &&
           input->version == APPLIANCE_HEALTH_VERSION;
}

static enum appliance_health_status appliance_health_verdict_validate(
    const struct appliance_health_verdict *verdict,
    enum appliance_health_domain expected_domain)
{
    if (verdict->version != APPLIANCE_HEALTH_VERSION) {
        return APPLIANCE_HEALTH_STATUS_BAD_VERSION;
    }
    if (verdict->domain != expected_domain ||
        !appliance_health_domain_valid(verdict->domain)) {
        return APPLIANCE_HEALTH_STATUS_BAD_DOMAIN;
    }
    if (!appliance_health_state_valid(verdict->state)) {
        return APPLIANCE_HEALTH_STATUS_BAD_STATE;
    }
    if (verdict->state == APPLIANCE_HEALTH_STATE_PASS ||
        verdict->state == APPLIANCE_HEALTH_STATE_UNKNOWN) {
        if (verdict->fault != APPLIANCE_HEALTH_FAULT_NONE) {
            return APPLIANCE_HEALTH_STATUS_BAD_FAULT;
        }
        return APPLIANCE_HEALTH_STATUS_OK;
    }
    if (!appliance_health_fault_valid_for_domain(
            expected_domain, verdict->fault)) {
        return APPLIANCE_HEALTH_STATUS_BAD_FAULT;
    }
    return APPLIANCE_HEALTH_STATUS_OK;
}

static enum appliance_health_status appliance_health_validate_input(
    const struct appliance_health_input *input)
{
    static const enum appliance_health_domain domains[
        APPLIANCE_HEALTH_DOMAIN_COUNT] = {
        APPLIANCE_HEALTH_DOMAIN_BOOT,
        APPLIANCE_HEALTH_DOMAIN_STORAGE,
        APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
        APPLIANCE_HEALTH_DOMAIN_NETWORK,
        APPLIANCE_HEALTH_DOMAIN_SERVICE,
        APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
    };
    uint32_t index;

    if (!appliance_health_started_valid(input)) {
        return APPLIANCE_HEALTH_STATUS_BAD_VERSION;
    }
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        enum appliance_health_status status =
            appliance_health_verdict_validate(
                &input->domains[index], domains[index]);

        if (status != APPLIANCE_HEALTH_STATUS_OK) {
            return status;
        }
    }
    return APPLIANCE_HEALTH_STATUS_OK;
}

static enum appliance_health_state appliance_health_result_for_input(
    const struct appliance_health_input *input)
{
    uint32_t index;

    if (input->critical) {
        return APPLIANCE_HEALTH_STATE_FAILED;
    }
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        if (input->domains[index].state == APPLIANCE_HEALTH_STATE_UNKNOWN) {
            return APPLIANCE_HEALTH_STATE_UNKNOWN;
        }
    }
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        if (input->domains[index].state == APPLIANCE_HEALTH_STATE_FAIL) {
            return APPLIANCE_HEALTH_STATE_FAILED;
        }
    }
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        if (input->domains[index].state == APPLIANCE_HEALTH_STATE_WARN) {
            return APPLIANCE_HEALTH_STATE_DEGRADED;
        }
    }
    return APPLIANCE_HEALTH_STATE_OK;
}

static void appliance_health_find_first_fault(
    const struct appliance_health_input *input,
    enum appliance_health_domain *domain_out,
    enum appliance_health_fault *fault_out)
{
    uint32_t index;

    *domain_out = APPLIANCE_HEALTH_DOMAIN_NONE;
    *fault_out = APPLIANCE_HEALTH_FAULT_NONE;
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        const struct appliance_health_verdict *verdict =
            &input->domains[index];

        if (verdict->state != APPLIANCE_HEALTH_STATE_PASS) {
            *domain_out = verdict->domain;
            *fault_out = verdict->fault;
            return;
        }
    }
}

enum appliance_health_status appliance_health_evaluate(
    const struct appliance_health_input *input,
    struct appliance_health_output *output_out)
{
    enum appliance_health_status status;
    enum appliance_health_domain first_domain;
    enum appliance_health_fault first_fault;

    if (input == NULL || output_out == NULL) {
        return APPLIANCE_HEALTH_STATUS_NULL;
    }
    status = appliance_health_validate_input(input);
    if (status != APPLIANCE_HEALTH_STATUS_OK) {
        return status;
    }

    appliance_health_find_first_fault(
        input, &first_domain, &first_fault);
    output_out->version = APPLIANCE_HEALTH_VERSION;
    output_out->published = true;
    output_out->state = appliance_health_result_for_input(input);
    output_out->healthy =
        output_out->state == APPLIANCE_HEALTH_STATE_OK &&
        !input->critical;
    output_out->critical = input->critical;
    output_out->first_fault_domain = first_domain;
    output_out->first_fault = first_fault;
    return APPLIANCE_HEALTH_STATUS_OK;
}

const char *appliance_health_status_string(
    enum appliance_health_status status)
{
    switch (status) {
    case APPLIANCE_HEALTH_STATUS_OK:
        return "APPLIANCE_HEALTH_STATUS_OK";
    case APPLIANCE_HEALTH_STATUS_NULL:
        return "APPLIANCE_HEALTH_STATUS_NULL";
    case APPLIANCE_HEALTH_STATUS_BAD_VERSION:
        return "APPLIANCE_HEALTH_STATUS_BAD_VERSION";
    case APPLIANCE_HEALTH_STATUS_BAD_STATE:
        return "APPLIANCE_HEALTH_STATUS_BAD_STATE";
    case APPLIANCE_HEALTH_STATUS_BAD_DOMAIN:
        return "APPLIANCE_HEALTH_STATUS_BAD_DOMAIN";
    case APPLIANCE_HEALTH_STATUS_BAD_FAULT:
        return "APPLIANCE_HEALTH_STATUS_BAD_FAULT";
    default:
        return NULL;
    }
}

const char *appliance_health_state_string(
    enum appliance_health_state state)
{
    switch (state) {
    case APPLIANCE_HEALTH_STATE_UNKNOWN:
        return "UNKNOWN";
    case APPLIANCE_HEALTH_STATE_PASS:
        return "OK";
    case APPLIANCE_HEALTH_STATE_WARN:
        return "DEGRADED";
    case APPLIANCE_HEALTH_STATE_FAIL:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_health_domain_string(
    enum appliance_health_domain domain)
{
    switch (domain) {
    case APPLIANCE_HEALTH_DOMAIN_NONE:
        return "NONE";
    case APPLIANCE_HEALTH_DOMAIN_BOOT:
        return "BOOT";
    case APPLIANCE_HEALTH_DOMAIN_STORAGE:
        return "STORAGE";
    case APPLIANCE_HEALTH_DOMAIN_CONFIGURATION:
        return "CONFIGURATION";
    case APPLIANCE_HEALTH_DOMAIN_NETWORK:
        return "NETWORK";
    case APPLIANCE_HEALTH_DOMAIN_SERVICE:
        return "SERVICE";
    case APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT:
        return "HTTPS_MANAGEMENT";
    default:
        return NULL;
    }
}

const char *appliance_health_fault_string(
    enum appliance_health_fault fault)
{
    switch (fault) {
    case APPLIANCE_HEALTH_FAULT_NONE:
        return "NONE";
    case APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE:
        return "INCOMPLETE";
    case APPLIANCE_HEALTH_BOOT_FAULT_PANIC:
        return "PANIC";
    case APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL:
        return "JOURNAL";
    case APPLIANCE_HEALTH_STORAGE_FAULT_CHECKPOINT:
        return "CHECKPOINT";
    case APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP:
        return "BACKUP";
    case APPLIANCE_HEALTH_CONFIG_FAULT_MISSING:
        return "MISSING";
    case APPLIANCE_HEALTH_CONFIG_FAULT_INVALID:
        return "INVALID";
    case APPLIANCE_HEALTH_CONFIG_FAULT_COMMIT:
        return "COMMIT";
    case APPLIANCE_HEALTH_NETWORK_FAULT_LINK:
        return "LINK";
    case APPLIANCE_HEALTH_NETWORK_FAULT_ADDRESS:
        return "ADDRESS";
    case APPLIANCE_HEALTH_NETWORK_FAULT_REACHABILITY:
        return "REACHABILITY";
    case APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY:
        return "NOT_READY";
    case APPLIANCE_HEALTH_SERVICE_FAULT_STOPPED:
        return "STOPPED";
    case APPLIANCE_HEALTH_SERVICE_FAULT_FAILED:
        return "FAILED";
    case APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER:
        return "LISTENER";
    case APPLIANCE_HEALTH_HTTPS_FAULT_AUTH:
        return "AUTH";
    case APPLIANCE_HEALTH_HTTPS_FAULT_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return NULL;
    }
}
