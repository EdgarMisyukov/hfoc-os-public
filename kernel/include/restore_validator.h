#ifndef HFOC_RESTORE_VALIDATOR_H
#define HFOC_RESTORE_VALIDATOR_H

#include "backup_stream.h"
#include "config_kv.h"

#include <stdbool.h>
#include <stddef.h>

#define RESTORE_PLAN_MAX_ENTRIES CONFIG_KV_CAPACITY_MAX
#define RESTORE_STAGE_SLOT_COUNT 2U

enum restore_status {
    RESTORE_OK = 0,
    RESTORE_NULL = 1,
    RESTORE_BAD_STATE = 2,
    RESTORE_CORRUPT = 3,
    RESTORE_OVERFLOW = 4,
    RESTORE_NO_CHANGE = 5,
    RESTORE_WRITE_FAULT = 6,
    RESTORE_DRY_RUN = 7,
    RESTORE_BAD_STAGE = 8,
};

struct restore_plan {
    struct config_kv_entry entries[RESTORE_PLAN_MAX_ENTRIES];
    size_t count;
    struct backup_stream_counts counts;
    bool valid;
    bool dry_run;
};

typedef bool (*restore_write_fn)(void *context, size_t write_index);

struct restore_io {
    void *context;
    restore_write_fn write;
    struct config_kv_entry *rollback;
    size_t rollback_capacity;
};

typedef bool (*restore_stage_write_fn)(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct restore_plan *plan);

struct restore_stage_io {
    void *context;
    restore_stage_write_fn write;
};

struct restore_activation {
    struct config_kv slots[RESTORE_STAGE_SLOT_COUNT];
    uint8_t active_slot;
    bool initialized;
};

enum restore_status restore_validate(
    const void *stream,
    size_t length,
    struct restore_plan *plan);
enum restore_status restore_dry_run_plan(
    const void *stream,
    size_t length,
    struct restore_plan *plan);
enum restore_status restore_commit(
    const struct restore_plan *plan,
    struct config_kv *active,
    const struct restore_io *io);
enum restore_status restore_apply(
    const void *stream,
    size_t length,
    struct config_kv *active,
    const struct restore_io *io);
enum restore_status restore_activation_init(
    struct restore_activation *activation,
    struct config_kv_entry *active_entries,
    struct config_kv_entry *staged_entries,
    size_t capacity);
enum restore_status restore_stage_apply(
    struct restore_activation *activation,
    const struct restore_plan *plan,
    const struct restore_stage_io *io);
const struct config_kv *restore_activation_active(
    const struct restore_activation *activation);
const char *restore_status_string(enum restore_status status);

#endif
