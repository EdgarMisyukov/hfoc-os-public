#include "verification_policy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void verification_policy_clear_pending(
    struct verification_policy *policy)
{
    memset(&policy->pending, 0, sizeof(policy->pending));
    policy->has_pending = false;
}

enum verification_policy_status verification_policy_snapshot_validate(
    const struct verification_policy_snapshot *snapshot)
{
    uint32_t index;
    uint32_t previous;

    if (snapshot == NULL) {
        return VERIFICATION_POLICY_NULL;
    }
    if (snapshot->version != VERIFICATION_POLICY_VERSION) {
        return VERIFICATION_POLICY_INVALID_VERSION;
    }
    if (snapshot->generation < VERIFICATION_POLICY_GENERATION_MIN ||
        snapshot->generation > VERIFICATION_POLICY_GENERATION_MAX) {
        return VERIFICATION_POLICY_INVALID_GENERATION;
    }
    if (snapshot->anchor_count < VERIFICATION_POLICY_ANCHOR_MIN ||
        snapshot->anchor_count > VERIFICATION_POLICY_ANCHOR_MAX) {
        return VERIFICATION_POLICY_INVALID_ANCHOR_COUNT;
    }
    for (index = 0U; index < snapshot->anchor_count; ++index) {
        if (trust_anchor_validate(&snapshot->anchors[index]) !=
            TRUST_ANCHOR_OK) {
            return VERIFICATION_POLICY_INVALID_ANCHOR;
        }
        if (snapshot->anchors[index].state !=
                TRUST_ANCHOR_STATE_ACTIVE &&
            snapshot->anchors[index].state !=
                TRUST_ANCHOR_STATE_REVOKED) {
            return VERIFICATION_POLICY_NON_ACTIVE_ANCHOR;
        }
    }
    for (index = 0U; index < snapshot->anchor_count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (snapshot->anchors[index].id ==
                snapshot->anchors[previous].id) {
                return VERIFICATION_POLICY_DUPLICATE_ANCHOR;
            }
        }
    }
    return VERIFICATION_POLICY_OK;
}

enum verification_policy_status verification_policy_init(
    struct verification_policy *policy,
    const struct verification_policy_snapshot *initial)
{
    enum verification_policy_status status;

    if (policy == NULL || initial == NULL) {
        return VERIFICATION_POLICY_NULL;
    }
    if (policy->has_active) {
        return VERIFICATION_POLICY_BAD_STATE;
    }
    status = verification_policy_snapshot_validate(initial);
    if (status != VERIFICATION_POLICY_OK) {
        return status;
    }

    memset(policy, 0, sizeof(*policy));
    policy->active = *initial;
    policy->has_active = true;
    return VERIFICATION_POLICY_OK;
}

enum verification_policy_status verification_policy_stage(
    struct verification_policy *policy,
    const struct verification_policy_snapshot *candidate)
{
    enum verification_policy_status status;

    if (policy == NULL || candidate == NULL) {
        return VERIFICATION_POLICY_NULL;
    }
    if (!policy->has_active) {
        return VERIFICATION_POLICY_BAD_STATE;
    }

    verification_policy_clear_pending(policy);
    status = verification_policy_snapshot_validate(candidate);
    if (status != VERIFICATION_POLICY_OK) {
        return status;
    }

    policy->pending = *candidate;
    policy->has_pending = true;
    return VERIFICATION_POLICY_OK;
}

enum verification_policy_status verification_policy_commit(
    struct verification_policy *policy)
{
    enum verification_policy_status status;

    if (policy == NULL) {
        return VERIFICATION_POLICY_NULL;
    }
    if (!policy->has_active) {
        return VERIFICATION_POLICY_BAD_STATE;
    }
    if (!policy->has_pending) {
        return VERIFICATION_POLICY_NO_PENDING;
    }

    status = verification_policy_snapshot_validate(&policy->pending);
    if (status != VERIFICATION_POLICY_OK) {
        verification_policy_clear_pending(policy);
        return status;
    }

    policy->active = policy->pending;
    verification_policy_clear_pending(policy);
    return VERIFICATION_POLICY_OK;
}

enum verification_policy_status verification_policy_rollback(
    struct verification_policy *policy)
{
    if (policy == NULL) {
        return VERIFICATION_POLICY_NULL;
    }
    if (!policy->has_active) {
        return VERIFICATION_POLICY_BAD_STATE;
    }
    if (!policy->has_pending) {
        return VERIFICATION_POLICY_NO_PENDING;
    }

    verification_policy_clear_pending(policy);
    return VERIFICATION_POLICY_OK;
}

const struct verification_policy_snapshot *verification_policy_active(
    const struct verification_policy *policy)
{
    return policy != NULL && policy->has_active &&
           verification_policy_snapshot_validate(&policy->active) ==
               VERIFICATION_POLICY_OK
        ? &policy->active
        : NULL;
}

bool verification_policy_has_pending(
    const struct verification_policy *policy)
{
    return policy != NULL && policy->has_pending;
}

const char *verification_policy_status_string(
    enum verification_policy_status status)
{
    switch (status) {
    case VERIFICATION_POLICY_OK:
        return "VERIFICATION_POLICY_OK";
    case VERIFICATION_POLICY_NULL:
        return "VERIFICATION_POLICY_NULL";
    case VERIFICATION_POLICY_BAD_STATE:
        return "VERIFICATION_POLICY_BAD_STATE";
    case VERIFICATION_POLICY_NO_PENDING:
        return "VERIFICATION_POLICY_NO_PENDING";
    case VERIFICATION_POLICY_INVALID_VERSION:
        return "VERIFICATION_POLICY_INVALID_VERSION";
    case VERIFICATION_POLICY_INVALID_GENERATION:
        return "VERIFICATION_POLICY_INVALID_GENERATION";
    case VERIFICATION_POLICY_INVALID_ANCHOR_COUNT:
        return "VERIFICATION_POLICY_INVALID_ANCHOR_COUNT";
    case VERIFICATION_POLICY_INVALID_ANCHOR:
        return "VERIFICATION_POLICY_INVALID_ANCHOR";
    case VERIFICATION_POLICY_DUPLICATE_ANCHOR:
        return "VERIFICATION_POLICY_DUPLICATE_ANCHOR";
    case VERIFICATION_POLICY_NON_ACTIVE_ANCHOR:
        return "VERIFICATION_POLICY_NON_ACTIVE_ANCHOR";
    default:
        return NULL;
    }
}
