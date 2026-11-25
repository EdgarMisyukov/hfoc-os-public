#ifndef HFOC_STORAGE_HEALTH_H
#define HFOC_STORAGE_HEALTH_H

#include "activation_intent.h"
#include "candidate_slot_inventory.h"
#include "health_check.h"
#include "journal_compaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORAGE_HEALTH_VERSION 1U

enum storage_health_status {
    STORAGE_HEALTH_OK = 0,
    STORAGE_HEALTH_NULL = 1,
    STORAGE_HEALTH_BAD_VERSION = 2,
    STORAGE_HEALTH_UNHEALTHY = 3,
};

enum storage_health_fault {
    STORAGE_HEALTH_FAULT_NONE = 0,
    STORAGE_HEALTH_FAULT_JOURNAL = 1,
    STORAGE_HEALTH_FAULT_CHECKPOINT = 2,
    STORAGE_HEALTH_FAULT_SLOT = 3,
    STORAGE_HEALTH_FAULT_INTENT = 4,
};

struct storage_health_input {
    uint32_t version;
    const struct journal_compaction_generation *journal;
    uint64_t checkpoint_sequence;
    const struct candidate_slot_inventory *inventory;
    const struct activation_intent_store *intent;
};

struct storage_health_verdict {
    uint32_t version;
    bool healthy;
    enum health_check_result_state result;
    enum storage_health_fault fault;
    uint64_t journal_generation_id;
    uint64_t checkpoint_sequence;
    uint32_t slot_generation;
    size_t slot_count;
    uint32_t known_good_slot_id;
};

enum storage_health_status storage_health_evaluate(
    const struct storage_health_input *input,
    struct storage_health_verdict *verdict_out);
const char *storage_health_status_string(
    enum storage_health_status status);
const char *storage_health_fault_string(
    enum storage_health_fault fault);

#endif
