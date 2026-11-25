#include "update_retention.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void update_retention_zero_bytes(char *destination,
                                        size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool update_retention_digest_valid(const uint8_t *digest)
{
    size_t index;

    for (index = 0U;
         index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE;
         ++index) {
        if (digest[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool update_retention_candidate_entry_valid(
    const struct candidate_slot_inventory_entry *entry)
{
    return entry != NULL &&
           entry->slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           entry->slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX &&
           entry->generation >= CANDIDATE_SLOT_INVENTORY_GENERATION_MIN &&
           entry->generation <= CANDIDATE_SLOT_INVENTORY_GENERATION_MAX &&
           update_retention_digest_valid(entry->digest);
}

static bool update_retention_activation_entry_valid(
    const struct activation_intent_entry *entry)
{
    return entry != NULL &&
           entry->slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           entry->slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX &&
           entry->generation >= CANDIDATE_SLOT_INVENTORY_GENERATION_MIN &&
           entry->generation <= CANDIDATE_SLOT_INVENTORY_GENERATION_MAX &&
           update_retention_digest_valid(entry->digest);
}

static bool update_retention_candidate_entries_equal(
    const struct candidate_slot_inventory_entry *left,
    const struct candidate_slot_inventory_entry *right)
{
    size_t index;

    if (left->slot_id != right->slot_id ||
        left->generation != right->generation) {
        return false;
    }
    for (index = 0U;
         index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE;
         ++index) {
        if (left->digest[index] != right->digest[index]) {
            return false;
        }
    }
    return true;
}

static bool update_retention_inventory_valid(
    const struct candidate_slot_inventory *inventory)
{
    size_t index;
    size_t previous;

    if (inventory == NULL ||
        inventory->version != CANDIDATE_SLOT_INVENTORY_VERSION ||
        !inventory->active ||
        inventory->slot_count == 0U ||
        inventory->slot_count > CANDIDATE_SLOT_INVENTORY_MAX_SLOTS) {
        return false;
    }
    for (index = 0U; index < inventory->slot_count; ++index) {
        if (!update_retention_candidate_entry_valid(
                &inventory->slots[index])) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (inventory->slots[previous].slot_id ==
                inventory->slots[index].slot_id) {
                return false;
            }
        }
    }
    return true;
}

static bool update_retention_activation_entries_equal(
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

static bool update_retention_pending_record_valid(
    const struct activation_intent_record *record)
{
    return record != NULL &&
           record->version == ACTIVATION_INTENT_VERSION &&
           record->pending &&
           record->bundle_size >= ACTIVATION_INTENT_BUNDLE_MIN_SIZE &&
           record->bundle_size <= ACTIVATION_INTENT_BUNDLE_MAX_SIZE &&
           update_retention_activation_entry_valid(&record->target) &&
           update_retention_activation_entry_valid(&record->rollback) &&
           !update_retention_activation_entries_equal(
               &record->target, &record->rollback);
}

static bool update_retention_intent_valid(
    const struct activation_intent_store *intent,
    struct activation_intent_record *pending_out,
    bool *has_pending)
{
    if (intent == NULL ||
        !intent->initialized ||
        !update_retention_activation_entry_valid(&intent->known_good)) {
        return false;
    }
    *has_pending = intent->pending.pending;
    if (*has_pending) {
        if (!update_retention_pending_record_valid(&intent->pending) ||
            !update_retention_activation_entries_equal(
                &intent->pending.rollback, &intent->known_good)) {
            return false;
        }
        *pending_out = intent->pending;
    } else {
        update_retention_zero_bytes(
            (char *)pending_out, sizeof(*pending_out));
    }
    return true;
}

static void update_retention_copy_entry(
    struct candidate_slot_inventory_entry *destination,
    const struct candidate_slot_inventory_entry *source)
{
    size_t index;

    destination->slot_id = source->slot_id;
    destination->generation = source->generation;
    for (index = 0U;
         index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE;
         ++index) {
        destination->digest[index] = source->digest[index];
    }
}

static void update_retention_copy_activation_entry(
    struct candidate_slot_inventory_entry *destination,
    const struct activation_intent_entry *source)
{
    size_t index;

    destination->slot_id = source->slot_id;
    destination->generation = source->generation;
    for (index = 0U;
         index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE;
         ++index) {
        destination->digest[index] = source->digest[index];
    }
}

static bool update_retention_entry_protected(
    const struct candidate_slot_inventory_entry *entry,
    const struct update_retention_evidence *draft)
{
    return update_retention_candidate_entries_equal(
               entry, &draft->protected_active) ||
           update_retention_candidate_entries_equal(
               entry, &draft->protected_rollback) ||
           update_retention_candidate_entries_equal(
               entry, &draft->protected_pending);
}

static void update_retention_order_inventory(
    const struct candidate_slot_inventory *inventory,
    const struct candidate_slot_inventory_entry **ordered)
{
    size_t index;

    for (index = 0U; index < inventory->slot_count; ++index) {
        const struct candidate_slot_inventory_entry *entry =
            &inventory->slots[index];
        size_t position = index;

        while (position > 0U &&
               ordered[position - 1U]->slot_id > entry->slot_id) {
            ordered[position] = ordered[position - 1U];
            --position;
        }
        ordered[position] = entry;
    }
}

enum update_retention_status update_retention_init(
    struct update_retention_store *store)
{
    if (store == NULL) {
        return UPDATE_RETENTION_NULL;
    }
    if (store->initialized) {
        return UPDATE_RETENTION_BAD_STATE;
    }

    update_retention_zero_bytes((char *)store, sizeof(*store));
    store->version = UPDATE_RETENTION_VERSION;
    store->initialized = true;
    return UPDATE_RETENTION_OK;
}

enum update_retention_status update_retention_run(
    struct update_retention_store *store,
    const struct candidate_slot_inventory *inventory,
    const struct activation_intent_store *intent,
    const struct update_retention_policy *policy)
{
    struct update_retention_evidence draft;
    struct activation_intent_record pending;
    const struct candidate_slot_inventory_entry
        *ordered[CANDIDATE_SLOT_INVENTORY_MAX_SLOTS];
    bool has_pending;
    size_t index;
    size_t retained_count = 0U;
    size_t reclaim_count = 0U;

    if (store == NULL || inventory == NULL || intent == NULL ||
        policy == NULL) {
        return UPDATE_RETENTION_NULL;
    }
    if (!store->initialized) {
        return UPDATE_RETENTION_BAD_STATE;
    }
    if (store->published) {
        return UPDATE_RETENTION_ALREADY_PUBLISHED;
    }
    if (policy->version != UPDATE_RETENTION_VERSION ||
        policy->max_unreferenced_kept > UPDATE_RETENTION_POLICY_KEEP_MAX) {
        return UPDATE_RETENTION_BAD_POLICY;
    }
    if (!inventory->active) {
        return UPDATE_RETENTION_INACTIVE_INVENTORY;
    }
    if (!update_retention_inventory_valid(inventory)) {
        return UPDATE_RETENTION_INVALID_INVENTORY;
    }
    if (!update_retention_intent_valid(
            intent, &pending, &has_pending)) {
        return UPDATE_RETENTION_BAD_INTENT;
    }

    update_retention_zero_bytes((char *)&draft, sizeof(draft));
    draft.version = UPDATE_RETENTION_VERSION;
    draft.policy_max_unreferenced_kept =
        policy->max_unreferenced_kept;
    update_retention_copy_activation_entry(
        &draft.protected_active, &intent->known_good);
    if (has_pending) {
        update_retention_copy_activation_entry(
            &draft.protected_rollback, &pending.rollback);
        update_retention_copy_activation_entry(
            &draft.protected_pending, &pending.target);
    } else {
        update_retention_copy_activation_entry(
            &draft.protected_rollback, &intent->known_good);
    }

    update_retention_order_inventory(inventory, ordered);
    for (index = 0U; index < inventory->slot_count; ++index) {
        if (update_retention_entry_protected(ordered[index], &draft)) {
            continue;
        }
        if (retained_count <
            (size_t)policy->max_unreferenced_kept) {
            update_retention_copy_entry(
                &draft.retained[retained_count], ordered[index]);
            ++retained_count;
        } else {
            update_retention_copy_entry(
                &draft.reclaim[reclaim_count], ordered[index]);
            ++reclaim_count;
        }
    }
    draft.retained_count = retained_count;
    draft.reclaim_count = reclaim_count;

    store->evidence = draft;
    store->published = true;
    return UPDATE_RETENTION_OK;
}

enum update_retention_status update_retention_evidence(
    const struct update_retention_store *store,
    struct update_retention_evidence *evidence_out)
{
    if (store == NULL || evidence_out == NULL) {
        return UPDATE_RETENTION_NULL;
    }
    if (!store->initialized) {
        return UPDATE_RETENTION_BAD_STATE;
    }
    if (!store->published) {
        return UPDATE_RETENTION_NO_EVIDENCE;
    }

    *evidence_out = store->evidence;
    return UPDATE_RETENTION_OK;
}

const char *update_retention_status_string(
    enum update_retention_status status)
{
    switch (status) {
    case UPDATE_RETENTION_OK:
        return "UPDATE_RETENTION_OK";
    case UPDATE_RETENTION_NULL:
        return "UPDATE_RETENTION_NULL";
    case UPDATE_RETENTION_BAD_STATE:
        return "UPDATE_RETENTION_BAD_STATE";
    case UPDATE_RETENTION_BAD_POLICY:
        return "UPDATE_RETENTION_BAD_POLICY";
    case UPDATE_RETENTION_INACTIVE_INVENTORY:
        return "UPDATE_RETENTION_INACTIVE_INVENTORY";
    case UPDATE_RETENTION_INVALID_INVENTORY:
        return "UPDATE_RETENTION_INVALID_INVENTORY";
    case UPDATE_RETENTION_BAD_INTENT:
        return "UPDATE_RETENTION_BAD_INTENT";
    case UPDATE_RETENTION_ALREADY_PUBLISHED:
        return "UPDATE_RETENTION_ALREADY_PUBLISHED";
    case UPDATE_RETENTION_NO_EVIDENCE:
        return "UPDATE_RETENTION_NO_EVIDENCE";
    default:
        return NULL;
    }
}
