#include "candidate_slot_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void candidate_slot_inventory_zero_bytes(
    char *destination,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static void candidate_slot_inventory_copy_bytes(
    char *destination,
    const char *source,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool candidate_slot_inventory_generation_valid(uint32_t generation) {
    return generation >= CANDIDATE_SLOT_INVENTORY_GENERATION_MIN &&
           generation <= CANDIDATE_SLOT_INVENTORY_GENERATION_MAX;
}

static bool candidate_slot_inventory_slot_valid(uint32_t slot_id) {
    return slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX;
}

static bool candidate_slot_inventory_digest_valid(
    const uint8_t *digest)
{
    size_t index;

    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE; ++index) {
        if (digest[index] != 0U) {
            return true;
        }
    }
    return false;
}

enum candidate_slot_inventory_status candidate_slot_inventory_init(
    struct candidate_slot_inventory *inventory)
{
    if (inventory == NULL) {
        return CANDIDATE_SLOT_INVENTORY_NULL;
    }
    candidate_slot_inventory_zero_bytes(
        (char *)inventory, sizeof(*inventory));
    inventory->version = CANDIDATE_SLOT_INVENTORY_VERSION;
    return CANDIDATE_SLOT_INVENTORY_OK;
}

enum candidate_slot_inventory_status candidate_slot_inventory_publish(
    struct candidate_slot_inventory *inventory,
    const struct candidate_slot_inventory_entry *entries,
    size_t entry_count,
    uint32_t generation)
{
    struct candidate_slot_inventory draft;
    size_t index;
    size_t previous;

    if (inventory == NULL || entries == NULL) {
        return CANDIDATE_SLOT_INVENTORY_NULL;
    }
    if (entry_count == 0U) {
        return CANDIDATE_SLOT_INVENTORY_EMPTY;
    }
    if (entry_count > CANDIDATE_SLOT_INVENTORY_MAX_SLOTS) {
        return CANDIDATE_SLOT_INVENTORY_TOO_MANY;
    }
    if (!candidate_slot_inventory_generation_valid(generation)) {
        return CANDIDATE_SLOT_INVENTORY_BAD_GENERATION;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (!candidate_slot_inventory_slot_valid(entries[index].slot_id)) {
            return CANDIDATE_SLOT_INVENTORY_BAD_SLOT;
        }
        if (!candidate_slot_inventory_generation_valid(
                entries[index].generation)) {
            return CANDIDATE_SLOT_INVENTORY_BAD_GENERATION;
        }
        if (!candidate_slot_inventory_digest_valid(entries[index].digest)) {
            return CANDIDATE_SLOT_INVENTORY_BAD_DIGEST;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (entries[previous].slot_id == entries[index].slot_id) {
                return CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT;
            }
        }
    }

    if (inventory->active &&
        generation <= inventory->generation) {
        return CANDIDATE_SLOT_INVENTORY_STALE_GENERATION;
    }

    candidate_slot_inventory_zero_bytes((char *)&draft, sizeof(draft));
    draft.version = CANDIDATE_SLOT_INVENTORY_VERSION;
    draft.generation = generation;
    draft.slot_count = entry_count;
    for (index = 0U; index < entry_count; ++index) {
        draft.slots[index] = entries[index];
    }
    draft.active = true;
    candidate_slot_inventory_copy_bytes(
        (char *)inventory, (const char *)&draft, sizeof(draft));
    return CANDIDATE_SLOT_INVENTORY_OK;
}

enum candidate_slot_inventory_status candidate_slot_inventory_lookup(
    const struct candidate_slot_inventory *inventory,
    uint32_t slot_id,
    struct candidate_slot_inventory_entry *entry_out)
{
    size_t index;

    if (inventory == NULL || entry_out == NULL) {
        return CANDIDATE_SLOT_INVENTORY_NULL;
    }
    if (!inventory->active) {
        return CANDIDATE_SLOT_INVENTORY_NOT_FOUND;
    }
    for (index = 0U; index < inventory->slot_count; ++index) {
        if (inventory->slots[index].slot_id == slot_id) {
            *entry_out = inventory->slots[index];
            return CANDIDATE_SLOT_INVENTORY_OK;
        }
    }
    return CANDIDATE_SLOT_INVENTORY_NOT_FOUND;
}

uint32_t candidate_slot_inventory_generation(
    const struct candidate_slot_inventory *inventory)
{
    if (inventory == NULL || !inventory->active) {
        return 0U;
    }
    return inventory->generation;
}

size_t candidate_slot_inventory_slot_count(
    const struct candidate_slot_inventory *inventory)
{
    if (inventory == NULL || !inventory->active) {
        return 0U;
    }
    return inventory->slot_count;
}

bool candidate_slot_inventory_active(
    const struct candidate_slot_inventory *inventory)
{
    return inventory != NULL && inventory->active;
}

const char *candidate_slot_inventory_status_string(
    enum candidate_slot_inventory_status status)
{
    switch (status) {
    case CANDIDATE_SLOT_INVENTORY_OK:
        return "CANDIDATE_SLOT_INVENTORY_OK";
    case CANDIDATE_SLOT_INVENTORY_NULL:
        return "CANDIDATE_SLOT_INVENTORY_NULL";
    case CANDIDATE_SLOT_INVENTORY_BAD_STATE:
        return "CANDIDATE_SLOT_INVENTORY_BAD_STATE";
    case CANDIDATE_SLOT_INVENTORY_EMPTY:
        return "CANDIDATE_SLOT_INVENTORY_EMPTY";
    case CANDIDATE_SLOT_INVENTORY_TOO_MANY:
        return "CANDIDATE_SLOT_INVENTORY_TOO_MANY";
    case CANDIDATE_SLOT_INVENTORY_BAD_SLOT:
        return "CANDIDATE_SLOT_INVENTORY_BAD_SLOT";
    case CANDIDATE_SLOT_INVENTORY_BAD_GENERATION:
        return "CANDIDATE_SLOT_INVENTORY_BAD_GENERATION";
    case CANDIDATE_SLOT_INVENTORY_BAD_DIGEST:
        return "CANDIDATE_SLOT_INVENTORY_BAD_DIGEST";
    case CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT:
        return "CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT";
    case CANDIDATE_SLOT_INVENTORY_STALE_GENERATION:
        return "CANDIDATE_SLOT_INVENTORY_STALE_GENERATION";
    case CANDIDATE_SLOT_INVENTORY_NOT_FOUND:
        return "CANDIDATE_SLOT_INVENTORY_NOT_FOUND";
    default:
        return NULL;
    }
}
