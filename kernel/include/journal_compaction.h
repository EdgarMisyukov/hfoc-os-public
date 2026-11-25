#ifndef HFOC_JOURNAL_COMPACTION_H
#define HFOC_JOURNAL_COMPACTION_H

#include "config_kv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JOURNAL_COMPACTION_CHECKPOINT_VERSION 1U
#define JOURNAL_COMPACTION_MAX_CHECKPOINTS 32U
#define JOURNAL_COMPACTION_MAX_ENTRIES CONFIG_KV_CAPACITY_MAX
#define JOURNAL_COMPACTION_GENERATION_SLOT_COUNT 2U
#define JOURNAL_COMPACTION_INITIAL_GENERATION 1U

enum journal_compaction_status {
    JOURNAL_COMPACTION_OK = 0,
    JOURNAL_COMPACTION_NULL = 1,
    JOURNAL_COMPACTION_BAD_STATE = 2,
    JOURNAL_COMPACTION_BAD_CAPACITY = 3,
    JOURNAL_COMPACTION_BAD_VERSION = 4,
    JOURNAL_COMPACTION_BAD_SEQUENCE = 5,
    JOURNAL_COMPACTION_DUPLICATE = 6,
    JOURNAL_COMPACTION_BAD_GENERATION = 7,
    JOURNAL_COMPACTION_BAD_SOURCE = 8,
    JOURNAL_COMPACTION_WRITE_FAULT = 9,
};

struct journal_compaction_checkpoint {
    uint8_t version;
    uint64_t sequence;
    const struct config_kv *kv;
};

struct journal_compaction_candidate {
    struct config_kv_entry entries[JOURNAL_COMPACTION_MAX_ENTRIES];
    size_t count;
    uint32_t config_count;
    uint32_t audit_count;
    uint64_t source_sequence;
    bool valid;
};

struct journal_compaction_generation {
    uint64_t generation_id;
    uint64_t source_sequence;
    struct config_kv_entry entries[JOURNAL_COMPACTION_MAX_ENTRIES];
    size_t count;
    uint32_t config_count;
    uint32_t audit_count;
    bool valid;
    bool active;
    bool staged;
    bool interrupted;
};

struct journal_compaction_commit {
    struct journal_compaction_generation
        slots[JOURNAL_COMPACTION_GENERATION_SLOT_COUNT];
    uint8_t active_slot;
    uint64_t next_generation;
    bool initialized;
};

struct journal_compaction_io;

typedef bool (*journal_compaction_write_fn)(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct journal_compaction_generation *generation);

struct journal_compaction_io {
    void *context;
    journal_compaction_write_fn write;
};

enum journal_compaction_status journal_compaction_select(
    struct journal_compaction_candidate *candidate,
    const struct journal_compaction_checkpoint *checkpoints,
    size_t count);
enum journal_compaction_status journal_compaction_commit_init(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_candidate *initial);
enum journal_compaction_status journal_compaction_publish(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_candidate *candidate,
    const struct journal_compaction_io *io);
enum journal_compaction_status journal_compaction_recover(
    struct journal_compaction_commit *commit,
    const struct journal_compaction_io *io);
const struct journal_compaction_generation *journal_compaction_active(
    const struct journal_compaction_commit *commit);
const char *journal_compaction_status_string(
    enum journal_compaction_status status);

#endif
