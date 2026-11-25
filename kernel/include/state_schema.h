#ifndef HFOC_STATE_SCHEMA_H
#define HFOC_STATE_SCHEMA_H

#include "config_journal.h"
#include "config_kv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATE_SCHEMA_ID_MAX 16U
#define STATE_SCHEMA_REGISTRY_CAPACITY_MAX 32U
#define STATE_SCHEMA_RECORD_LENGTH_MAX 1024U

#define STATE_SCHEMA_KIND_JOURNAL 0U
#define STATE_SCHEMA_KIND_KV 1U
#define STATE_SCHEMA_KIND_COUNT 2U

#define STATE_SCHEMA_TARGET_JOURNAL 0U

enum state_schema_status {
    STATE_SCHEMA_OK = 0,
    STATE_SCHEMA_NULL = 1,
    STATE_SCHEMA_BAD_KIND = 2,
    STATE_SCHEMA_BAD_ID = 3,
    STATE_SCHEMA_BAD_VERSION = 4,
    STATE_SCHEMA_BAD_LENGTH = 5,
    STATE_SCHEMA_BAD_CAPACITY = 6,
    STATE_SCHEMA_DUPLICATE = 7,
    STATE_SCHEMA_BAD_STATE = 8,
    STATE_SCHEMA_UNKNOWN = 9,
    STATE_SCHEMA_UNSUPPORTED_VERSION = 10,
    STATE_SCHEMA_RECORD_TOO_SHORT = 11,
    STATE_SCHEMA_RECORD_TOO_LONG = 12,
    STATE_SCHEMA_PUBLISH_FAILED = 13,
};

struct state_schema_descriptor {
    uint8_t kind;
    uint8_t target_kind;
    uint8_t id_length;
    unsigned char id[STATE_SCHEMA_ID_MAX];
    uint32_t version;
    uint32_t minimum_version;
    uint32_t maximum_version;
    uint16_t minimum_length;
    uint16_t maximum_length;
};

struct state_schema_registry {
    struct state_schema_descriptor descriptors[
        STATE_SCHEMA_REGISTRY_CAPACITY_MAX];
    size_t count;
    bool initialized;
};

enum state_schema_status state_schema_registry_init(
    struct state_schema_registry *registry,
    const struct state_schema_descriptor *descriptors,
    size_t count);
enum state_schema_status state_schema_validate_record(
    const struct state_schema_registry *registry,
    uint8_t kind,
    uint8_t target_kind,
    const void *id,
    size_t id_length,
    uint32_t version,
    size_t record_length);
enum state_schema_status state_schema_append_journal(
    struct config_journal *journal,
    const struct state_schema_registry *registry,
    const void *id,
    size_t id_length,
    uint32_t version,
    const void *data,
    size_t data_length);
enum state_schema_status state_schema_kv_put(
    struct config_kv *kv,
    const struct state_schema_registry *registry,
    const void *id,
    size_t id_length,
    uint32_t version,
    uint8_t kind,
    const void *key,
    size_t key_length,
    const void *value,
    size_t value_length,
    bool *replaced);
const char *state_schema_status_string(enum state_schema_status status);

#endif
