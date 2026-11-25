#ifndef HFOC_UPDATE_ACCEPTANCE_H
#define HFOC_UPDATE_ACCEPTANCE_H

#include "activation_intent.h"
#include "first_boot_health.h"

#include <stdbool.h>
#include <stdint.h>

#define UPDATE_ACCEPTANCE_VERSION 1U

enum update_acceptance_status {
    UPDATE_ACCEPTANCE_OK = 0,
    UPDATE_ACCEPTANCE_NULL = 1,
    UPDATE_ACCEPTANCE_BAD_STATE = 2,
    UPDATE_ACCEPTANCE_BAD_REQUEST = 3,
    UPDATE_ACCEPTANCE_NO_PENDING = 4,
    UPDATE_ACCEPTANCE_HEALTH_NOT_READY = 5,
    UPDATE_ACCEPTANCE_HEALTH_FAILED = 6,
    UPDATE_ACCEPTANCE_BAD_ROLLBACK = 7,
    UPDATE_ACCEPTANCE_ALREADY_ACCEPTED = 8,
    UPDATE_ACCEPTANCE_NO_ACCEPTANCE = 9,
    UPDATE_ACCEPTANCE_WRITE_FAULT = 10,
};

struct update_acceptance_evidence {
    uint32_t version;
    struct activation_intent_entry accepted_slot;
    struct activation_intent_entry rollback_slot;
    struct first_boot_health_verdict health_verdict;
    bool candidate_active;
};

typedef bool (*update_acceptance_write_fn)(
    void *context,
    uint8_t phase,
    const struct update_acceptance_evidence *evidence);

struct update_acceptance_io {
    void *context;
    update_acceptance_write_fn write;
};

struct update_acceptance_store {
    uint32_t version;
    struct update_acceptance_evidence evidence;
    bool initialized;
    bool accepted;
};

enum update_acceptance_status update_acceptance_init(
    struct update_acceptance_store *store);
enum update_acceptance_status update_acceptance_commit(
    struct update_acceptance_store *store,
    struct activation_intent_store *intent,
    const struct first_boot_health_store *health,
    const struct update_acceptance_io *io);
enum update_acceptance_status update_acceptance_evidence(
    const struct update_acceptance_store *store,
    struct update_acceptance_evidence *evidence_out);
const char *update_acceptance_status_string(
    enum update_acceptance_status status);

#endif
