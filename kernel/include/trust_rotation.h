#ifndef HFOC_TRUST_ROTATION_H
#define HFOC_TRUST_ROTATION_H

#include "verification_policy.h"

#include <stdbool.h>
#include <stdint.h>

#define TRUST_ROTATION_VERSION 1U

enum trust_rotation_status {
    TRUST_ROTATION_OK = 0,
    TRUST_ROTATION_NULL = 1,
    TRUST_ROTATION_BAD_STATE = 2,
    TRUST_ROTATION_NO_STAGED = 3,
    TRUST_ROTATION_NO_PREPARED = 4,
    TRUST_ROTATION_NO_POLICY = 5,
    TRUST_ROTATION_INVALID_ANCHOR = 6,
    TRUST_ROTATION_NON_INACTIVE_ANCHOR = 7,
    TRUST_ROTATION_DUPLICATE_ANCHOR = 8,
    TRUST_ROTATION_POLICY_FULL = 9,
    TRUST_ROTATION_GENERATION_OVERFLOW = 10,
    TRUST_ROTATION_STAGE_FAILED = 11,
    TRUST_ROTATION_COMMIT_FAILED = 12,
};

struct trust_rotation {
    uint32_t version;
    struct trust_anchor staged;
    struct verification_policy_snapshot prepared;
    bool has_staged;
    bool has_prepared;
};

enum trust_rotation_status trust_rotation_init(
    struct trust_rotation *rotation);
enum trust_rotation_status trust_rotation_stage(
    struct trust_rotation *rotation,
    const struct verification_policy *policy,
    const struct trust_anchor *anchor);
enum trust_rotation_status trust_rotation_prepare(
    struct trust_rotation *rotation,
    struct verification_policy *policy);
enum trust_rotation_status trust_rotation_activate(
    struct trust_rotation *rotation,
    struct verification_policy *policy);
enum trust_rotation_status trust_rotation_rollback(
    struct trust_rotation *rotation,
    struct verification_policy *policy);
bool trust_rotation_has_staged(
    const struct trust_rotation *rotation);
bool trust_rotation_has_prepared(
    const struct trust_rotation *rotation);
const struct trust_anchor *trust_rotation_staged(
    const struct trust_rotation *rotation);
const struct verification_policy_snapshot *trust_rotation_prepared(
    const struct trust_rotation *rotation);
const char *trust_rotation_status_string(
    enum trust_rotation_status status);

#endif
