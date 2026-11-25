#include "candidate_preflight.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void candidate_preflight_zero_bytes(char *destination,
                                           size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static void candidate_preflight_copy_bytes(char *destination,
                                           const char *source,
                                           size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool candidate_preflight_bytes_equal(const uint8_t *left,
                                            const uint8_t *right,
                                            size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool candidate_preflight_digest_valid(const uint8_t *digest)
{
    size_t index;

    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE; ++index) {
        if (digest[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool candidate_preflight_slot_valid(uint32_t slot_id)
{
    return slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX;
}

enum candidate_preflight_status candidate_preflight_init(
    struct candidate_preflight_gate *gate)
{
    if (gate == NULL) {
        return CANDIDATE_PREFLIGHT_NULL;
    }
    if (gate->version == CANDIDATE_PREFLIGHT_VERSION) {
        return CANDIDATE_PREFLIGHT_BAD_STATE;
    }
    candidate_preflight_zero_bytes((char *)gate, sizeof(*gate));
    gate->version = CANDIDATE_PREFLIGHT_VERSION;
    return CANDIDATE_PREFLIGHT_OK;
}

enum candidate_preflight_status candidate_preflight_run(
    struct candidate_preflight_gate *gate,
    const struct candidate_preflight_request *request,
    const struct candidate_slot_inventory *inventory)
{
    struct candidate_slot_inventory_entry slot;
    struct candidate_preflight_gate draft;
    enum update_compatibility_status compatibility_status;
    enum candidate_slot_inventory_status inventory_status;

    if (gate == NULL || request == NULL || inventory == NULL) {
        return CANDIDATE_PREFLIGHT_NULL;
    }
    if (gate->version != CANDIDATE_PREFLIGHT_VERSION ||
        gate->preflighted) {
        return CANDIDATE_PREFLIGHT_BAD_STATE;
    }
    if (request->version != CANDIDATE_PREFLIGHT_VERSION) {
        return CANDIDATE_PREFLIGHT_BAD_REQUEST;
    }
    if (request->bundle.size < CANDIDATE_PREFLIGHT_BUNDLE_MIN_SIZE ||
        request->bundle.size > CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE) {
        return CANDIDATE_PREFLIGHT_BAD_SIZE;
    }
    if (!candidate_preflight_digest_valid(request->bundle.digest)) {
        return CANDIDATE_PREFLIGHT_BAD_BUNDLE;
    }
    if (request->trust_status != SIGNED_METADATA_OK) {
        return CANDIDATE_PREFLIGHT_TRUST_DENIED;
    }

    compatibility_status = update_compatibility_check(
        &request->compatibility, &request->source, &request->target);
    if (compatibility_status != UPDATE_COMPATIBILITY_OK) {
        return CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED;
    }
    if (!candidate_preflight_slot_valid(request->slot_id)) {
        return CANDIDATE_PREFLIGHT_BAD_SLOT;
    }
    if (!candidate_slot_inventory_active(inventory)) {
        return CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE;
    }
    inventory_status = candidate_slot_inventory_lookup(
        inventory, request->slot_id, &slot);
    if (inventory_status != CANDIDATE_SLOT_INVENTORY_OK) {
        return CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE;
    }
    if (slot.generation != request->generation) {
        return CANDIDATE_PREFLIGHT_GENERATION_MISMATCH;
    }
    if (!candidate_preflight_bytes_equal(
            slot.digest, request->bundle.digest,
            CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE)) {
        return CANDIDATE_PREFLIGHT_DIGEST_MISMATCH;
    }

    candidate_preflight_zero_bytes((char *)&draft, sizeof(draft));
    draft.version = CANDIDATE_PREFLIGHT_VERSION;
    draft.candidate = *request;
    draft.preflighted = true;
    candidate_preflight_copy_bytes(
        (char *)gate, (const char *)&draft, sizeof(draft));
    return CANDIDATE_PREFLIGHT_OK;
}

const char *candidate_preflight_status_string(
    enum candidate_preflight_status status)
{
    switch (status) {
    case CANDIDATE_PREFLIGHT_OK:
        return "CANDIDATE_PREFLIGHT_OK";
    case CANDIDATE_PREFLIGHT_NULL:
        return "CANDIDATE_PREFLIGHT_NULL";
    case CANDIDATE_PREFLIGHT_BAD_STATE:
        return "CANDIDATE_PREFLIGHT_BAD_STATE";
    case CANDIDATE_PREFLIGHT_BAD_REQUEST:
        return "CANDIDATE_PREFLIGHT_BAD_REQUEST";
    case CANDIDATE_PREFLIGHT_BAD_BUNDLE:
        return "CANDIDATE_PREFLIGHT_BAD_BUNDLE";
    case CANDIDATE_PREFLIGHT_BAD_SIZE:
        return "CANDIDATE_PREFLIGHT_BAD_SIZE";
    case CANDIDATE_PREFLIGHT_TRUST_DENIED:
        return "CANDIDATE_PREFLIGHT_TRUST_DENIED";
    case CANDIDATE_PREFLIGHT_BAD_SLOT:
        return "CANDIDATE_PREFLIGHT_BAD_SLOT";
    case CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE:
        return "CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE";
    case CANDIDATE_PREFLIGHT_GENERATION_MISMATCH:
        return "CANDIDATE_PREFLIGHT_GENERATION_MISMATCH";
    case CANDIDATE_PREFLIGHT_DIGEST_MISMATCH:
        return "CANDIDATE_PREFLIGHT_DIGEST_MISMATCH";
    case CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED:
        return "CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED";
    default:
        return NULL;
    }
}
