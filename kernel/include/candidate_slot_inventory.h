#ifndef HFOC_CANDIDATE_SLOT_INVENTORY_H
#define HFOC_CANDIDATE_SLOT_INVENTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CANDIDATE_SLOT_INVENTORY_VERSION 1U
#define CANDIDATE_SLOT_INVENTORY_MAX_SLOTS 8U
#define CANDIDATE_SLOT_INVENTORY_SLOT_MIN 1U
#define CANDIDATE_SLOT_INVENTORY_SLOT_MAX 8U
#define CANDIDATE_SLOT_INVENTORY_GENERATION_MIN 1U
#define CANDIDATE_SLOT_INVENTORY_GENERATION_MAX 0xFFFFFFFFU
#define CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE 32U

enum candidate_slot_inventory_status {
    CANDIDATE_SLOT_INVENTORY_OK = 0,
    CANDIDATE_SLOT_INVENTORY_NULL = 1,
    CANDIDATE_SLOT_INVENTORY_BAD_STATE = 2,
    CANDIDATE_SLOT_INVENTORY_EMPTY = 3,
    CANDIDATE_SLOT_INVENTORY_TOO_MANY = 4,
    CANDIDATE_SLOT_INVENTORY_BAD_SLOT = 5,
    CANDIDATE_SLOT_INVENTORY_BAD_GENERATION = 6,
    CANDIDATE_SLOT_INVENTORY_BAD_DIGEST = 7,
    CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT = 8,
    CANDIDATE_SLOT_INVENTORY_STALE_GENERATION = 9,
    CANDIDATE_SLOT_INVENTORY_NOT_FOUND = 10,
};

struct candidate_slot_inventory_entry {
    uint32_t slot_id;
    uint32_t generation;
    uint8_t digest[CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE];
};

struct candidate_slot_inventory {
    uint32_t version;
    uint32_t generation;
    size_t slot_count;
    struct candidate_slot_inventory_entry
        slots[CANDIDATE_SLOT_INVENTORY_MAX_SLOTS];
    bool active;
};

enum candidate_slot_inventory_status candidate_slot_inventory_init(
    struct candidate_slot_inventory *inventory);
enum candidate_slot_inventory_status candidate_slot_inventory_publish(
    struct candidate_slot_inventory *inventory,
    const struct candidate_slot_inventory_entry *entries,
    size_t entry_count,
    uint32_t generation);
enum candidate_slot_inventory_status candidate_slot_inventory_lookup(
    const struct candidate_slot_inventory *inventory,
    uint32_t slot_id,
    struct candidate_slot_inventory_entry *entry_out);
uint32_t candidate_slot_inventory_generation(
    const struct candidate_slot_inventory *inventory);
size_t candidate_slot_inventory_slot_count(
    const struct candidate_slot_inventory *inventory);
bool candidate_slot_inventory_active(
    const struct candidate_slot_inventory *inventory);
const char *candidate_slot_inventory_status_string(
    enum candidate_slot_inventory_status status);

#endif
