#include "restore_validator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool restore_entry_valid(const struct config_kv_entry *entry)
{
    return entry != NULL &&
           entry->occupied &&
           entry->kind < CONFIG_KV_KIND_COUNT &&
           entry->key_length > 0U &&
           entry->key_length <= CONFIG_KV_KEY_MAX &&
           entry->value_length <= CONFIG_KV_VALUE_MAX &&
           entry->revision != 0U;
}

static bool restore_kv_valid(const struct config_kv *kv)
{
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
        if (kv->entries[index].occupied &&
            !restore_entry_valid(&kv->entries[index])) {
            return false;
        }
    }
    return true;
}

static bool restore_plan_valid(const struct restore_plan *plan)
{
    size_t config_count = 0U;
    size_t audit_count = 0U;
    size_t index;

    if (plan == NULL ||
        !plan->valid ||
        plan->count > RESTORE_PLAN_MAX_ENTRIES) {
        return false;
    }
    if ((size_t)plan->counts.config_count + plan->counts.audit_count !=
        plan->count) {
        return false;
    }
    for (index = 0U; index < plan->count; ++index) {
        if (!restore_entry_valid(&plan->entries[index])) {
            return false;
        }
        if (plan->entries[index].kind == CONFIG_KV_KIND_CONFIG) {
            ++config_count;
        } else {
            ++audit_count;
        }
    }
    return config_count == plan->counts.config_count &&
           audit_count == plan->counts.audit_count;
}

static bool restore_entries_equal(const struct config_kv_entry *left,
                                  const struct config_kv_entry *right)
{
    size_t index;

    if (left->occupied != right->occupied ||
        left->kind != right->kind ||
        left->key_length != right->key_length ||
        left->value_length != right->value_length ||
        left->revision != right->revision) {
        return false;
    }
    for (index = 0U; index < CONFIG_KV_KEY_MAX; ++index) {
        if (left->key[index] != right->key[index]) {
            return false;
        }
    }
    for (index = 0U; index < CONFIG_KV_VALUE_MAX; ++index) {
        if (left->value[index] != right->value[index]) {
            return false;
        }
    }
    return true;
}

static bool restore_state_equal(const struct config_kv *active,
                                const struct restore_plan *plan)
{
    size_t active_count = 0U;
    size_t matched = 0U;
    size_t index;

    for (index = 0U; index < active->capacity; ++index) {
        if (active->entries[index].occupied) {
            ++active_count;
        }
    }
    if (active_count != plan->count) {
        return false;
    }
    for (index = 0U; index < active->capacity; ++index) {
        size_t plan_index;

        if (!active->entries[index].occupied) {
            continue;
        }
        for (plan_index = 0U; plan_index < plan->count; ++plan_index) {
            if (restore_entries_equal(&active->entries[index],
                                      &plan->entries[plan_index])) {
                ++matched;
                break;
            }
        }
    }
    return matched == plan->count;
}

static void restore_copy_entry(struct config_kv_entry *destination,
                               const struct config_kv_entry *source)
{
    size_t index;

    destination->occupied = source->occupied;
    destination->kind = source->kind;
    destination->key_length = source->key_length;
    for (index = 0U; index < CONFIG_KV_KEY_MAX; ++index) {
        destination->key[index] = source->key[index];
    }
    destination->value_length = source->value_length;
    for (index = 0U; index < CONFIG_KV_VALUE_MAX; ++index) {
        destination->value[index] = source->value[index];
    }
    destination->revision = source->revision;
}

static void restore_zero_entry(struct config_kv_entry *entry)
{
    size_t index;

    entry->occupied = false;
    entry->kind = 0U;
    entry->key_length = 0U;
    for (index = 0U; index < CONFIG_KV_KEY_MAX; ++index) {
        entry->key[index] = 0U;
    }
    entry->value_length = 0U;
    for (index = 0U; index < CONFIG_KV_VALUE_MAX; ++index) {
        entry->value[index] = 0U;
    }
    entry->revision = 0U;
}

static void restore_zero_plan(struct restore_plan *plan)
{
    unsigned char *bytes = (unsigned char *)plan;
    size_t index;

    for (index = 0U; index < sizeof(*plan); ++index) {
        bytes[index] = 0U;
    }
}

static void restore_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool restore_activation_valid(
    const struct restore_activation *activation)
{
    size_t index;

    if (activation == NULL ||
        !activation->initialized ||
        activation->active_slot >= RESTORE_STAGE_SLOT_COUNT) {
        return false;
    }
    for (index = 0U; index < RESTORE_STAGE_SLOT_COUNT; ++index) {
        if (!restore_kv_valid(&activation->slots[index])) {
            return false;
        }
    }
    return activation->slots[0].capacity ==
           activation->slots[1].capacity;
}

static void restore_stage_discard(struct config_kv *staged)
{
    size_t index;

    for (index = 0U; index < staged->capacity; ++index) {
        restore_zero_entry(&staged->entries[index]);
    }
    staged->count = 0U;
    staged->initialized = true;
}

static void restore_stage_copy_plan(struct config_kv *staged,
                                    const struct restore_plan *plan)
{
    size_t index;

    for (index = 0U; index < plan->count; ++index) {
        restore_copy_entry(&staged->entries[index],
                           &plan->entries[index]);
    }
    for (index = plan->count; index < staged->capacity; ++index) {
        restore_zero_entry(&staged->entries[index]);
    }
    staged->count = plan->count;
    staged->initialized = true;
}

static bool restore_io_allowed(const struct restore_io *io,
                               size_t write_index)
{
    if (io == NULL || io->write == NULL) {
        return true;
    }
    return io->write(io->context, write_index);
}

static void restore_rollback_active(struct config_kv *active,
                                    const struct config_kv_entry *rollback,
                                    size_t rollback_capacity,
                                    size_t previous_count)
{
    size_t index;

    for (index = 0U;
         index < rollback_capacity && index < active->capacity;
         ++index) {
        restore_copy_entry(&active->entries[index], &rollback[index]);
    }
    active->count = previous_count;
    active->initialized = true;
}

static enum restore_status restore_backup_status(
    enum backup_stream_status status)
{
    switch (status) {
    case BACKUP_STREAM_OK:
        return RESTORE_OK;
    case BACKUP_STREAM_NULL:
        return RESTORE_NULL;
    case BACKUP_STREAM_BAD_STATE:
        return RESTORE_BAD_STATE;
    case BACKUP_STREAM_OVERFLOW:
        return RESTORE_OVERFLOW;
    case BACKUP_STREAM_TOO_SMALL:
    case BACKUP_STREAM_UNSUPPORTED:
    case BACKUP_STREAM_CORRUPT:
        return RESTORE_CORRUPT;
    default:
        return RESTORE_BAD_STATE;
    }
}

enum restore_status restore_validate(
    const void *stream,
    size_t length,
    struct restore_plan *plan)
{
    struct backup_stream_layout layout;
    struct backup_stream_counts counts;
    enum backup_stream_status framing_status;
    enum backup_stream_status stream_status;
    size_t entry_count = 0U;

    if (stream == NULL || plan == NULL) {
        return RESTORE_NULL;
    }
    framing_status = backup_stream_frame_validate(stream, length, &layout);
    if (framing_status != BACKUP_STREAM_OK) {
        return restore_backup_status(framing_status);
    }
    stream_status = backup_stream_import(
        stream, length, NULL, 0U, &entry_count, &counts);
    if (stream_status != BACKUP_STREAM_OK) {
        return restore_backup_status(stream_status);
    }

    restore_zero_plan(plan);
    stream_status = backup_stream_import(
        stream, length, plan->entries, RESTORE_PLAN_MAX_ENTRIES,
        &entry_count, &counts);
    if (stream_status != BACKUP_STREAM_OK) {
        return restore_backup_status(stream_status);
    }
    plan->count = entry_count;
    plan->counts = counts;
    plan->valid = true;
    plan->dry_run = false;
    return RESTORE_OK;
}

enum restore_status restore_dry_run_plan(
    const void *stream,
    size_t length,
    struct restore_plan *plan)
{
    enum restore_status status;

    if (stream == NULL || plan == NULL) {
        return RESTORE_NULL;
    }
    status = restore_validate(stream, length, plan);
    if (status != RESTORE_OK) {
        return status;
    }
    plan->dry_run = true;
    return RESTORE_OK;
}

enum restore_status restore_commit(
    const struct restore_plan *plan,
    struct config_kv *active,
    const struct restore_io *io)
{
    size_t previous_count;
    size_t write_index = 0U;
    size_t index;

    if (plan == NULL || active == NULL) {
        return RESTORE_NULL;
    }
    if (!restore_plan_valid(plan) || !restore_kv_valid(active)) {
        return RESTORE_BAD_STATE;
    }
    if (plan->dry_run) {
        return RESTORE_DRY_RUN;
    }
    if (plan->count > active->capacity) {
        return RESTORE_OVERFLOW;
    }
    if (io == NULL ||
        io->rollback == NULL ||
        io->rollback_capacity < active->capacity) {
        return RESTORE_BAD_STATE;
    }
    if (restore_state_equal(active, plan)) {
        return RESTORE_NO_CHANGE;
    }

    for (index = 0U; index < active->capacity; ++index) {
        restore_copy_entry(&io->rollback[index], &active->entries[index]);
    }
    previous_count = active->count;

    for (index = 0U; index < plan->count; ++index) {
        if (!restore_io_allowed(io, write_index)) {
            restore_rollback_active(active, io->rollback,
                                    io->rollback_capacity,
                                    previous_count);
            return RESTORE_WRITE_FAULT;
        }
        restore_copy_entry(&active->entries[index], &plan->entries[index]);
        ++write_index;
    }
    for (index = plan->count; index < active->capacity; ++index) {
        if (!restore_io_allowed(io, write_index)) {
            restore_rollback_active(active, io->rollback,
                                    io->rollback_capacity,
                                    previous_count);
            return RESTORE_WRITE_FAULT;
        }
        restore_zero_entry(&active->entries[index]);
        ++write_index;
    }

    active->count = plan->count;
    active->initialized = true;
    return RESTORE_OK;
}

enum restore_status restore_apply(
    const void *stream,
    size_t length,
    struct config_kv *active,
    const struct restore_io *io)
{
    struct restore_plan plan;
    enum restore_status status;

    status = restore_validate(stream, length, &plan);
    if (status != RESTORE_OK) {
        return status;
    }
    return restore_commit(&plan, active, io);
}

const char *restore_status_string(enum restore_status status)
{
    switch (status) {
    case RESTORE_OK:
        return "RESTORE_OK";
    case RESTORE_NULL:
        return "RESTORE_NULL";
    case RESTORE_BAD_STATE:
        return "RESTORE_BAD_STATE";
    case RESTORE_CORRUPT:
        return "RESTORE_CORRUPT";
    case RESTORE_OVERFLOW:
        return "RESTORE_OVERFLOW";
    case RESTORE_NO_CHANGE:
        return "RESTORE_NO_CHANGE";
    case RESTORE_WRITE_FAULT:
        return "RESTORE_WRITE_FAULT";
    case RESTORE_DRY_RUN:
        return "RESTORE_DRY_RUN";
    case RESTORE_BAD_STAGE:
        return "RESTORE_BAD_STAGE";
    default:
        return "RESTORE_UNKNOWN";
    }
}

enum restore_status restore_activation_init(
    struct restore_activation *activation,
    struct config_kv_entry *active_entries,
    struct config_kv_entry *staged_entries,
    size_t capacity)
{
    if (activation == NULL ||
        active_entries == NULL ||
        staged_entries == NULL) {
        return RESTORE_NULL;
    }
    if (capacity == 0U ||
        capacity > CONFIG_KV_CAPACITY_MAX ||
        activation->initialized) {
        return RESTORE_BAD_STAGE;
    }

    restore_zero_bytes((unsigned char *)activation, sizeof(*activation));
    if (config_kv_init(&activation->slots[0],
                       active_entries, capacity) != CONFIG_KV_OK ||
        config_kv_init(&activation->slots[1],
                       staged_entries, capacity) != CONFIG_KV_OK) {
        return RESTORE_BAD_STAGE;
    }
    activation->active_slot = 0U;
    activation->initialized = true;
    return RESTORE_OK;
}

enum restore_status restore_stage_apply(
    struct restore_activation *activation,
    const struct restore_plan *plan,
    const struct restore_stage_io *io)
{
    struct config_kv *staged;
    const struct config_kv *active;
    size_t staged_slot;

    if (activation == NULL || plan == NULL || io == NULL) {
        return RESTORE_NULL;
    }
    if (!restore_activation_valid(activation)) {
        return RESTORE_BAD_STAGE;
    }
    if (!restore_plan_valid(plan)) {
        return RESTORE_BAD_STATE;
    }
    if (plan->dry_run) {
        return RESTORE_DRY_RUN;
    }
    active = &activation->slots[activation->active_slot];
    if (plan->count > active->capacity) {
        return RESTORE_OVERFLOW;
    }
    if (restore_state_equal(active, plan)) {
        return RESTORE_NO_CHANGE;
    }

    staged_slot = 1U - (size_t)activation->active_slot;
    staged = &activation->slots[staged_slot];
    restore_stage_copy_plan(staged, plan);

    if (io->write != NULL &&
        !io->write(io->context, 0U, staged_slot, plan)) {
        restore_stage_discard(staged);
        return RESTORE_WRITE_FAULT;
    }
    if (io->write != NULL &&
        !io->write(io->context, 1U, staged_slot, plan)) {
        restore_stage_discard(staged);
        return RESTORE_WRITE_FAULT;
    }

    activation->active_slot = (uint8_t)staged_slot;
    return RESTORE_OK;
}

const struct config_kv *restore_activation_active(
    const struct restore_activation *activation)
{
    if (activation == NULL ||
        !activation->initialized ||
        activation->active_slot >= RESTORE_STAGE_SLOT_COUNT ||
        !restore_kv_valid(&activation->slots[activation->active_slot])) {
        return NULL;
    }
    return &activation->slots[activation->active_slot];
}
