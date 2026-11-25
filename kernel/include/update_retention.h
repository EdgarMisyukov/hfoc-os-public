#ifndef HFOC_UPDATE_RETENTION_H
#define HFOC_UPDATE_RETENTION_H

#include "activation_intent.h"
#include "candidate_slot_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_RETENTION_VERSION 1U
#define UPDATE_RETENTION_MAX_RETAIN \
    (CANDIDATE_SLOT_INVENTORY_MAX_SLOTS - 1U)
#define UPDATE_RETENTION_MAX_RECLAIM CANDIDATE_SLOT_INVENTORY_MAX_SLOTS
#define UPDATE_RETENTION_POLICY_KEEP_MIN 0U
#define UPDATE_RETENTION_POLICY_KEEP_MAX UPDATE_RETENTION_MAX_RETAIN
#define UPDATE_RETENTION_DEFAULT_MAX_KEPT 4U

enum update_retention_status {
    UPDATE_RETENTION_OK = 0,
    UPDATE_RETENTION_NULL = 1,
    UPDATE_RETENTION_BAD_STATE = 2,
    UPDATE_RETENTION_BAD_POLICY = 3,
    UPDATE_RETENTION_INACTIVE_INVENTORY = 4,
    UPDATE_RETENTION_INVALID_INVENTORY = 5,
    UPDATE_RETENTION_BAD_INTENT = 6,
    UPDATE_RETENTION_ALREADY_PUBLISHED = 7,
    UPDATE_RETENTION_NO_EVIDENCE = 8,
};

struct update_retention_policy {
    uint32_t version;
    uint32_t max_unreferenced_kept;
};

struct update_retention_evidence {
    uint32_t version;
    uint32_t policy_max_unreferenced_kept;
    struct candidate_slot_inventory_entry protected_active;
    struct candidate_slot_inventory_entry protected_rollback;
    struct candidate_slot_inventory_entry protected_pending;
    struct candidate_slot_inventory_entry
        retained[UPDATE_RETENTION_MAX_RETAIN];
    size_t retained_count;
    struct candidate_slot_inventory_entry
        reclaim[UPDATE_RETENTION_MAX_RECLAIM];
    size_t reclaim_count;
};

struct update_retention_store {
    uint32_t version;
    struct update_retention_evidence evidence;
    bool initialized;
    bool published;
};

enum update_retention_status update_retention_init(
    struct update_retention_store *store);
enum update_retention_status update_retention_run(
    struct update_retention_store *store,
    const struct candidate_slot_inventory *inventory,
    const struct activation_intent_store *intent,
    const struct update_retention_policy *policy);
enum update_retention_status update_retention_evidence(
    const struct update_retention_store *store,
    struct update_retention_evidence *evidence_out);
const char *update_retention_status_string(
    enum update_retention_status status);

#endif
