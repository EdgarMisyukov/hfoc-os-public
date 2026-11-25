#ifndef HFOC_AUTOMATIC_ROLLBACK_H
#define HFOC_AUTOMATIC_ROLLBACK_H

#include "activation_intent.h"
#include "first_boot_health.h"

#include <stdbool.h>
#include <stdint.h>

#define AUTOMATIC_ROLLBACK_VERSION 1U

enum automatic_rollback_reason {
    AUTOMATIC_ROLLBACK_REASON_NONE = 0,
    AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED = 1,
};

enum automatic_rollback_status {
    AUTOMATIC_ROLLBACK_OK = 0,
    AUTOMATIC_ROLLBACK_NULL = 1,
    AUTOMATIC_ROLLBACK_BAD_STATE = 2,
    AUTOMATIC_ROLLBACK_BAD_REQUEST = 3,
    AUTOMATIC_ROLLBACK_NO_PENDING = 4,
    AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED = 5,
    AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED = 6,
    AUTOMATIC_ROLLBACK_NO_EVIDENCE = 7,
};

struct automatic_rollback_evidence {
    uint32_t version;
    enum automatic_rollback_reason reason;
    struct activation_intent_entry selected_slot;
    struct activation_intent_entry candidate_slot;
    struct first_boot_health_verdict health_verdict;
    bool candidate_active;
};

struct automatic_rollback_store {
    uint32_t version;
    struct automatic_rollback_evidence evidence;
    bool initialized;
    bool published;
};

enum automatic_rollback_status automatic_rollback_init(
    struct automatic_rollback_store *store);
enum automatic_rollback_status automatic_rollback_run(
    struct automatic_rollback_store *store,
    const struct activation_intent_store *intent,
    const struct first_boot_health_store *health);
enum automatic_rollback_status automatic_rollback_evidence(
    const struct automatic_rollback_store *store,
    struct automatic_rollback_evidence *evidence_out);
enum automatic_rollback_status automatic_rollback_selected(
    const struct automatic_rollback_store *store,
    struct activation_intent_entry *entry_out);
const char *automatic_rollback_status_string(
    enum automatic_rollback_status status);
const char *automatic_rollback_reason_string(
    enum automatic_rollback_reason reason);

#endif
