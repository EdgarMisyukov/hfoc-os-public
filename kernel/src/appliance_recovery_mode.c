#include "appliance_recovery_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool appliance_recovery_mode_domain_valid(uint32_t domain)
{
    return domain >= APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION &&
           domain <= APPLIANCE_RECOVERY_MODE_DOMAIN_MAX;
}

static bool appliance_recovery_mode_config_plan_valid(
    enum appliance_config_integrity_recovery recovery)
{
    return recovery >= APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE &&
           recovery <= APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR;
}

static void appliance_recovery_mode_zero_bytes(
    char *destination,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool appliance_recovery_mode_action_mutating(uint32_t action)
{
    return action ==
               APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD ||
           action == APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT ||
           action == APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK ||
           action == APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART;
}

static uint32_t appliance_recovery_mode_raw_action(
    const struct appliance_recovery_mode_evidence *evidence)
{
    switch (evidence->domain) {
    case APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION:
        switch (evidence->config_recovery) {
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD:
            return APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD;
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING:
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID:
            return APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT;
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR:
            return APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY;
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE:
        case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT:
        default:
            return APPLIANCE_RECOVERY_MODE_ACTION_NONE;
        }
    case APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE:
        if (evidence->update_verified &&
            evidence->pending_update &&
            evidence->first_boot_health_failed) {
            return APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK;
        }
        return APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY;
    case APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME:
    default:
        return evidence->restart_available
            ? APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART
            : APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY;
    }
}

enum appliance_recovery_mode_status appliance_recovery_mode_init(
    struct appliance_recovery_mode_gate *gate)
{
    if (gate == NULL) {
        return APPLIANCE_RECOVERY_MODE_NULL;
    }
    if (gate->version == APPLIANCE_RECOVERY_MODE_VERSION ||
        gate->evaluated) {
        return APPLIANCE_RECOVERY_MODE_BAD_STATE;
    }
    appliance_recovery_mode_zero_bytes(
        (char *)gate, sizeof(*gate));
    gate->version = APPLIANCE_RECOVERY_MODE_VERSION;
    return APPLIANCE_RECOVERY_MODE_OK;
}

enum appliance_recovery_mode_status appliance_recovery_mode_evaluate(
    struct appliance_recovery_mode_gate *gate,
    const struct appliance_recovery_mode_evidence *evidence,
    struct appliance_recovery_mode_decision *decision_out)
{
    struct appliance_recovery_mode_gate draft;
    struct appliance_recovery_mode_decision decision;
    uint32_t action;

    if (gate == NULL || evidence == NULL || decision_out == NULL) {
        return APPLIANCE_RECOVERY_MODE_NULL;
    }
    if (gate->version != APPLIANCE_RECOVERY_MODE_VERSION ||
        gate->evaluated) {
        return APPLIANCE_RECOVERY_MODE_BAD_STATE;
    }
    if (evidence->version != APPLIANCE_RECOVERY_MODE_VERSION ||
        !appliance_recovery_mode_domain_valid(evidence->domain) ||
        !appliance_recovery_mode_config_plan_valid(
            evidence->config_recovery)) {
        return APPLIANCE_RECOVERY_MODE_BAD_REQUEST;
    }

    action = appliance_recovery_mode_raw_action(evidence);
    if (evidence->recovery_trust_active &&
        appliance_recovery_mode_action_mutating(action)) {
        action = APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY;
    }

    appliance_recovery_mode_zero_bytes(
        (char *)&decision, sizeof(decision));
    decision.version = APPLIANCE_RECOVERY_MODE_VERSION;
    decision.domain = evidence->domain;
    decision.action = action;
    decision.local_only = true;
    decision.requires_reboot =
        action == APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK;

    appliance_recovery_mode_zero_bytes(
        (char *)&draft, sizeof(draft));
    draft.version = APPLIANCE_RECOVERY_MODE_VERSION;
    draft.evaluated = true;
    draft.decision = decision;
    *gate = draft;
    *decision_out = decision;
    return APPLIANCE_RECOVERY_MODE_OK;
}

bool appliance_recovery_mode_is_evaluated(
    const struct appliance_recovery_mode_gate *gate)
{
    return gate != NULL &&
           gate->version == APPLIANCE_RECOVERY_MODE_VERSION &&
           gate->evaluated;
}

const char *appliance_recovery_mode_status_string(
    enum appliance_recovery_mode_status status)
{
    switch (status) {
    case APPLIANCE_RECOVERY_MODE_OK:
        return "APPLIANCE_RECOVERY_MODE_OK";
    case APPLIANCE_RECOVERY_MODE_NULL:
        return "APPLIANCE_RECOVERY_MODE_NULL";
    case APPLIANCE_RECOVERY_MODE_BAD_STATE:
        return "APPLIANCE_RECOVERY_MODE_BAD_STATE";
    case APPLIANCE_RECOVERY_MODE_BAD_REQUEST:
        return "APPLIANCE_RECOVERY_MODE_BAD_REQUEST";
    default:
        return NULL;
    }
}

const char *appliance_recovery_mode_domain_string(uint32_t domain)
{
    switch (domain) {
    case APPLIANCE_RECOVERY_MODE_DOMAIN_NONE:
        return "APPLIANCE_RECOVERY_MODE_DOMAIN_NONE";
    case APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION:
        return "APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION";
    case APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE:
        return "APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE";
    case APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME:
        return "APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME";
    default:
        return NULL;
    }
}

const char *appliance_recovery_mode_action_string(uint32_t action)
{
    switch (action) {
    case APPLIANCE_RECOVERY_MODE_ACTION_NONE:
        return "APPLIANCE_RECOVERY_MODE_ACTION_NONE";
    case APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD:
        return "APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD";
    case APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT:
        return "APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT";
    case APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK:
        return "APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK";
    case APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART:
        return "APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART";
    case APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY:
        return "APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY";
    default:
        return NULL;
    }
}
