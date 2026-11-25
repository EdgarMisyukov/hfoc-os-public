#include "automatic_rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void automatic_rollback_zero_bytes(char *destination,
                                          size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool automatic_rollback_entries_equal(
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

enum automatic_rollback_status automatic_rollback_init(
    struct automatic_rollback_store *store)
{
    if (store == NULL) {
        return AUTOMATIC_ROLLBACK_NULL;
    }
    if (store->initialized) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }

    automatic_rollback_zero_bytes(
        (char *)store, sizeof(*store));
    store->version = AUTOMATIC_ROLLBACK_VERSION;
    store->initialized = true;
    return AUTOMATIC_ROLLBACK_OK;
}

enum automatic_rollback_status automatic_rollback_run(
    struct automatic_rollback_store *store,
    const struct activation_intent_store *intent,
    const struct first_boot_health_store *health)
{
    struct activation_intent_record pending;
    struct activation_intent_entry known_good;
    struct first_boot_health_verdict verdict;
    struct automatic_rollback_evidence evidence;
    enum activation_intent_status intent_status;
    enum first_boot_health_status health_status;

    if (store == NULL || intent == NULL || health == NULL) {
        return AUTOMATIC_ROLLBACK_NULL;
    }
    if (!store->initialized) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }
    if (store->published) {
        return AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED;
    }
    if (!intent->initialized) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }

    intent_status = activation_intent_pending(intent, &pending);
    if (intent_status != ACTIVATION_INTENT_OK) {
        return AUTOMATIC_ROLLBACK_NO_PENDING;
    }

    health_status = first_boot_health_evaluate(health, &verdict);
    if (health_status == FIRST_BOOT_HEALTH_BAD_STATE) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }
    if (health_status != FIRST_BOOT_HEALTH_FAILED) {
        return AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED;
    }

    intent_status = activation_intent_known_good(intent, &known_good);
    if (intent_status != ACTIVATION_INTENT_OK ||
        !automatic_rollback_entries_equal(
            &known_good, &pending.rollback)) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }

    automatic_rollback_zero_bytes(
        (char *)&evidence, sizeof(evidence));
    evidence.version = AUTOMATIC_ROLLBACK_VERSION;
    evidence.reason =
        AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED;
    evidence.selected_slot = pending.rollback;
    evidence.candidate_slot = pending.target;
    evidence.health_verdict = verdict;
    evidence.candidate_active = false;

    store->evidence = evidence;
    store->published = true;
    return AUTOMATIC_ROLLBACK_OK;
}

enum automatic_rollback_status automatic_rollback_evidence(
    const struct automatic_rollback_store *store,
    struct automatic_rollback_evidence *evidence_out)
{
    if (store == NULL || evidence_out == NULL) {
        return AUTOMATIC_ROLLBACK_NULL;
    }
    if (!store->initialized) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }
    if (!store->published) {
        return AUTOMATIC_ROLLBACK_NO_EVIDENCE;
    }

    *evidence_out = store->evidence;
    return AUTOMATIC_ROLLBACK_OK;
}

enum automatic_rollback_status automatic_rollback_selected(
    const struct automatic_rollback_store *store,
    struct activation_intent_entry *entry_out)
{
    if (store == NULL || entry_out == NULL) {
        return AUTOMATIC_ROLLBACK_NULL;
    }
    if (!store->initialized) {
        return AUTOMATIC_ROLLBACK_BAD_STATE;
    }
    if (!store->published) {
        return AUTOMATIC_ROLLBACK_NO_EVIDENCE;
    }

    *entry_out = store->evidence.selected_slot;
    return AUTOMATIC_ROLLBACK_OK;
}

const char *automatic_rollback_status_string(
    enum automatic_rollback_status status)
{
    switch (status) {
    case AUTOMATIC_ROLLBACK_OK:
        return "AUTOMATIC_ROLLBACK_OK";
    case AUTOMATIC_ROLLBACK_NULL:
        return "AUTOMATIC_ROLLBACK_NULL";
    case AUTOMATIC_ROLLBACK_BAD_STATE:
        return "AUTOMATIC_ROLLBACK_BAD_STATE";
    case AUTOMATIC_ROLLBACK_BAD_REQUEST:
        return "AUTOMATIC_ROLLBACK_BAD_REQUEST";
    case AUTOMATIC_ROLLBACK_NO_PENDING:
        return "AUTOMATIC_ROLLBACK_NO_PENDING";
    case AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED:
        return "AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED";
    case AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED:
        return "AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED";
    case AUTOMATIC_ROLLBACK_NO_EVIDENCE:
        return "AUTOMATIC_ROLLBACK_NO_EVIDENCE";
    default:
        return NULL;
    }
}

const char *automatic_rollback_reason_string(
    enum automatic_rollback_reason reason)
{
    switch (reason) {
    case AUTOMATIC_ROLLBACK_REASON_NONE:
        return "AUTOMATIC_ROLLBACK_REASON_NONE";
    case AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED:
        return "AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED";
    default:
        return NULL;
    }
}
