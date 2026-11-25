#ifndef HFOC_STATE_MIGRATION_H
#define HFOC_STATE_MIGRATION_H

#include "state_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATE_MIGRATION_MAX_STEPS 32U

enum state_migration_status {
    STATE_MIGRATION_OK = 0,
    STATE_MIGRATION_NULL = 1,
    STATE_MIGRATION_BAD_STATE = 2,
    STATE_MIGRATION_BAD_PLAN = 3,
    STATE_MIGRATION_BAD_CAPACITY = 4,
    STATE_MIGRATION_BAD_KIND = 5,
    STATE_MIGRATION_BAD_TARGET = 6,
    STATE_MIGRATION_BAD_ID = 7,
    STATE_MIGRATION_BAD_VERSION = 8,
    STATE_MIGRATION_BAD_LENGTH = 9,
    STATE_MIGRATION_UNKNOWN = 10,
    STATE_MIGRATION_UNSUPPORTED_VERSION = 11,
    STATE_MIGRATION_DUPLICATE = 12,
    STATE_MIGRATION_OVERFLOW = 13,
    STATE_MIGRATION_PUBLISH_FAILED = 14,
    STATE_MIGRATION_WRITE_FAULT = 15,
};

struct state_migration_step {
    uint8_t kind;
    uint8_t target_kind;
    uint8_t id_length;
    unsigned char id[STATE_SCHEMA_ID_MAX];
    uint32_t from_version;
    uint32_t to_version;
    uint8_t key_length;
    unsigned char key[CONFIG_KV_KEY_MAX];
    uint16_t value_length;
    unsigned char value[CONFIG_KV_VALUE_MAX];
};

struct state_migration_plan {
    struct state_migration_step steps[STATE_MIGRATION_MAX_STEPS];
    size_t count;
    bool valid;
};

struct state_migration_io {
    void *context;
    bool (*write)(void *context, size_t step_index);
    struct config_kv_entry *rollback;
    size_t rollback_capacity;
};

enum state_migration_status state_migration_plan_init(
    struct state_migration_plan *plan,
    const struct state_schema_registry *registry,
    const struct state_migration_step *steps,
    size_t count);
enum state_migration_status state_migration_commit(
    struct config_kv *active,
    const struct state_schema_registry *registry,
    const struct state_migration_plan *plan,
    const struct state_migration_io *io);
enum state_migration_status state_migration_apply(
    struct config_kv *active,
    const struct state_schema_registry *registry,
    const struct state_migration_step *steps,
    size_t count,
    const struct state_migration_io *io);
const char *state_migration_status_string(
    enum state_migration_status status);

#endif
