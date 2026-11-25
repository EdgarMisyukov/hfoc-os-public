#include "trust_revocation.h"

#include "audit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool trust_revocation_valid(
    const struct trust_revocation *revocation)
{
    return revocation != NULL &&
           revocation->version == TRUST_REVOCATION_VERSION;
}

enum trust_revocation_status trust_revocation_init(
    struct trust_revocation *revocation)
{
    if (revocation == NULL) {
        return TRUST_REVOCATION_NULL;
    }

    memset(revocation, 0, sizeof(*revocation));
    revocation->version = TRUST_REVOCATION_VERSION;
    return TRUST_REVOCATION_OK;
}

enum trust_revocation_status trust_revocation_revoke(
    struct trust_revocation *revocation,
    struct verification_policy *policy,
    uint32_t anchor_id,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out)
{
    const struct verification_policy_snapshot *active;
    struct verification_policy_snapshot candidate;
    struct audit_record record;
    enum verification_policy_status policy_status;
    enum audit_status audit_status;
    uint32_t anchor_index;
    uint32_t index;
    uint64_t detail;

    if (revocation == NULL || policy == NULL || audit_ring == NULL) {
        return TRUST_REVOCATION_NULL;
    }
    if (!trust_revocation_valid(revocation)) {
        return TRUST_REVOCATION_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return TRUST_REVOCATION_NO_POLICY;
    }
    if (anchor_id < TRUST_ANCHOR_ID_MIN ||
        anchor_id > TRUST_ANCHOR_ID_MAX) {
        return TRUST_REVOCATION_UNKNOWN_ANCHOR;
    }

    anchor_index = 0U;
    for (index = 0U; index < active->anchor_count; ++index) {
        if (active->anchors[index].id == anchor_id) {
            anchor_index = index;
            break;
        }
    }
    if (index == active->anchor_count) {
        return TRUST_REVOCATION_UNKNOWN_ANCHOR;
    }
    if (active->anchors[anchor_index].state !=
        TRUST_ANCHOR_STATE_ACTIVE) {
        return TRUST_REVOCATION_NON_ACTIVE_ANCHOR;
    }
    if (active->generation == VERIFICATION_POLICY_GENERATION_MAX) {
        return TRUST_REVOCATION_GENERATION_OVERFLOW;
    }
    if (!event_ring_is_published(audit_ring)) {
        return TRUST_REVOCATION_BAD_STATE;
    }
    if (event_ring_count(audit_ring) >= KERNEL_EVENT_RING_CAPACITY) {
        return TRUST_REVOCATION_AUDIT_FAILED;
    }

    candidate = *active;
    ++candidate.generation;
    candidate.anchors[anchor_index].state =
        TRUST_ANCHOR_STATE_REVOKED;

    policy_status = verification_policy_stage(policy, &candidate);
    if (policy_status != VERIFICATION_POLICY_OK) {
        if (policy_status == VERIFICATION_POLICY_NULL) {
            return TRUST_REVOCATION_NULL;
        }
        return TRUST_REVOCATION_POLICY_STAGE_FAILED;
    }
    policy_status = verification_policy_commit(policy);
    if (policy_status != VERIFICATION_POLICY_OK) {
        if (policy_status == VERIFICATION_POLICY_NULL) {
            return TRUST_REVOCATION_NULL;
        }
        return TRUST_REVOCATION_POLICY_COMMIT_FAILED;
    }

    detail = (uint64_t)anchor_id |
             ((uint64_t)candidate.generation <<
              TRUST_REVOCATION_DETAIL_GENERATION_SHIFT);
    audit_status = audit_record_init(
        &record, AUDIT_RECORD_TRUST,
        TRUST_REVOCATION_AUDIT_OPERATION_REVOKE,
        TRUST_REVOCATION_AUDIT_RESULT_OK,
        anchor_id, 0U, 0U, detail);
    if (audit_status != AUDIT_OK) {
        return TRUST_REVOCATION_AUDIT_FAILED;
    }
    audit_status = audit_record_append(
        audit_ring, &record, audit_sequence_out);
    if (audit_status != AUDIT_OK) {
        if (audit_status == AUDIT_BAD_STATE) {
            return TRUST_REVOCATION_BAD_STATE;
        }
        return TRUST_REVOCATION_AUDIT_FAILED;
    }

    revocation->last_anchor_id = anchor_id;
    revocation->last_generation = candidate.generation;
    revocation->has_revoked = true;
    return TRUST_REVOCATION_OK;
}

bool trust_revocation_anchor_is_revoked(
    const struct trust_revocation *revocation,
    const struct verification_policy *policy,
    uint32_t anchor_id)
{
    const struct verification_policy_snapshot *active;
    uint32_t index;

    if (!trust_revocation_valid(revocation) || policy == NULL) {
        return false;
    }
    active = verification_policy_active(policy);
    if (active == NULL ||
        anchor_id < TRUST_ANCHOR_ID_MIN ||
        anchor_id > TRUST_ANCHOR_ID_MAX) {
        return false;
    }
    for (index = 0U; index < active->anchor_count; ++index) {
        if (active->anchors[index].id == anchor_id) {
            return active->anchors[index].state ==
                   TRUST_ANCHOR_STATE_REVOKED;
        }
    }
    return false;
}

const char *trust_revocation_status_string(
    enum trust_revocation_status status)
{
    switch (status) {
    case TRUST_REVOCATION_OK:
        return "TRUST_REVOCATION_OK";
    case TRUST_REVOCATION_NULL:
        return "TRUST_REVOCATION_NULL";
    case TRUST_REVOCATION_BAD_STATE:
        return "TRUST_REVOCATION_BAD_STATE";
    case TRUST_REVOCATION_NO_POLICY:
        return "TRUST_REVOCATION_NO_POLICY";
    case TRUST_REVOCATION_UNKNOWN_ANCHOR:
        return "TRUST_REVOCATION_UNKNOWN_ANCHOR";
    case TRUST_REVOCATION_NON_ACTIVE_ANCHOR:
        return "TRUST_REVOCATION_NON_ACTIVE_ANCHOR";
    case TRUST_REVOCATION_GENERATION_OVERFLOW:
        return "TRUST_REVOCATION_GENERATION_OVERFLOW";
    case TRUST_REVOCATION_POLICY_STAGE_FAILED:
        return "TRUST_REVOCATION_POLICY_STAGE_FAILED";
    case TRUST_REVOCATION_POLICY_COMMIT_FAILED:
        return "TRUST_REVOCATION_POLICY_COMMIT_FAILED";
    case TRUST_REVOCATION_AUDIT_FAILED:
        return "TRUST_REVOCATION_AUDIT_FAILED";
    default:
        return NULL;
    }
}
