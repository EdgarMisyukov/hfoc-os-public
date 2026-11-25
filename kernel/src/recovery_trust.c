#include "recovery_trust.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool recovery_trust_payload_exact(
    const struct signed_metadata_layout *layout,
    const void *envelope,
    const char *expected,
    size_t expected_length)
{
    const unsigned char *const bytes =
        (const unsigned char *)envelope;

    return layout->metadata_length == expected_length &&
           memcmp(bytes + layout->metadata_offset,
                  expected, expected_length) == 0;
}

static enum recovery_trust_status
recovery_trust_map_evidence_status(enum boot_evidence_status status)
{
    switch (status) {
    case BOOT_EVIDENCE_OK:
        return RECOVERY_TRUST_OK;
    case BOOT_EVIDENCE_NULL:
        return RECOVERY_TRUST_NULL;
    case BOOT_EVIDENCE_NO_POLICY:
        return RECOVERY_TRUST_NO_POLICY;
    case BOOT_EVIDENCE_DIGEST_MISMATCH:
    case BOOT_EVIDENCE_POLICY_MISMATCH:
        return RECOVERY_TRUST_EVIDENCE_MISMATCH;
    default:
        return RECOVERY_TRUST_NO_EVIDENCE;
    }
}

static enum recovery_trust_status
recovery_trust_map_metadata_status(enum signed_metadata_status status)
{
    switch (status) {
    case SIGNED_METADATA_OK:
        return RECOVERY_TRUST_OK;
    case SIGNED_METADATA_NULL:
        return RECOVERY_TRUST_NULL;
    case SIGNED_METADATA_NO_POLICY:
        return RECOVERY_TRUST_NO_POLICY;
    case SIGNED_METADATA_PURPOSE_DENIED:
        return RECOVERY_TRUST_METADATA_DENIED;
    default:
        return RECOVERY_TRUST_INVALID_METADATA;
    }
}

enum recovery_trust_status recovery_trust_init(
    struct recovery_trust_state *state)
{
    if (state == NULL) {
        return RECOVERY_TRUST_NULL;
    }
    if (state->active ||
        state->version == RECOVERY_TRUST_VERSION) {
        return RECOVERY_TRUST_BAD_STATE;
    }
    memset(state, 0, sizeof(*state));
    state->version = RECOVERY_TRUST_VERSION;
    state->mode = RECOVERY_TRUST_MODE_NORMAL;
    return RECOVERY_TRUST_OK;
}

enum recovery_trust_status recovery_trust_enter(
    struct recovery_trust_state *state,
    const struct verification_policy *policy,
    const struct boot_evidence *evidence,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout)
{
    const struct verification_policy_snapshot *active;
    struct signed_metadata_layout verified;
    struct recovery_trust_state draft;
    enum boot_evidence_status evidence_status;
    enum signed_metadata_status metadata_status;
    enum recovery_trust_status recovery_status;

    if (state == NULL || policy == NULL || evidence == NULL ||
        envelope == NULL || layout == NULL) {
        return RECOVERY_TRUST_NULL;
    }
    if (state->version != RECOVERY_TRUST_VERSION ||
        state->mode != RECOVERY_TRUST_MODE_NORMAL ||
        state->active) {
        return RECOVERY_TRUST_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return RECOVERY_TRUST_NO_POLICY;
    }

    evidence_status = boot_evidence_check_policy(evidence, policy);
    recovery_status = recovery_trust_map_evidence_status(
        evidence_status);
    if (recovery_status != RECOVERY_TRUST_OK) {
        return recovery_status;
    }

    metadata_status = signed_metadata_verify(
        policy, KEY_USE_PURPOSE_RECOVERY,
        envelope, envelope_length, &verified);
    recovery_status = recovery_trust_map_metadata_status(
        metadata_status);
    if (recovery_status != RECOVERY_TRUST_OK) {
        return recovery_status;
    }
    if (!recovery_trust_payload_exact(
            &verified, envelope, RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN)) {
        return RECOVERY_TRUST_INVALID_METADATA;
    }

    memset(&draft, 0, sizeof(draft));
    draft.version = RECOVERY_TRUST_VERSION;
    draft.mode = RECOVERY_TRUST_MODE_RECOVERY;
    draft.policy_generation = active->generation;
    draft.signer_id = verified.signer_id;
    draft.active = true;
    *state = draft;
    *layout = verified;
    return RECOVERY_TRUST_OK;
}

enum recovery_trust_status recovery_trust_authorize(
    const struct recovery_trust_state *state,
    uint32_t action)
{
    if (state == NULL) {
        return RECOVERY_TRUST_NULL;
    }
    if (state->version != RECOVERY_TRUST_VERSION ||
        (state->mode != RECOVERY_TRUST_MODE_NORMAL &&
         state->mode != RECOVERY_TRUST_MODE_RECOVERY) ||
        (state->mode == RECOVERY_TRUST_MODE_NORMAL && state->active) ||
        (state->mode == RECOVERY_TRUST_MODE_RECOVERY && !state->active)) {
        return RECOVERY_TRUST_BAD_STATE;
    }
    if (action != RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE &&
        action != RECOVERY_TRUST_ACTION_CONTROL &&
        action != RECOVERY_TRUST_ACTION_UPDATE) {
        return RECOVERY_TRUST_UNSUPPORTED_ACTION;
    }
    if (state->mode == RECOVERY_TRUST_MODE_RECOVERY &&
        action != RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE) {
        return RECOVERY_TRUST_MODE_DENIED;
    }
    return RECOVERY_TRUST_OK;
}

enum recovery_trust_status recovery_trust_exit(
    struct recovery_trust_state *state,
    const struct verification_policy *policy,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout)
{
    const struct verification_policy_snapshot *active;
    struct signed_metadata_layout verified;
    enum signed_metadata_status metadata_status;
    enum recovery_trust_status recovery_status;

    if (state == NULL || policy == NULL || envelope == NULL ||
        layout == NULL) {
        return RECOVERY_TRUST_NULL;
    }
    if (state->version != RECOVERY_TRUST_VERSION ||
        state->mode != RECOVERY_TRUST_MODE_RECOVERY ||
        !state->active) {
        return RECOVERY_TRUST_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return RECOVERY_TRUST_NO_POLICY;
    }
    if (active->generation != state->policy_generation) {
        return RECOVERY_TRUST_POLICY_MISMATCH;
    }

    metadata_status = signed_metadata_verify(
        policy, KEY_USE_PURPOSE_RECOVERY,
        envelope, envelope_length, &verified);
    recovery_status = recovery_trust_map_metadata_status(
        metadata_status);
    if (recovery_status != RECOVERY_TRUST_OK) {
        return recovery_status;
    }
    if (!recovery_trust_payload_exact(
            &verified, envelope, RECOVERY_TRUST_EXIT_METADATA,
            RECOVERY_TRUST_EXIT_METADATA_LEN)) {
        return RECOVERY_TRUST_INVALID_METADATA;
    }

    memset(state, 0, sizeof(*state));
    state->version = RECOVERY_TRUST_VERSION;
    state->mode = RECOVERY_TRUST_MODE_NORMAL;
    *layout = verified;
    return RECOVERY_TRUST_OK;
}

bool recovery_trust_is_recovery(
    const struct recovery_trust_state *state)
{
    return state != NULL &&
           state->version == RECOVERY_TRUST_VERSION &&
           state->mode == RECOVERY_TRUST_MODE_RECOVERY &&
           state->active;
}

const char *recovery_trust_status_string(
    enum recovery_trust_status status)
{
    switch (status) {
    case RECOVERY_TRUST_OK:
        return "RECOVERY_TRUST_OK";
    case RECOVERY_TRUST_NULL:
        return "RECOVERY_TRUST_NULL";
    case RECOVERY_TRUST_BAD_STATE:
        return "RECOVERY_TRUST_BAD_STATE";
    case RECOVERY_TRUST_NO_POLICY:
        return "RECOVERY_TRUST_NO_POLICY";
    case RECOVERY_TRUST_NO_EVIDENCE:
        return "RECOVERY_TRUST_NO_EVIDENCE";
    case RECOVERY_TRUST_EVIDENCE_MISMATCH:
        return "RECOVERY_TRUST_EVIDENCE_MISMATCH";
    case RECOVERY_TRUST_INVALID_METADATA:
        return "RECOVERY_TRUST_INVALID_METADATA";
    case RECOVERY_TRUST_METADATA_DENIED:
        return "RECOVERY_TRUST_METADATA_DENIED";
    case RECOVERY_TRUST_POLICY_MISMATCH:
        return "RECOVERY_TRUST_POLICY_MISMATCH";
    case RECOVERY_TRUST_UNSUPPORTED_ACTION:
        return "RECOVERY_TRUST_UNSUPPORTED_ACTION";
    case RECOVERY_TRUST_MODE_DENIED:
        return "RECOVERY_TRUST_MODE_DENIED";
    default:
        return NULL;
    }
}
