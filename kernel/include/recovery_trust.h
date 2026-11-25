#ifndef HFOC_RECOVERY_TRUST_H
#define HFOC_RECOVERY_TRUST_H

#include "boot_evidence.h"
#include "signed_metadata.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECOVERY_TRUST_VERSION 1U
#define RECOVERY_TRUST_MODE_NORMAL 0U
#define RECOVERY_TRUST_MODE_RECOVERY 1U
#define RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE 1U
#define RECOVERY_TRUST_ACTION_CONTROL 2U
#define RECOVERY_TRUST_ACTION_UPDATE 3U
#define RECOVERY_TRUST_ENTER_METADATA "recovery-enter-v1"
#define RECOVERY_TRUST_ENTER_METADATA_LEN 17U
#define RECOVERY_TRUST_EXIT_METADATA "recovery-exit-v1"
#define RECOVERY_TRUST_EXIT_METADATA_LEN 16U

enum recovery_trust_status {
    RECOVERY_TRUST_OK = 0,
    RECOVERY_TRUST_NULL = 1,
    RECOVERY_TRUST_BAD_STATE = 2,
    RECOVERY_TRUST_NO_POLICY = 3,
    RECOVERY_TRUST_NO_EVIDENCE = 4,
    RECOVERY_TRUST_EVIDENCE_MISMATCH = 5,
    RECOVERY_TRUST_INVALID_METADATA = 6,
    RECOVERY_TRUST_METADATA_DENIED = 7,
    RECOVERY_TRUST_POLICY_MISMATCH = 8,
    RECOVERY_TRUST_UNSUPPORTED_ACTION = 9,
    RECOVERY_TRUST_MODE_DENIED = 10,
};

struct recovery_trust_state {
    uint32_t version;
    uint32_t mode;
    uint32_t policy_generation;
    uint32_t signer_id;
    bool active;
};

enum recovery_trust_status recovery_trust_init(
    struct recovery_trust_state *state);
enum recovery_trust_status recovery_trust_enter(
    struct recovery_trust_state *state,
    const struct verification_policy *policy,
    const struct boot_evidence *evidence,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout);
enum recovery_trust_status recovery_trust_authorize(
    const struct recovery_trust_state *state,
    uint32_t action);
enum recovery_trust_status recovery_trust_exit(
    struct recovery_trust_state *state,
    const struct verification_policy *policy,
    const void *envelope,
    size_t envelope_length,
    struct signed_metadata_layout *layout);
bool recovery_trust_is_recovery(
    const struct recovery_trust_state *state);
const char *recovery_trust_status_string(
    enum recovery_trust_status status);

#endif
