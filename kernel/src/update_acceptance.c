#include "update_acceptance.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void update_acceptance_zero_bytes(char *destination,
                                         size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool update_acceptance_entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    size_t index;

    if (left->slot_id != right->slot_id ||
        left->generation != right->generation) {
        return false;
    }
    for (index = 0U;
         index < ACTIVATION_INTENT_DIGEST_SIZE;
         ++index) {
        if (left->digest[index] != right->digest[index]) {
            return false;
        }
    }
    return true;
}

enum update_acceptance_status update_acceptance_init(
    struct update_acceptance_store *store)
{
    if (store == NULL) {
        return UPDATE_ACCEPTANCE_NULL;
    }
    if (store->initialized) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }

    update_acceptance_zero_bytes((char *)store, sizeof(*store));
    store->version = UPDATE_ACCEPTANCE_VERSION;
    store->initialized = true;
    return UPDATE_ACCEPTANCE_OK;
}

enum update_acceptance_status update_acceptance_commit(
    struct update_acceptance_store *store,
    struct activation_intent_store *intent,
    const struct first_boot_health_store *health,
    const struct update_acceptance_io *io)
{
    struct activation_intent_record pending;
    struct activation_intent_entry known_good;
    struct first_boot_health_verdict verdict;
    struct update_acceptance_evidence evidence;
    enum activation_intent_status intent_status;
    enum first_boot_health_status health_status;

    if (store == NULL || intent == NULL || health == NULL ||
        io == NULL) {
        return UPDATE_ACCEPTANCE_NULL;
    }
    if (!store->initialized) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }
    if (store->accepted) {
        return UPDATE_ACCEPTANCE_ALREADY_ACCEPTED;
    }
    if (!intent->initialized) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }

    intent_status = activation_intent_pending(intent, &pending);
    if (intent_status != ACTIVATION_INTENT_OK) {
        return UPDATE_ACCEPTANCE_NO_PENDING;
    }

    health_status = first_boot_health_evaluate(health, &verdict);
    if (health_status == FIRST_BOOT_HEALTH_BAD_STATE) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }
    if (health_status == FIRST_BOOT_HEALTH_NOT_READY) {
        return UPDATE_ACCEPTANCE_HEALTH_NOT_READY;
    }
    if (health_status == FIRST_BOOT_HEALTH_FAILED) {
        return UPDATE_ACCEPTANCE_HEALTH_FAILED;
    }
    if (health_status != FIRST_BOOT_HEALTH_OK ||
        !verdict.ready ||
        verdict.failed ||
        verdict.missing_count != 0U) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }

    intent_status = activation_intent_known_good(intent, &known_good);
    if (intent_status != ACTIVATION_INTENT_OK ||
        !update_acceptance_entries_equal(
            &known_good, &pending.rollback)) {
        return UPDATE_ACCEPTANCE_BAD_ROLLBACK;
    }
    if (update_acceptance_entries_equal(
            &pending.target, &pending.rollback)) {
        return UPDATE_ACCEPTANCE_BAD_REQUEST;
    }

    update_acceptance_zero_bytes(
        (char *)&evidence, sizeof(evidence));
    evidence.version = UPDATE_ACCEPTANCE_VERSION;
    evidence.accepted_slot = pending.target;
    evidence.rollback_slot = pending.rollback;
    evidence.health_verdict = verdict;
    evidence.candidate_active = true;

    if (io->write != NULL &&
        (!io->write(io->context, 0U, &evidence) ||
         !io->write(io->context, 1U, &evidence))) {
        return UPDATE_ACCEPTANCE_WRITE_FAULT;
    }

    intent_status = activation_intent_commit_known_good(intent);
    if (intent_status != ACTIVATION_INTENT_OK) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }

    store->evidence = evidence;
    store->accepted = true;
    return UPDATE_ACCEPTANCE_OK;
}

enum update_acceptance_status update_acceptance_evidence(
    const struct update_acceptance_store *store,
    struct update_acceptance_evidence *evidence_out)
{
    if (store == NULL || evidence_out == NULL) {
        return UPDATE_ACCEPTANCE_NULL;
    }
    if (!store->initialized) {
        return UPDATE_ACCEPTANCE_BAD_STATE;
    }
    if (!store->accepted) {
        return UPDATE_ACCEPTANCE_NO_ACCEPTANCE;
    }

    *evidence_out = store->evidence;
    return UPDATE_ACCEPTANCE_OK;
}

const char *update_acceptance_status_string(
    enum update_acceptance_status status)
{
    switch (status) {
    case UPDATE_ACCEPTANCE_OK:
        return "UPDATE_ACCEPTANCE_OK";
    case UPDATE_ACCEPTANCE_NULL:
        return "UPDATE_ACCEPTANCE_NULL";
    case UPDATE_ACCEPTANCE_BAD_STATE:
        return "UPDATE_ACCEPTANCE_BAD_STATE";
    case UPDATE_ACCEPTANCE_BAD_REQUEST:
        return "UPDATE_ACCEPTANCE_BAD_REQUEST";
    case UPDATE_ACCEPTANCE_NO_PENDING:
        return "UPDATE_ACCEPTANCE_NO_PENDING";
    case UPDATE_ACCEPTANCE_HEALTH_NOT_READY:
        return "UPDATE_ACCEPTANCE_HEALTH_NOT_READY";
    case UPDATE_ACCEPTANCE_HEALTH_FAILED:
        return "UPDATE_ACCEPTANCE_HEALTH_FAILED";
    case UPDATE_ACCEPTANCE_BAD_ROLLBACK:
        return "UPDATE_ACCEPTANCE_BAD_ROLLBACK";
    case UPDATE_ACCEPTANCE_ALREADY_ACCEPTED:
        return "UPDATE_ACCEPTANCE_ALREADY_ACCEPTED";
    case UPDATE_ACCEPTANCE_NO_ACCEPTANCE:
        return "UPDATE_ACCEPTANCE_NO_ACCEPTANCE";
    case UPDATE_ACCEPTANCE_WRITE_FAULT:
        return "UPDATE_ACCEPTANCE_WRITE_FAULT";
    default:
        return NULL;
    }
}
