#include "journal_compaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void journal_compaction_zero_bytes(unsigned char *bytes,
                                          size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void journal_compaction_copy_bytes(unsigned char *destination,
                                          const unsigned char *source,
                                          size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool journal_compaction_bytes_equal(const unsigned char *left,
                                           const unsigned char *right,
                                           size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool journal_compaction_entry_valid(
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

static bool journal_compaction_entry_matches(
    const struct config_kv_entry *entry,
    uint8_t kind,
    const unsigned char *key,
    size_t key_length)
{
    return entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           journal_compaction_bytes_equal(entry->key, key, key_length);
}

static bool journal_compaction_kv_valid(const struct config_kv *kv)
{
    size_t occupied = 0U;
    size_t index;

    if (kv == NULL ||
        !kv->initialized ||
        kv->entries == NULL ||
        kv->capacity == 0U ||
        kv->capacity > CONFIG_KV_CAPACITY_MAX ||
        kv->count > kv->capacity) {
        return false;
    }

    for (index = 0U; index < kv->capacity; ++index) {
        const struct config_kv_entry *entry = &kv->entries[index];
        size_t previous;

        if (!entry->occupied) {
            continue;
        }
        if (!journal_compaction_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (journal_compaction_entry_matches(
                    &kv->entries[previous], entry->kind, entry->key,
                    entry->key_length)) {
                return false;
            }
        }
        ++occupied;
    }
    return occupied == kv->count;
}

static bool journal_compaction_entry_less(
    const struct config_kv_entry *left,
    const struct config_kv_entry *right)
{
    size_t index;

    if (left->kind != right->kind) {
        return left->kind < right->kind;
    }
    if (left->key_length != right->key_length) {
        return left->key_length < right->key_length;
    }
    for (index = 0U; index < left->key_length; ++index) {
        if (left->key[index] != right->key[index]) {
            return left->key[index] < right->key[index];
        }
    }
    return left->revision < right->revision;
}

static void journal_compaction_sort_entries(
    const struct config_kv_entry **entries,
    size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const struct config_kv_entry *entry = entries[index];
        size_t position = index;

        while (position > 0U &&
               journal_compaction_entry_less(entry,
                                             entries[position - 1U])) {
            entries[position] = entries[position - 1U];
            --position;
        }
        entries[position] = entry;
    }
}

static void journal_compaction_copy_entry(
    struct config_kv_entry *destination,
    const struct config_kv_entry *source)
{
    journal_compaction_copy_bytes((unsigned char *)destination,
                                  (const unsigned char *)source,
                                  sizeof(*destination));
}

static bool journal_compaction_entries_valid(
    const struct config_kv_entry *entries,
    size_t count,
    uint32_t config_count,
    uint32_t audit_count)
{
    size_t occupied = 0U;
    uint32_t seen_config = 0U;
    uint32_t seen_audit = 0U;
    size_t index;

    if (count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        config_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        audit_count > JOURNAL_COMPACTION_MAX_ENTRIES ||
        (uint64_t)config_count + audit_count != count) {
        return false;
    }
    if (count > 0U && entries == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        const struct config_kv_entry *entry = &entries[index];
        size_t previous;

        if (!journal_compaction_entry_valid(entry)) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (journal_compaction_entry_matches(
                    &entries[previous], entry->kind, entry->key,
                    entry->key_length)) {
                return false;
            }
        }
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++seen_config;
        } else {
            ++seen_audit;
        }
        ++occupied;
    }
    return occupied == count &&
           seen_config == config_count &&
           seen_audit == audit_count;
}

static bool journal_compaction_candidate_valid(
    const struct journal_compaction_candidate *candidate)
{
    return candidate != NULL &&
           candidate->valid &&
           candidate->source_sequence != 0U &&
           journal_compaction_entries_valid(
               candidate->entries,
               candidate->count,
               candidate->config_count,
               candidate->audit_count);
}

static bool journal_compaction_generation_valid(
    const struct journal_compaction_generation *generation)
{
    return generation != NULL &&
           generation->valid &&
           generation->generation_id != 0U &&
           generation->source_sequence != 0U &&
           journal_compaction_entries_valid(
               generation->entries,
               generation->count,
               generation->config_count,
               generation->audit_count);
}

static bool journal_compaction_commit_structure_valid(
    const struct journal_compaction_commit *commit,
    bool allow_interrupted)
{
    size_t active_count = 0U;
    size_t interrupted_count = 0U;
    size_t index;
    size_t previous;

    if (commit == NULL ||
        !commit->initialized ||
        commit->active_slot >=
            JOURNAL_COMPACTION_GENERATION_SLOT_COUNT ||
        commit->next_generation == 0U) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        const struct journal_compaction_generation *slot =
            &commit->slots[index];

        if (slot->interrupted) {
            ++interrupted_count;
            if (!allow_interrupted || interrupted_count > 1U) {
                return false;
            }
        }
        if (slot->active) {
            ++active_count;
            if (index != commit->active_slot ||
                !journal_compaction_generation_valid(slot) ||
                !slot->staged ||
                slot->interrupted) {
                return false;
            }
        } else if (slot->valid) {
            if (!journal_compaction_generation_valid(slot) ||
                (!slot->interrupted && !slot->staged)) {
                return false;
            }
        }
    }
    if (active_count != 1U) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        for (previous = 0U; previous < index; ++previous) {
            const struct journal_compaction_generation *left =
                &commit->slots[index];
            const struct journal_compaction_generation *right =
                &commit->slots[previous];

            if (left->valid && right->valid &&
                (left->generation_id == right->generation_id ||
                 left->source_sequence == right->source_sequence)) {
                return false;
            }
        }
    }
    return commit->next_generation >
           commit->slots[commit->active_slot].generation_id;
}

static void journal_compaction_copy_candidate_to_generation(
    struct journal_compaction_generation *generation,
    const struct journal_compaction_candidate *candidate,
    uint64_t generation_id)
{
    size_t index;

    journal_compaction_zero_bytes((unsigned char *)generation,
                                  sizeof(*generation));
    generation->generation_id = generation_id;
    generation->source_sequence = candidate->source_sequence;
    for (index = 0U; index < candidate->count; ++index) {
        journal_compaction_copy_entry(&generation->entries[index],
                                      &candidate->entries[index]);
    }
    generation->count = candidate->count;
    generation->config_count = candidate->config_count;
    generation->audit_count = candidate->audit_count;
    generation->valid = true;
    generation->active = false;
    generation->staged = false;
    generation->interrupted = false;
}

enum journal_compaction_status journal_compaction_select(
    struct journal_compaction_candidate *candidate,
    const struct journal_compaction_checkpoint *checkpoints,
    size_t count)
{
    const struct journal_compaction_checkpoint *selected;
    const struct config_kv_entry *refs[JOURNAL_COMPACTION_MAX_ENTRIES];
    size_t index;
    size_t previous;
    uint32_t config_count = 0U;
    uint32_t audit_count = 0U;

    if (candidate == NULL ||
        (count > 0U && checkpoints == NULL)) {
        return JOURNAL_COMPACTION_NULL;
    }
    if (count == 0U ||
        count > JOURNAL_COMPACTION_MAX_CHECKPOINTS) {
        return JOURNAL_COMPACTION_BAD_CAPACITY;
    }

    for (index = 0U; index < count; ++index) {
        if (checkpoints[index].kv == NULL) {
            return JOURNAL_COMPACTION_NULL;
        }
        if (checkpoints[index].version !=
            JOURNAL_COMPACTION_CHECKPOINT_VERSION) {
            return JOURNAL_COMPACTION_BAD_VERSION;
        }
        if (checkpoints[index].sequence == 0U) {
            return JOURNAL_COMPACTION_BAD_SEQUENCE;
        }
        if (!journal_compaction_kv_valid(checkpoints[index].kv)) {
            return JOURNAL_COMPACTION_BAD_STATE;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (checkpoints[previous].sequence ==
                checkpoints[index].sequence) {
                return JOURNAL_COMPACTION_DUPLICATE;
            }
        }
    }

    selected = &checkpoints[0];
    for (index = 1U; index < count; ++index) {
        if (checkpoints[index].sequence > selected->sequence) {
            selected = &checkpoints[index];
        }
    }

    journal_compaction_zero_bytes((unsigned char *)candidate,
                                  sizeof(*candidate));
    for (index = 0U; index < selected->kv->capacity; ++index) {
        const struct config_kv_entry *entry = &selected->kv->entries[index];
        size_t ref_index;

        if (!entry->occupied) {
            continue;
        }
        for (ref_index = 0U; ref_index < index; ++ref_index) {
            const struct config_kv_entry *candidate_ref =
                &selected->kv->entries[ref_index];

            if (journal_compaction_entry_matches(
                    candidate_ref, entry->kind, entry->key,
                    entry->key_length)) {
                return JOURNAL_COMPACTION_BAD_STATE;
            }
        }
    }

    index = 0U;
    for (previous = 0U; previous < selected->kv->capacity; ++previous) {
        const struct config_kv_entry *entry =
            &selected->kv->entries[previous];

        if (entry->occupied) {
            refs[index] = entry;
            ++index;
        }
    }
    journal_compaction_sort_entries(refs, index);

    for (previous = 0U; previous < index; ++previous) {
        const struct config_kv_entry *entry = refs[previous];

        journal_compaction_copy_entry(&candidate->entries[previous], entry);
        if (entry->kind == CONFIG_KV_KIND_CONFIG) {
            ++config_count;
        } else {
            ++audit_count;
        }
    }

    candidate->count = index;
    candidate->config_count = config_count;
    candidate->audit_count = audit_count;
    candidate->source_sequence = selected->sequence;
    candidate->valid = true;
    return JOURNAL_COMPACTION_OK;
}

enum journal_compaction_status journal_compaction_commit_init(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_candidate *initial)
{
    if (commit == NULL || initial == NULL) {
        return JOURNAL_COMPACTION_NULL;
    }
    if (!journal_compaction_candidate_valid(initial)) {
        return JOURNAL_COMPACTION_BAD_STATE;
    }
    if (commit->initialized) {
        return JOURNAL_COMPACTION_BAD_GENERATION;
    }

    journal_compaction_zero_bytes((unsigned char *)commit,
                                  sizeof(*commit));
    journal_compaction_copy_candidate_to_generation(
        &commit->slots[0],
        initial,
        JOURNAL_COMPACTION_INITIAL_GENERATION);
    commit->slots[0].staged = true;
    commit->slots[0].active = true;
    commit->active_slot = 0U;
    commit->next_generation =
        JOURNAL_COMPACTION_INITIAL_GENERATION + 1U;
    commit->initialized = true;
    return JOURNAL_COMPACTION_OK;
}

enum journal_compaction_status journal_compaction_publish(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_candidate *candidate,
    const struct journal_compaction_io *io)
{
    struct journal_compaction_generation *staged;
    struct journal_compaction_generation *active;
    size_t staged_slot;

    if (commit == NULL || candidate == NULL || io == NULL) {
        return JOURNAL_COMPACTION_NULL;
    }
    if (!commit->initialized ||
        !journal_compaction_commit_structure_valid(commit, false)) {
        return JOURNAL_COMPACTION_BAD_GENERATION;
    }
    if (!journal_compaction_candidate_valid(candidate)) {
        return JOURNAL_COMPACTION_BAD_STATE;
    }
    active = &commit->slots[commit->active_slot];
    if (candidate->source_sequence <= active->source_sequence) {
        return JOURNAL_COMPACTION_BAD_SOURCE;
    }
    if (commit->next_generation == UINT64_MAX) {
        return JOURNAL_COMPACTION_BAD_GENERATION;
    }

    staged_slot = 1U - (size_t)commit->active_slot;
    staged = &commit->slots[staged_slot];
    journal_compaction_copy_candidate_to_generation(
        staged, candidate, commit->next_generation);

    if (io->write != NULL &&
        !io->write(io->context, 0U, staged_slot, staged)) {
        staged->interrupted = true;
        return JOURNAL_COMPACTION_WRITE_FAULT;
    }
    staged->staged = true;
    if (io->write != NULL &&
        !io->write(io->context, 1U, staged_slot, staged)) {
        staged->interrupted = true;
        return JOURNAL_COMPACTION_WRITE_FAULT;
    }

    active->active = false;
    staged->active = true;
    commit->active_slot = (uint8_t)staged_slot;
    ++commit->next_generation;
    return JOURNAL_COMPACTION_OK;
}

enum journal_compaction_status journal_compaction_recover(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_io *io)
{
    struct journal_compaction_generation *interrupted = NULL;
    struct journal_compaction_generation *active;
    size_t interrupted_slot = SIZE_MAX;
    size_t interrupted_count = 0U;
    size_t index;

    if (commit == NULL || io == NULL) {
        return JOURNAL_COMPACTION_NULL;
    }
    if (!commit->initialized ||
        !journal_compaction_commit_structure_valid(commit, true)) {
        return JOURNAL_COMPACTION_BAD_GENERATION;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        if (commit->slots[index].interrupted) {
            ++interrupted_count;
            interrupted = &commit->slots[index];
            interrupted_slot = index;
        }
    }
    if (interrupted_count == 0U) {
        return JOURNAL_COMPACTION_OK;
    }
    if (interrupted_count != 1U || interrupted == NULL) {
        return JOURNAL_COMPACTION_BAD_GENERATION;
    }

    active = &commit->slots[commit->active_slot];
    if (!interrupted->valid ||
        !interrupted->staged ||
        interrupted->source_sequence <= active->source_sequence ||
        interrupted->generation_id != commit->next_generation) {
        journal_compaction_zero_bytes(
            (unsigned char *)interrupted, sizeof(*interrupted));
        return JOURNAL_COMPACTION_OK;
    }

    if (io->write != NULL &&
        !io->write(io->context, 1U, interrupted_slot, interrupted)) {
        journal_compaction_zero_bytes(
            (unsigned char *)interrupted, sizeof(*interrupted));
        return JOURNAL_COMPACTION_OK;
    }

    active->active = false;
    interrupted->active = true;
    interrupted->interrupted = false;
    commit->active_slot = (uint8_t)interrupted_slot;
    ++commit->next_generation;
    return JOURNAL_COMPACTION_OK;
}

const struct journal_compaction_generation *journal_compaction_active(
    const struct journal_compaction_commit *commit)
{
    if (commit == NULL ||
        !commit->initialized ||
        commit->active_slot >=
            JOURNAL_COMPACTION_GENERATION_SLOT_COUNT) {
        return NULL;
    }
    return &commit->slots[commit->active_slot];
}

const char *journal_compaction_status_string(
    enum journal_compaction_status status)
{
    switch (status) {
    case JOURNAL_COMPACTION_OK:
        return "JOURNAL_COMPACTION_OK";
    case JOURNAL_COMPACTION_NULL:
        return "JOURNAL_COMPACTION_NULL";
    case JOURNAL_COMPACTION_BAD_STATE:
        return "JOURNAL_COMPACTION_BAD_STATE";
    case JOURNAL_COMPACTION_BAD_CAPACITY:
        return "JOURNAL_COMPACTION_BAD_CAPACITY";
    case JOURNAL_COMPACTION_BAD_VERSION:
        return "JOURNAL_COMPACTION_BAD_VERSION";
    case JOURNAL_COMPACTION_BAD_SEQUENCE:
        return "JOURNAL_COMPACTION_BAD_SEQUENCE";
    case JOURNAL_COMPACTION_DUPLICATE:
        return "JOURNAL_COMPACTION_DUPLICATE";
    case JOURNAL_COMPACTION_BAD_GENERATION:
        return "JOURNAL_COMPACTION_BAD_GENERATION";
    case JOURNAL_COMPACTION_BAD_SOURCE:
        return "JOURNAL_COMPACTION_BAD_SOURCE";
    case JOURNAL_COMPACTION_WRITE_FAULT:
        return "JOURNAL_COMPACTION_WRITE_FAULT";
    default:
        return "JOURNAL_COMPACTION_UNKNOWN";
    }
}
