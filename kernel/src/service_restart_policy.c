#include "service_restart_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum service_restart_policy_status service_restart_policy_validate(
    const struct service_restart_policy *policy)
{
    if (policy == NULL) {
        return SERVICE_RESTART_POLICY_NULL;
    }
    if (policy->version != SERVICE_RESTART_POLICY_VERSION) {
        return SERVICE_RESTART_POLICY_BAD_VERSION;
    }
    if (policy->restart_budget > SERVICE_RESTART_BUDGET_MAX) {
        return SERVICE_RESTART_POLICY_BAD_BUDGET;
    }
    if (policy->restart_interval_ticks <
            SERVICE_RESTART_INTERVAL_MIN_TICKS ||
        policy->restart_interval_ticks >
            SERVICE_RESTART_INTERVAL_MAX_TICKS) {
        return SERVICE_RESTART_POLICY_BAD_INTERVAL;
    }
    return SERVICE_RESTART_POLICY_OK;
}

enum service_restart_policy_status service_restart_policy_init(
    struct service_restart_policy *policy,
    uint32_t restart_budget,
    uint64_t restart_interval_ticks,
    bool disabled_after_exhaustion)
{
    if (policy == NULL) {
        return SERVICE_RESTART_POLICY_NULL;
    }
    if (restart_budget > SERVICE_RESTART_BUDGET_MAX) {
        return SERVICE_RESTART_POLICY_BAD_BUDGET;
    }
    if (restart_interval_ticks <
            SERVICE_RESTART_INTERVAL_MIN_TICKS ||
        restart_interval_ticks >
            SERVICE_RESTART_INTERVAL_MAX_TICKS) {
        return SERVICE_RESTART_POLICY_BAD_INTERVAL;
    }

    memset(policy, 0, sizeof(*policy));
    policy->version = SERVICE_RESTART_POLICY_VERSION;
    policy->restart_budget = restart_budget;
    policy->restart_interval_ticks = restart_interval_ticks;
    policy->disabled_after_exhaustion = disabled_after_exhaustion;
    policy->published = true;
    policy->state = SERVICE_RESTART_POLICY_STATE_ACTIVE;
    return SERVICE_RESTART_POLICY_OK;
}

enum service_restart_policy_status service_restart_policy_can_restart(
    const struct service_restart_policy *policy,
    uint64_t now_ticks,
    bool *allowed)
{
    if (policy == NULL || allowed == NULL) {
        return SERVICE_RESTART_POLICY_NULL;
    }
    *allowed = false;
    if (!policy->published ||
        service_restart_policy_validate(policy) !=
            SERVICE_RESTART_POLICY_OK) {
        return SERVICE_RESTART_POLICY_BAD_STATE;
    }
    if (policy->state == SERVICE_RESTART_POLICY_STATE_DISABLED) {
        return SERVICE_RESTART_POLICY_DISABLED;
    }
    if (policy->state == SERVICE_RESTART_POLICY_STATE_EXHAUSTED ||
        policy->restart_count >= policy->restart_budget) {
        return SERVICE_RESTART_POLICY_EXHAUSTED;
    }
    if (policy->restart_interval_ticks > UINT64_MAX - now_ticks) {
        return SERVICE_RESTART_POLICY_TIME_OVERFLOW;
    }
    if (now_ticks < policy->next_allowed_tick) {
        return SERVICE_RESTART_POLICY_INTERVAL;
    }
    *allowed = true;
    return SERVICE_RESTART_POLICY_OK;
}

enum service_restart_policy_status service_restart_policy_record_restart(
    struct service_restart_policy *policy,
    uint64_t now_ticks)
{
    if (policy == NULL) {
        return SERVICE_RESTART_POLICY_NULL;
    }
    if (!policy->published ||
        service_restart_policy_validate(policy) !=
            SERVICE_RESTART_POLICY_OK) {
        return SERVICE_RESTART_POLICY_BAD_STATE;
    }
    if (policy->state == SERVICE_RESTART_POLICY_STATE_DISABLED) {
        return SERVICE_RESTART_POLICY_DISABLED;
    }
    if (policy->state == SERVICE_RESTART_POLICY_STATE_EXHAUSTED ||
        policy->restart_count >= policy->restart_budget) {
        policy->state = policy->disabled_after_exhaustion ?
            SERVICE_RESTART_POLICY_STATE_DISABLED :
            SERVICE_RESTART_POLICY_STATE_EXHAUSTED;
        return SERVICE_RESTART_POLICY_EXHAUSTED;
    }
    if (policy->restart_interval_ticks > UINT64_MAX - now_ticks) {
        return SERVICE_RESTART_POLICY_TIME_OVERFLOW;
    }
    if (now_ticks < policy->next_allowed_tick) {
        return SERVICE_RESTART_POLICY_INTERVAL;
    }

    ++policy->restart_count;
    policy->next_allowed_tick = now_ticks + policy->restart_interval_ticks;
    if (policy->restart_count >= policy->restart_budget) {
        policy->state = policy->disabled_after_exhaustion ?
            SERVICE_RESTART_POLICY_STATE_DISABLED :
            SERVICE_RESTART_POLICY_STATE_EXHAUSTED;
    }
    return SERVICE_RESTART_POLICY_OK;
}

enum service_restart_policy_status service_restart_policy_reset(
    struct service_restart_policy *policy)
{
    if (policy == NULL) {
        return SERVICE_RESTART_POLICY_NULL;
    }
    if (!policy->published ||
        service_restart_policy_validate(policy) !=
            SERVICE_RESTART_POLICY_OK) {
        return SERVICE_RESTART_POLICY_BAD_STATE;
    }
    if (policy->state == SERVICE_RESTART_POLICY_STATE_DISABLED) {
        return SERVICE_RESTART_POLICY_DISABLED;
    }
    if (policy->state != SERVICE_RESTART_POLICY_STATE_EXHAUSTED) {
        return SERVICE_RESTART_POLICY_BAD_STATE;
    }
    policy->restart_count = 0U;
    policy->next_allowed_tick = 0U;
    policy->state = SERVICE_RESTART_POLICY_STATE_ACTIVE;
    return SERVICE_RESTART_POLICY_OK;
}

uint32_t service_restart_policy_restart_count(
    const struct service_restart_policy *policy)
{
    if (policy == NULL || !policy->published) {
        return 0U;
    }
    return policy->restart_count;
}

enum service_restart_policy_state service_restart_policy_state(
    const struct service_restart_policy *policy)
{
    if (policy == NULL || !policy->published) {
        return SERVICE_RESTART_POLICY_STATE_ACTIVE;
    }
    return policy->state;
}

const char *service_restart_policy_status_string(
    enum service_restart_policy_status status)
{
    switch (status) {
    case SERVICE_RESTART_POLICY_OK:
        return "SERVICE_RESTART_POLICY_OK";
    case SERVICE_RESTART_POLICY_NULL:
        return "SERVICE_RESTART_POLICY_NULL";
    case SERVICE_RESTART_POLICY_BAD_STATE:
        return "SERVICE_RESTART_POLICY_BAD_STATE";
    case SERVICE_RESTART_POLICY_BAD_VERSION:
        return "SERVICE_RESTART_POLICY_BAD_VERSION";
    case SERVICE_RESTART_POLICY_BAD_BUDGET:
        return "SERVICE_RESTART_POLICY_BAD_BUDGET";
    case SERVICE_RESTART_POLICY_BAD_INTERVAL:
        return "SERVICE_RESTART_POLICY_BAD_INTERVAL";
    case SERVICE_RESTART_POLICY_INTERVAL:
        return "SERVICE_RESTART_POLICY_INTERVAL";
    case SERVICE_RESTART_POLICY_EXHAUSTED:
        return "SERVICE_RESTART_POLICY_EXHAUSTED";
    case SERVICE_RESTART_POLICY_DISABLED:
        return "SERVICE_RESTART_POLICY_DISABLED";
    case SERVICE_RESTART_POLICY_TIME_OVERFLOW:
        return "SERVICE_RESTART_POLICY_TIME_OVERFLOW";
    default:
        return NULL;
    }
}

const char *service_restart_policy_state_string(
    enum service_restart_policy_state state)
{
    switch (state) {
    case SERVICE_RESTART_POLICY_STATE_ACTIVE:
        return "SERVICE_RESTART_POLICY_STATE_ACTIVE";
    case SERVICE_RESTART_POLICY_STATE_EXHAUSTED:
        return "SERVICE_RESTART_POLICY_STATE_EXHAUSTED";
    case SERVICE_RESTART_POLICY_STATE_DISABLED:
        return "SERVICE_RESTART_POLICY_STATE_DISABLED";
    default:
        return NULL;
    }
}
