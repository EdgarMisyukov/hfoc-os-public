#include "storage_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void storage_health_zero_bytes(unsigned char *bytes,
                                      size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static bool storage_health_bytes_equal(const unsigned char *left,
                                       const unsigned char *right,
                                       size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool storage_health_digest_valid(const uint8_t *digest)
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

static bool storage_health_entry_valid(
    const struct config_kv_entry *entry)
{
    return entry != NULL &&
           entry->occupied &&
           entry->kind < CONFIG_KV_KIND_COUNT &&
           entry->key_length > 0U &&
           entry->key_length <= CONFIG_KV_KEY_MAX &&
           entry->value_length <= CONFIG_KV_VALUE_MAX &&
           entry->revision != 0U;
}

static bool storage_health_entries_match(
    const struct config_kv_entry *left,
    const struct config_kv_entry *right)
{
    return left->kind == right->kind &&
           left->key_length == right->key_length &&
           storage_health_bytes_equal(left->key, right->key,
                                      left->key_length);
}

static bool storage_health_journal_valid(
    const struct journal_compaction_generation *journal)
{
    size_t index;
    size_t previous;

    if (journal == NULL ||
        !journal->valid ||
        !journal->active ||
        !journal->staged ||
        journal->interrupted ||
        journal->generation_id == 0U ||
        journal->source_sequence == 0U ||
        journal->count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        journal->config_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        journal->audit_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        (uint64_t)journal->config_count + journal->audit_count !=
            journal->count) {
        return false;
    }

    for (index = 0U; index < journal->count; ++index) {
        const struct config_kv_entry *entry =
            &journal->entries[index];

        if (!storage_health_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (storage_health_entries_match(
                    &journal->entries[previous], entry)) {
                return false;
            }
        }
    }
    return true;
}

static bool storage_health_slot_valid(
    const struct candidate_slot_inventory_entry *slot)
{
    return slot != NULL &&
           slot->slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           slot->slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX &&
           slot->generation >= CANDIDATE_SLOT_INVENTORY_GENERATION_MIN &&
           slot->generation <= CANDIDATE_SLOT_INVENTORY_GENERATION_MAX &&
           storage_health_digest_valid(slot->digest);
}

static bool storage_health_inventory_valid(
    const struct candidate_slot_inventory *inventory)
{
    size_t index;
    size_t previous;

    if (inventory == NULL ||
        inventory->version != CANDIDATE_SLOT_INVENTORY_VERSION ||
        !inventory->active ||
        inventory->slot_count == 0U ||
        inventory->slot_count > CANDIDATE_SLOT_INVENTORY_MAX_SLOTS ||
        inventory->generation < CANDIDATE_SLOT_INVENTORY_GENERATION_MIN ||
        inventory->generation > CANDIDATE_SLOT_INVENTORY_GENERATION_MAX) {
        return false;
    }

    for (index = 0U; index < inventory->slot_count; ++index) {
        const struct candidate_slot_inventory_entry *slot =
            &inventory->slots[index];

        if (!storage_health_slot_valid(slot)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (inventory->slots[previous].slot_id == slot->slot_id) {
                return false;
            }
        }
    }
    return true;
}

static bool storage_health_intent_entry_valid(
    const struct activation_intent_entry *entry)
{
    return entry != NULL &&
           entry->slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           entry->slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX &&
           entry->generation >= CANDIDATE_SLOT_INVENTORY_GENERATION_MIN &&
           entry->generation <= CANDIDATE_SLOT_INVENTORY_GENERATION_MAX &&
           storage_health_digest_valid(entry->digest);
}

static bool storage_health_intent_entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           storage_health_bytes_equal(left->digest, right->digest,
                                      ACTIVATION_INTENT_DIGEST_SIZE);
}

static const struct candidate_slot_inventory_entry *
storage_health_inventory_find(
    const struct candidate_slot_inventory *inventory,
    const struct activation_intent_entry *wanted)
{
    size_t index;

    for (index = 0U; index < inventory->slot_count; ++index) {
        const struct candidate_slot_inventory_entry *slot =
            &inventory->slots[index];

        if (slot->slot_id == wanted->slot_id &&
            slot->generation == wanted->generation &&
            storage_health_bytes_equal(
                slot->digest, wanted->digest,
                CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE)) {
            return slot;
        }
    }
    return NULL;
}

static bool storage_health_intent_valid(
    const struct activation_intent_store *intent,
    const struct candidate_slot_inventory *inventory)
{
    if (intent == NULL ||
        intent->version != ACTIVATION_INTENT_VERSION ||
        !intent->initialized ||
        !storage_health_intent_entry_valid(&intent->known_good) ||
        storage_health_inventory_find(
            inventory, &intent->known_good) == NULL) {
        return false;
    }

    if (!intent->pending.pending) {
        return true;
    }

    return intent->pending.version == ACTIVATION_INTENT_VERSION &&
           storage_health_intent_entry_valid(&intent->pending.target) &&
           storage_health_intent_entry_valid(&intent->pending.rollback) &&
           intent->pending.bundle_size >= ACTIVATION_INTENT_BUNDLE_MIN_SIZE &&
           intent->pending.bundle_size <= ACTIVATION_INTENT_BUNDLE_MAX_SIZE &&
           !storage_health_intent_entries_equal(
               &intent->pending.target, &intent->pending.rollback) &&
           !storage_health_intent_entries_equal(
               &intent->pending.target, &intent->known_good) &&
           storage_health_intent_entries_equal(
               &intent->pending.rollback, &intent->known_good) &&
           storage_health_inventory_find(
               inventory, &intent->pending.target) != NULL &&
           storage_health_inventory_find(
               inventory, &intent->pending.rollback) != NULL;
}

enum storage_health_status storage_health_evaluate(
    const struct storage_health_input *input,
    struct storage_health_verdict *verdict_out)
{
    enum storage_health_fault fault = STORAGE_HEALTH_FAULT_NONE;

    if (input == NULL || verdict_out == NULL) {
        return STORAGE_HEALTH_NULL;
    }
    if (input->version != STORAGE_HEALTH_VERSION) {
        return STORAGE_HEALTH_BAD_VERSION;
    }
    if (input->journal == NULL ||
        input->inventory == NULL ||
        input->intent == NULL) {
        return STORAGE_HEALTH_NULL;
    }

    if (!storage_health_journal_valid(input->journal)) {
        fault = STORAGE_HEALTH_FAULT_JOURNAL;
    } else if (input->checkpoint_sequence == 0U ||
               input->checkpoint_sequence !=
                   input->journal->source_sequence) {
        fault = STORAGE_HEALTH_FAULT_CHECKPOINT;
    } else if (!storage_health_inventory_valid(input->inventory)) {
        fault = STORAGE_HEALTH_FAULT_SLOT;
    } else if (!storage_health_intent_valid(
                   input->intent, input->inventory)) {
        fault = STORAGE_HEALTH_FAULT_INTENT;
    }

    storage_health_zero_bytes((unsigned char *)verdict_out,
                              sizeof(*verdict_out));
    verdict_out->version = STORAGE_HEALTH_VERSION;
    if (fault != STORAGE_HEALTH_FAULT_NONE) {
        verdict_out->healthy = false;
        verdict_out->result = HEALTH_CHECK_RESULT_FAIL;
        verdict_out->fault = fault;
        return STORAGE_HEALTH_UNHEALTHY;
    }

    verdict_out->healthy = true;
    verdict_out->result = HEALTH_CHECK_RESULT_PASS;
    verdict_out->fault = STORAGE_HEALTH_FAULT_NONE;
    verdict_out->journal_generation_id =
        input->journal->generation_id;
    verdict_out->checkpoint_sequence = input->checkpoint_sequence;
    verdict_out->slot_generation = input->inventory->generation;
    verdict_out->slot_count = input->inventory->slot_count;
    verdict_out->known_good_slot_id = input->intent->known_good.slot_id;
    return STORAGE_HEALTH_OK;
}

const char *storage_health_status_string(
    enum storage_health_status status)
{
    switch (status) {
    case STORAGE_HEALTH_OK:
        return "STORAGE_HEALTH_OK";
    case STORAGE_HEALTH_NULL:
        return "STORAGE_HEALTH_NULL";
    case STORAGE_HEALTH_BAD_VERSION:
        return "STORAGE_HEALTH_BAD_VERSION";
    case STORAGE_HEALTH_UNHEALTHY:
        return "STORAGE_HEALTH_UNHEALTHY";
    default:
        return NULL;
    }
}

const char *storage_health_fault_string(
    enum storage_health_fault fault)
{
    switch (fault) {
    case STORAGE_HEALTH_FAULT_NONE:
        return "STORAGE_HEALTH_FAULT_NONE";
    case STORAGE_HEALTH_FAULT_JOURNAL:
        return "STORAGE_HEALTH_FAULT_JOURNAL";
    case STORAGE_HEALTH_FAULT_CHECKPOINT:
        return "STORAGE_HEALTH_FAULT_CHECKPOINT";
    case STORAGE_HEALTH_FAULT_SLOT:
        return "STORAGE_HEALTH_FAULT_SLOT";
    case STORAGE_HEALTH_FAULT_INTENT:
        return "STORAGE_HEALTH_FAULT_INTENT";
    default:
        return NULL;
    }
}
