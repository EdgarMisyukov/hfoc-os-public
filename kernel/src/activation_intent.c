#include "activation_intent.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void activation_intent_zero_bytes(char *destination,
                                         size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool activation_intent_digest_valid(const uint8_t *digest)
{
    size_t index;

    for (index = 0U; index < ACTIVATION_INTENT_DIGEST_SIZE; ++index) {
        if (digest[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool activation_intent_entry_valid(
    const struct activation_intent_entry *entry)
{
    if (entry == NULL ||
        entry->slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        entry->slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        entry->generation < CANDIDATE_SLOT_INVENTORY_GENERATION_MIN ||
        entry->generation > CANDIDATE_SLOT_INVENTORY_GENERATION_MAX ||
        !activation_intent_digest_valid(entry->digest)) {
        return false;
    }
    return true;
}

static bool activation_intent_record_valid(
    const struct activation_intent_record *record)
{
    if (record == NULL ||
        record->version != ACTIVATION_INTENT_VERSION ||
        !activation_intent_entry_valid(&record->target) ||
        !activation_intent_entry_valid(&record->rollback) ||
        record->bundle_size < ACTIVATION_INTENT_BUNDLE_MIN_SIZE ||
        record->bundle_size > ACTIVATION_INTENT_BUNDLE_MAX_SIZE) {
        return false;
    }
    return true;
}

static bool activation_intent_entries_equal(
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

enum activation_intent_status activation_intent_init(
    struct activation_intent_store *store,
    const struct activation_intent_entry *known_good)
{
    if (store == NULL || known_good == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (!activation_intent_entry_valid(known_good)) {
        return ACTIVATION_INTENT_BAD_ROLLBACK;
    }

    activation_intent_zero_bytes((char *)store, sizeof(*store));
    store->version = ACTIVATION_INTENT_VERSION;
    store->known_good = *known_good;
    store->initialized = true;
    return ACTIVATION_INTENT_OK;
}

enum activation_intent_status activation_intent_record(
    struct activation_intent_store *store,
    const struct candidate_staged_write_gate *candidate,
    const struct activation_intent_io *io)
{
    struct activation_intent_record staged;
    struct activation_intent_entry candidate_entry;

    if (store == NULL || candidate == NULL || io == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (!store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (activation_intent_record_valid(&store->pending) &&
        store->pending.pending) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (candidate->version != CANDIDATE_STAGED_WRITE_VERSION) {
        return ACTIVATION_INTENT_BAD_REQUEST;
    }
    if (!candidate->ready) {
        return ACTIVATION_INTENT_BAD_CANDIDATE;
    }

    candidate_entry.slot_id = candidate->slot_id;
    candidate_entry.generation = candidate->generation;
    activation_intent_zero_bytes(
        (char *)candidate_entry.digest,
        sizeof(candidate_entry.digest));
    {
        size_t index;

        for (index = 0U;
             index < ACTIVATION_INTENT_DIGEST_SIZE;
             ++index) {
            candidate_entry.digest[index] = candidate->digest[index];
        }
    }
    if (!activation_intent_entry_valid(&candidate_entry)) {
        return ACTIVATION_INTENT_BAD_CANDIDATE;
    }
    if (candidate->bundle_size < ACTIVATION_INTENT_BUNDLE_MIN_SIZE ||
        candidate->bundle_size > ACTIVATION_INTENT_BUNDLE_MAX_SIZE) {
        return ACTIVATION_INTENT_BAD_REQUEST;
    }
    if (!activation_intent_entry_valid(&store->known_good)) {
        return ACTIVATION_INTENT_BAD_ROLLBACK;
    }
    if (candidate->slot_id == store->known_good.slot_id) {
        return ACTIVATION_INTENT_SAME_SLOT;
    }

    activation_intent_zero_bytes((char *)&staged, sizeof(staged));
    staged.version = ACTIVATION_INTENT_VERSION;
    staged.target = candidate_entry;
    staged.rollback = store->known_good;
    staged.bundle_size = candidate->bundle_size;
    staged.pending = false;

    if (io->write != NULL &&
        !io->write(io->context, 0U, &staged)) {
        return ACTIVATION_INTENT_WRITE_FAULT;
    }
    staged.pending = true;
    if (io->write != NULL &&
        !io->write(io->context, 1U, &staged)) {
        staged.pending = false;
        store->pending = staged;
        return ACTIVATION_INTENT_WRITE_FAULT;
    }

    store->pending = staged;
    return ACTIVATION_INTENT_OK;
}

enum activation_intent_status activation_intent_pending(
    const struct activation_intent_store *store,
    struct activation_intent_record *record_out)
{
    if (store == NULL || record_out == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (!store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (!activation_intent_record_valid(&store->pending) ||
        !store->pending.pending) {
        return ACTIVATION_INTENT_NO_PENDING;
    }
    *record_out = store->pending;
    return ACTIVATION_INTENT_OK;
}

enum activation_intent_status activation_intent_known_good(
    const struct activation_intent_store *store,
    struct activation_intent_entry *entry_out)
{
    if (store == NULL || entry_out == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (!store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (!activation_intent_entry_valid(&store->known_good)) {
        return ACTIVATION_INTENT_BAD_ROLLBACK;
    }
    *entry_out = store->known_good;
    return ACTIVATION_INTENT_OK;
}

enum activation_intent_status activation_intent_commit_known_good(
    struct activation_intent_store *store)
{
    if (store == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (!store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (!activation_intent_record_valid(&store->pending) ||
        !store->pending.pending) {
        return ACTIVATION_INTENT_NO_PENDING;
    }
    if (!activation_intent_entry_valid(&store->known_good) ||
        !activation_intent_entries_equal(
            &store->known_good, &store->pending.rollback)) {
        return ACTIVATION_INTENT_BAD_ROLLBACK;
    }

    store->known_good = store->pending.target;
    activation_intent_zero_bytes(
        (char *)&store->pending, sizeof(store->pending));
    return ACTIVATION_INTENT_OK;
}

enum activation_intent_status activation_intent_resolve(
    struct activation_intent_store *store,
    struct activation_intent_entry *entry_out)
{
    if (store == NULL || entry_out == NULL) {
        return ACTIVATION_INTENT_NULL;
    }
    if (!store->initialized) {
        return ACTIVATION_INTENT_BAD_STATE;
    }
    if (!activation_intent_entry_valid(&store->known_good)) {
        return ACTIVATION_INTENT_BAD_ROLLBACK;
    }
    if (activation_intent_record_valid(&store->pending) &&
        store->pending.pending) {
        *entry_out = store->pending.target;
        return ACTIVATION_INTENT_OK;
    }

    activation_intent_zero_bytes(
        (char *)&store->pending, sizeof(store->pending));
    *entry_out = store->known_good;
    return ACTIVATION_INTENT_OK;
}

const char *activation_intent_status_string(
    enum activation_intent_status status)
{
    switch (status) {
    case ACTIVATION_INTENT_OK:
        return "ACTIVATION_INTENT_OK";
    case ACTIVATION_INTENT_NULL:
        return "ACTIVATION_INTENT_NULL";
    case ACTIVATION_INTENT_BAD_STATE:
        return "ACTIVATION_INTENT_BAD_STATE";
    case ACTIVATION_INTENT_BAD_REQUEST:
        return "ACTIVATION_INTENT_BAD_REQUEST";
    case ACTIVATION_INTENT_BAD_CANDIDATE:
        return "ACTIVATION_INTENT_BAD_CANDIDATE";
    case ACTIVATION_INTENT_BAD_ROLLBACK:
        return "ACTIVATION_INTENT_BAD_ROLLBACK";
    case ACTIVATION_INTENT_SAME_SLOT:
        return "ACTIVATION_INTENT_SAME_SLOT";
    case ACTIVATION_INTENT_WRITE_FAULT:
        return "ACTIVATION_INTENT_WRITE_FAULT";
    case ACTIVATION_INTENT_NO_PENDING:
        return "ACTIVATION_INTENT_NO_PENDING";
    default:
        return NULL;
    }
}
