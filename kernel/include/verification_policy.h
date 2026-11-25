#ifndef HFOC_VERIFICATION_POLICY_H
#define HFOC_VERIFICATION_POLICY_H

#include "trust_anchor.h"

#include <stdbool.h>
#include <stdint.h>

#define VERIFICATION_POLICY_VERSION 1U
#define VERIFICATION_POLICY_GENERATION_MIN 1U
#define VERIFICATION_POLICY_GENERATION_MAX 0xFFFFFFFFU
#define VERIFICATION_POLICY_ANCHOR_MIN 1U
#define VERIFICATION_POLICY_ANCHOR_MAX TRUST_ANCHOR_SET_MAX

enum verification_policy_status {
    VERIFICATION_POLICY_OK = 0,
    VERIFICATION_POLICY_NULL = 1,
    VERIFICATION_POLICY_BAD_STATE = 2,
    VERIFICATION_POLICY_NO_PENDING = 3,
    VERIFICATION_POLICY_INVALID_VERSION = 4,
    VERIFICATION_POLICY_INVALID_GENERATION = 5,
    VERIFICATION_POLICY_INVALID_ANCHOR_COUNT = 6,
    VERIFICATION_POLICY_INVALID_ANCHOR = 7,
    VERIFICATION_POLICY_DUPLICATE_ANCHOR = 8,
    VERIFICATION_POLICY_NON_ACTIVE_ANCHOR = 9,
};

struct verification_policy_snapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t anchor_count;
    struct trust_anchor anchors[TRUST_ANCHOR_SET_MAX];
};

struct verification_policy {
    struct verification_policy_snapshot active;
    struct verification_policy_snapshot pending;
    bool has_active;
    bool has_pending;
};

enum verification_policy_status verification_policy_snapshot_validate(
    const struct verification_policy_snapshot *snapshot);
enum verification_policy_status verification_policy_init(
    struct verification_policy *policy,
    const struct verification_policy_snapshot *initial);
enum verification_policy_status verification_policy_stage(
    struct verification_policy *policy,
    const struct verification_policy_snapshot *candidate);
enum verification_policy_status verification_policy_commit(
    struct verification_policy *policy);
enum verification_policy_status verification_policy_rollback(
    struct verification_policy *policy);
const struct verification_policy_snapshot *verification_policy_active(
    const struct verification_policy *policy);
bool verification_policy_has_pending(
    const struct verification_policy *policy);
const char *verification_policy_status_string(
    enum verification_policy_status status);

#endif
