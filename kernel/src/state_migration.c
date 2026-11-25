#include "state_migration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void state_migration_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void state_migration_copy_bytes(unsigned char *destination,
                                       const unsigned char *source,
                                       size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool state_migration_bytes_equal(const unsigned char *left,
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

static bool state_migration_id_equal(const struct state_migration_step *left,
                                     const struct state_migration_step *right)
{
    return left->kind == right->kind &&
           left->target_kind == right->target_kind &&
           left->id_length == right->id_length &&
           state_migration_bytes_equal(left->id, right->id,
                                       left->id_length);
}

static enum state_migration_status state_migration_schema_status(
    enum state_schema_status status)
{
    switch (status) {
    case STATE_SCHEMA_OK:
        return STATE_MIGRATION_OK;
    case STATE_SCHEMA_NULL:
        return STATE_MIGRATION_NULL;
    case STATE_SCHEMA_BAD_KIND:
        return STATE_MIGRATION_BAD_KIND;
    case STATE_SCHEMA_BAD_ID:
        return STATE_MIGRATION_BAD_ID;
    case STATE_SCHEMA_BAD_VERSION:
        return STATE_MIGRATION_BAD_VERSION;
    case STATE_SCHEMA_BAD_LENGTH:
    case STATE_SCHEMA_RECORD_TOO_SHORT:
    case STATE_SCHEMA_RECORD_TOO_LONG:
        return STATE_MIGRATION_BAD_LENGTH;
    case STATE_SCHEMA_BAD_STATE:
    case STATE_SCHEMA_DUPLICATE:
    case STATE_SCHEMA_BAD_CAPACITY:
    case STATE_SCHEMA_PUBLISH_FAILED:
        return STATE_MIGRATION_BAD_PLAN;
    case STATE_SCHEMA_UNKNOWN:
        return STATE_MIGRATION_UNKNOWN;
    case STATE_SCHEMA_UNSUPPORTED_VERSION:
        return STATE_MIGRATION_UNSUPPORTED_VERSION;
    default:
        return STATE_MIGRATION_BAD_PLAN;
    }
}

static enum state_migration_status state_migration_step_status(
    const struct state_schema_registry *registry,
    const struct state_migration_step *step)
{
    enum state_schema_status schema_status;
    enum state_migration_status status;

    if (registry == NULL || step == NULL) {
        return STATE_MIGRATION_NULL;
    }
    if (step->kind != STATE_SCHEMA_KIND_KV) {
        return STATE_MIGRATION_BAD_KIND;
    }
    if (step->target_kind >= CONFIG_KV_KIND_COUNT) {
        return STATE_MIGRATION_BAD_TARGET;
    }
    if (step->from_version == 0U ||
        step->to_version == 0U ||
        step->to_version <= step->from_version) {
        return STATE_MIGRATION_BAD_VERSION;
    }
    if (step->key_length == 0U ||
        step->key_length > CONFIG_KV_KEY_MAX ||
        step->value_length > CONFIG_KV_VALUE_MAX) {
        return STATE_MIGRATION_BAD_LENGTH;
    }

    schema_status = state_schema_validate_record(
        registry,
        STATE_SCHEMA_KIND_KV,
        step->target_kind,
        step->id,
        step->id_length,
        step->from_version,
        step->value_length);
    status = state_migration_schema_status(schema_status);
    if (status != STATE_MIGRATION_OK) {
        return status;
    }
    schema_status = state_schema_validate_record(
        registry,
        STATE_SCHEMA_KIND_KV,
        step->target_kind,
        step->id,
        step->id_length,
        step->to_version,
        step->value_length);
    return state_migration_schema_status(schema_status);
}

static bool state_migration_entry_valid(
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

static bool state_migration_active_valid(const struct config_kv *active)
{
    size_t index;

    if (active == NULL ||
        !active->initialized ||
        active->entries == NULL ||
        active->capacity == 0U ||
        active->capacity > CONFIG_KV_CAPACITY_MAX ||
        active->count > active->capacity) {
        return false;
    }
    for (index = 0U; index < active->capacity; ++index) {
        if (active->entries[index].occupied &&
            !state_migration_entry_valid(&active->entries[index])) {
            return false;
        }
    }
    return true;
}

static void state_migration_copy_entry(
    struct config_kv_entry *destination,
    const struct config_kv_entry *source)
{
    state_migration_copy_bytes((unsigned char *)destination,
                               (const unsigned char *)source,
                               sizeof(*destination));
}

static void state_migration_restore_active(
    struct config_kv *active,
    const struct config_kv_entry *rollback,
    size_t rollback_capacity,
    size_t previous_count)
{
    size_t index;

    for (index = 0U;
         index < rollback_capacity && index < active->capacity;
         ++index) {
        state_migration_copy_entry(&active->entries[index],
                                   &rollback[index]);
    }
    active->count = previous_count;
    active->initialized = true;
}

enum state_migration_status state_migration_plan_init(
    struct state_migration_plan *plan,
    const struct state_schema_registry *registry,
    const struct state_migration_step *steps,
    size_t count)
{
    enum state_migration_status status;
    size_t index;
    size_t previous;

    if (plan == NULL ||
        registry == NULL ||
        (count > 0U && steps == NULL)) {
        return STATE_MIGRATION_NULL;
    }
    if (count == 0U ||
        count > STATE_MIGRATION_MAX_STEPS) {
        return STATE_MIGRATION_BAD_CAPACITY;
    }

    for (index = 0U; index < count; ++index) {
        status = state_migration_step_status(registry, &steps[index]);
        if (status != STATE_MIGRATION_OK) {
            return status;
        }
    }
    for (index = 0U; index < count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (state_migration_id_equal(&steps[index],
                                         &steps[previous])) {
                return STATE_MIGRATION_DUPLICATE;
            }
        }
    }

    state_migration_zero_bytes((unsigned char *)plan, sizeof(*plan));
    for (index = 0U; index < count; ++index) {
        state_migration_copy_bytes(
            (unsigned char *)&plan->steps[index],
            (const unsigned char *)&steps[index],
            sizeof(steps[index]));
    }
    plan->count = count;
    plan->valid = true;
    return STATE_MIGRATION_OK;
}

enum state_migration_status state_migration_commit(
    struct config_kv *active,
    const struct state_schema_registry *registry,
    const struct state_migration_plan *plan,
    const struct state_migration_io *io)
{
    enum state_migration_status status;
    enum config_kv_status kv_status;
    size_t previous_count;
    size_t index;

    if (active == NULL ||
        registry == NULL ||
        plan == NULL ||
        io == NULL) {
        return STATE_MIGRATION_NULL;
    }
    if (!plan->valid ||
        plan->count == 0U ||
        plan->count > STATE_MIGRATION_MAX_STEPS) {
        return STATE_MIGRATION_BAD_PLAN;
    }
    if (!state_migration_active_valid(active)) {
        return STATE_MIGRATION_BAD_STATE;
    }
    if (plan->count > active->capacity) {
        return STATE_MIGRATION_OVERFLOW;
    }
    if (io->rollback == NULL ||
        io->rollback_capacity < active->capacity) {
        return STATE_MIGRATION_BAD_STATE;
    }

    for (index = 0U; index < plan->count; ++index) {
        status = state_migration_step_status(registry,
                                             &plan->steps[index]);
        if (status != STATE_MIGRATION_OK) {
            return status;
        }
    }

    for (index = 0U; index < active->capacity; ++index) {
        state_migration_copy_entry(&io->rollback[index],
                                   &active->entries[index]);
    }
    previous_count = active->count;

    for (index = 0U; index < plan->count; ++index) {
        const struct state_migration_step *step = &plan->steps[index];
        bool replaced = false;

        if (io->write != NULL && !io->write(io->context, index)) {
            state_migration_restore_active(
                active, io->rollback, io->rollback_capacity,
                previous_count);
            return STATE_MIGRATION_WRITE_FAULT;
        }
        kv_status = config_kv_put(
            active,
            step->target_kind,
            step->key,
            step->key_length,
            step->value,
            step->value_length,
            &replaced);
        if (kv_status != CONFIG_KV_OK) {
            state_migration_restore_active(
                active, io->rollback, io->rollback_capacity,
                previous_count);
            return STATE_MIGRATION_PUBLISH_FAILED;
        }
    }
    return STATE_MIGRATION_OK;
}

enum state_migration_status state_migration_apply(
    struct config_kv *active,
    const struct state_schema_registry *registry,
    const struct state_migration_step *steps,
    size_t count,
    const struct state_migration_io *io)
{
    struct state_migration_plan plan;
    enum state_migration_status status;

    status = state_migration_plan_init(&plan, registry, steps, count);
    if (status != STATE_MIGRATION_OK) {
        return status;
    }
    return state_migration_commit(active, registry, &plan, io);
}

const char *state_migration_status_string(
    enum state_migration_status status)
{
    switch (status) {
    case STATE_MIGRATION_OK:
        return "STATE_MIGRATION_OK";
    case STATE_MIGRATION_NULL:
        return "STATE_MIGRATION_NULL";
    case STATE_MIGRATION_BAD_STATE:
        return "STATE_MIGRATION_BAD_STATE";
    case STATE_MIGRATION_BAD_PLAN:
        return "STATE_MIGRATION_BAD_PLAN";
    case STATE_MIGRATION_BAD_CAPACITY:
        return "STATE_MIGRATION_BAD_CAPACITY";
    case STATE_MIGRATION_BAD_KIND:
        return "STATE_MIGRATION_BAD_KIND";
    case STATE_MIGRATION_BAD_TARGET:
        return "STATE_MIGRATION_BAD_TARGET";
    case STATE_MIGRATION_BAD_ID:
        return "STATE_MIGRATION_BAD_ID";
    case STATE_MIGRATION_BAD_VERSION:
        return "STATE_MIGRATION_BAD_VERSION";
    case STATE_MIGRATION_BAD_LENGTH:
        return "STATE_MIGRATION_BAD_LENGTH";
    case STATE_MIGRATION_UNKNOWN:
        return "STATE_MIGRATION_UNKNOWN";
    case STATE_MIGRATION_UNSUPPORTED_VERSION:
        return "STATE_MIGRATION_UNSUPPORTED_VERSION";
    case STATE_MIGRATION_DUPLICATE:
        return "STATE_MIGRATION_DUPLICATE";
    case STATE_MIGRATION_OVERFLOW:
        return "STATE_MIGRATION_OVERFLOW";
    case STATE_MIGRATION_PUBLISH_FAILED:
        return "STATE_MIGRATION_PUBLISH_FAILED";
    case STATE_MIGRATION_WRITE_FAULT:
        return "STATE_MIGRATION_WRITE_FAULT";
    default:
        return "STATE_MIGRATION_UNKNOWN";
    }
}
