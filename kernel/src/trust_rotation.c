#include "trust_rotation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void trust_rotation_clear(
    struct trust_rotation *rotation)
{
    memset(&rotation->staged, 0, sizeof(rotation->staged));
    memset(&rotation->prepared, 0, sizeof(rotation->prepared));
    rotation->has_staged = false;
    rotation->has_prepared = false;
}

static bool trust_rotation_valid(
    const struct trust_rotation *rotation)
{
    return rotation != NULL &&
           rotation->version == TRUST_ROTATION_VERSION;
}

static bool trust_rotation_anchor_in_policy(
    const struct verification_policy_snapshot *snapshot,
    uint32_t anchor_id)
{
    uint32_t index;

    for (index = 0U; index < snapshot->anchor_count; ++index) {
        if (snapshot->anchors[index].id == anchor_id) {
            return true;
        }
    }
    return false;
}

static enum trust_rotation_status trust_rotation_build_candidate(
    const struct trust_rotation *rotation,
    const struct verification_policy_snapshot *active,
    struct verification_policy_snapshot *candidate)
{
    enum verification_policy_status policy_status;

    if (active->anchor_count >= VERIFICATION_POLICY_ANCHOR_MAX) {
        return TRUST_ROTATION_POLICY_FULL;
    }
    if (active->generation ==
        VERIFICATION_POLICY_GENERATION_MAX) {
        return TRUST_ROTATION_GENERATION_OVERFLOW;
    }

    *candidate = *active;
    candidate->generation = active->generation + 1U;
    candidate->anchors[candidate->anchor_count] = rotation->staged;
    candidate->anchors[candidate->anchor_count].state =
        TRUST_ANCHOR_STATE_ACTIVE;
    ++candidate->anchor_count;

    policy_status =
        verification_policy_snapshot_validate(candidate);
    if (policy_status != VERIFICATION_POLICY_OK) {
        return TRUST_ROTATION_BAD_STATE;
    }
    return TRUST_ROTATION_OK;
}

enum trust_rotation_status trust_rotation_init(
    struct trust_rotation *rotation)
{
    if (rotation == NULL) {
        return TRUST_ROTATION_NULL;
    }
    memset(rotation, 0, sizeof(*rotation));
    rotation->version = TRUST_ROTATION_VERSION;
    return TRUST_ROTATION_OK;
}

enum trust_rotation_status trust_rotation_stage(
    struct trust_rotation *rotation,
    const struct verification_policy *policy,
    const struct trust_anchor *anchor)
{
    const struct verification_policy_snapshot *active;

    if (rotation == NULL || policy == NULL || anchor == NULL) {
        return TRUST_ROTATION_NULL;
    }
    if (!trust_rotation_valid(rotation)) {
        return TRUST_ROTATION_BAD_STATE;
    }
    if (rotation->has_staged || rotation->has_prepared) {
        return TRUST_ROTATION_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return TRUST_ROTATION_NO_POLICY;
    }
    if (trust_anchor_validate(anchor) != TRUST_ANCHOR_OK) {
        return TRUST_ROTATION_INVALID_ANCHOR;
    }
    if (anchor->state != TRUST_ANCHOR_STATE_INACTIVE) {
        return TRUST_ROTATION_NON_INACTIVE_ANCHOR;
    }
    if (trust_rotation_anchor_in_policy(active, anchor->id)) {
        return TRUST_ROTATION_DUPLICATE_ANCHOR;
    }

    rotation->staged = *anchor;
    rotation->has_staged = true;
    return TRUST_ROTATION_OK;
}

enum trust_rotation_status trust_rotation_prepare(
    struct trust_rotation *rotation,
    struct verification_policy *policy)
{
    const struct verification_policy_snapshot *active;
    struct verification_policy_snapshot candidate;
    enum trust_rotation_status rotation_status;
    enum verification_policy_status policy_status;

    if (rotation == NULL || policy == NULL) {
        return TRUST_ROTATION_NULL;
    }
    if (!trust_rotation_valid(rotation)) {
        return TRUST_ROTATION_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return TRUST_ROTATION_NO_POLICY;
    }
    if (!rotation->has_staged) {
        return TRUST_ROTATION_NO_STAGED;
    }
    if (rotation->has_prepared) {
        return TRUST_ROTATION_BAD_STATE;
    }

    rotation_status = trust_rotation_build_candidate(
        rotation, active, &candidate);
    if (rotation_status != TRUST_ROTATION_OK) {
        return rotation_status;
    }

    policy_status = verification_policy_stage(policy, &candidate);
    if (policy_status != VERIFICATION_POLICY_OK) {
        return TRUST_ROTATION_STAGE_FAILED;
    }

    rotation->prepared = candidate;
    rotation->has_prepared = true;
    return TRUST_ROTATION_OK;
}

enum trust_rotation_status trust_rotation_activate(
    struct trust_rotation *rotation,
    struct verification_policy *policy)
{
    enum verification_policy_status policy_status;

    if (rotation == NULL || policy == NULL) {
        return TRUST_ROTATION_NULL;
    }
    if (!trust_rotation_valid(rotation)) {
        return TRUST_ROTATION_BAD_STATE;
    }
    if (verification_policy_active(policy) == NULL) {
        return TRUST_ROTATION_NO_POLICY;
    }
    if (!rotation->has_staged) {
        return TRUST_ROTATION_NO_STAGED;
    }
    if (!rotation->has_prepared) {
        return TRUST_ROTATION_NO_PREPARED;
    }
    if (!verification_policy_has_pending(policy) ||
        memcmp(&policy->pending, &rotation->prepared,
               sizeof(policy->pending)) != 0) {
        return TRUST_ROTATION_BAD_STATE;
    }

    policy_status = verification_policy_commit(policy);
    if (policy_status != VERIFICATION_POLICY_OK) {
        trust_rotation_clear(rotation);
        return TRUST_ROTATION_COMMIT_FAILED;
    }

    trust_rotation_clear(rotation);
    return TRUST_ROTATION_OK;
}

enum trust_rotation_status trust_rotation_rollback(
    struct trust_rotation *rotation,
    struct verification_policy *policy)
{
    enum verification_policy_status policy_status;

    if (rotation == NULL || policy == NULL) {
        return TRUST_ROTATION_NULL;
    }
    if (!trust_rotation_valid(rotation)) {
        return TRUST_ROTATION_BAD_STATE;
    }
    if (verification_policy_active(policy) == NULL) {
        return TRUST_ROTATION_NO_POLICY;
    }
    if (rotation->has_prepared) {
        if (verification_policy_has_pending(policy)) {
            if (memcmp(&policy->pending, &rotation->prepared,
                       sizeof(policy->pending)) != 0) {
                return TRUST_ROTATION_BAD_STATE;
            }
            policy_status = verification_policy_rollback(policy);
            if (policy_status != VERIFICATION_POLICY_OK) {
                return TRUST_ROTATION_BAD_STATE;
            }
        }
    }

    trust_rotation_clear(rotation);
    return TRUST_ROTATION_OK;
}

bool trust_rotation_has_staged(
    const struct trust_rotation *rotation)
{
    return trust_rotation_valid(rotation) && rotation->has_staged;
}

bool trust_rotation_has_prepared(
    const struct trust_rotation *rotation)
{
    return trust_rotation_valid(rotation) && rotation->has_prepared;
}

const struct trust_anchor *trust_rotation_staged(
    const struct trust_rotation *rotation)
{
    return trust_rotation_has_staged(rotation)
        ? &rotation->staged
        : NULL;
}

const struct verification_policy_snapshot *trust_rotation_prepared(
    const struct trust_rotation *rotation)
{
    return trust_rotation_has_prepared(rotation)
        ? &rotation->prepared
        : NULL;
}

const char *trust_rotation_status_string(
    enum trust_rotation_status status)
{
    switch (status) {
    case TRUST_ROTATION_OK:
        return "TRUST_ROTATION_OK";
    case TRUST_ROTATION_NULL:
        return "TRUST_ROTATION_NULL";
    case TRUST_ROTATION_BAD_STATE:
        return "TRUST_ROTATION_BAD_STATE";
    case TRUST_ROTATION_NO_STAGED:
        return "TRUST_ROTATION_NO_STAGED";
    case TRUST_ROTATION_NO_PREPARED:
        return "TRUST_ROTATION_NO_PREPARED";
    case TRUST_ROTATION_NO_POLICY:
        return "TRUST_ROTATION_NO_POLICY";
    case TRUST_ROTATION_INVALID_ANCHOR:
        return "TRUST_ROTATION_INVALID_ANCHOR";
    case TRUST_ROTATION_NON_INACTIVE_ANCHOR:
        return "TRUST_ROTATION_NON_INACTIVE_ANCHOR";
    case TRUST_ROTATION_DUPLICATE_ANCHOR:
        return "TRUST_ROTATION_DUPLICATE_ANCHOR";
    case TRUST_ROTATION_POLICY_FULL:
        return "TRUST_ROTATION_POLICY_FULL";
    case TRUST_ROTATION_GENERATION_OVERFLOW:
        return "TRUST_ROTATION_GENERATION_OVERFLOW";
    case TRUST_ROTATION_STAGE_FAILED:
        return "TRUST_ROTATION_STAGE_FAILED";
    case TRUST_ROTATION_COMMIT_FAILED:
        return "TRUST_ROTATION_COMMIT_FAILED";
    default:
        return NULL;
    }
}
