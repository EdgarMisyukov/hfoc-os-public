#ifndef HFOC_TRUST_REVOCATION_H
#define HFOC_TRUST_REVOCATION_H

#include "event_ring.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stdint.h>

#define TRUST_REVOCATION_VERSION 1U
#define TRUST_REVOCATION_AUDIT_OPERATION_REVOKE 1U
#define TRUST_REVOCATION_AUDIT_RESULT_OK 0U
#define TRUST_REVOCATION_DETAIL_GENERATION_SHIFT 8U

enum trust_revocation_status {
    TRUST_REVOCATION_OK = 0,
    TRUST_REVOCATION_NULL = 1,
    TRUST_REVOCATION_BAD_STATE = 2,
    TRUST_REVOCATION_NO_POLICY = 3,
    TRUST_REVOCATION_UNKNOWN_ANCHOR = 4,
    TRUST_REVOCATION_NON_ACTIVE_ANCHOR = 5,
    TRUST_REVOCATION_GENERATION_OVERFLOW = 6,
    TRUST_REVOCATION_POLICY_STAGE_FAILED = 7,
    TRUST_REVOCATION_POLICY_COMMIT_FAILED = 8,
    TRUST_REVOCATION_AUDIT_FAILED = 9,
};

struct trust_revocation {
    uint32_t version;
    uint32_t last_anchor_id;
    uint32_t last_generation;
    bool has_revoked;
};

enum trust_revocation_status trust_revocation_init(
    struct trust_revocation *revocation);
enum trust_revocation_status trust_revocation_revoke(
    struct trust_revocation *revocation,
    struct verification_policy *policy,
    uint32_t anchor_id,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out);
bool trust_revocation_anchor_is_revoked(
    const struct trust_revocation *revocation,
    const struct verification_policy *policy,
    uint32_t anchor_id);
const char *trust_revocation_status_string(
    enum trust_revocation_status status);

#endif
