#include "candidate_staged_write.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void candidate_staged_write_zero_bytes(char *destination,
                                              size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static void candidate_staged_write_copy_bytes(char *destination,
                                              const char *source,
                                              size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool candidate_staged_write_bytes_equal(const uint8_t *left,
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

static bool candidate_staged_write_digest_valid(const uint8_t *digest)
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        if (digest[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool candidate_staged_write_slot_valid(uint32_t slot_id)
{
    return slot_id >= CANDIDATE_SLOT_INVENTORY_SLOT_MIN &&
           slot_id <= CANDIDATE_SLOT_INVENTORY_SLOT_MAX;
}

enum candidate_staged_write_status candidate_staged_write_init(
    struct candidate_staged_write_gate *gate)
{
    if (gate == NULL) {
        return CANDIDATE_STAGED_WRITE_NULL;
    }
    if (gate->version == CANDIDATE_STAGED_WRITE_VERSION) {
        return CANDIDATE_STAGED_WRITE_BAD_STATE;
    }
    candidate_staged_write_zero_bytes((char *)gate, sizeof(*gate));
    gate->version = CANDIDATE_STAGED_WRITE_VERSION;
    return CANDIDATE_STAGED_WRITE_OK;
}

enum candidate_staged_write_status candidate_staged_write_run(
    struct candidate_staged_write_gate *gate,
    const struct candidate_preflight_gate *preflight,
    const struct candidate_slot_inventory *inventory,
    const void *data,
    const struct candidate_staged_write_io *io)
{
    struct candidate_slot_inventory_entry slot;
    struct candidate_staged_write_gate draft;
    struct sha256 context;
    uint8_t chunk[CANDIDATE_STAGED_WRITE_READBACK_CHUNK_SIZE];
    uint8_t readback_digest[SHA256_DIGEST_SIZE];
    enum candidate_slot_inventory_status inventory_status;
    size_t offset = 0U;

    if (gate == NULL || preflight == NULL || inventory == NULL ||
        data == NULL || io == NULL) {
        return CANDIDATE_STAGED_WRITE_NULL;
    }
    if (gate->version != CANDIDATE_STAGED_WRITE_VERSION ||
        gate->ready) {
        return CANDIDATE_STAGED_WRITE_BAD_STATE;
    }
    if (preflight->version != CANDIDATE_PREFLIGHT_VERSION ||
        !preflight->preflighted) {
        return CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED;
    }
    if (preflight->candidate.version != CANDIDATE_PREFLIGHT_VERSION) {
        return CANDIDATE_STAGED_WRITE_BAD_REQUEST;
    }
    if (preflight->candidate.bundle.size <
            CANDIDATE_PREFLIGHT_BUNDLE_MIN_SIZE ||
        preflight->candidate.bundle.size >
            CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE) {
        return CANDIDATE_STAGED_WRITE_BAD_REQUEST;
    }
    if (!candidate_staged_write_digest_valid(
            preflight->candidate.bundle.digest)) {
        return CANDIDATE_STAGED_WRITE_BAD_BUNDLE;
    }
    if (!candidate_staged_write_slot_valid(
            preflight->candidate.slot_id)) {
        return CANDIDATE_STAGED_WRITE_BAD_SLOT;
    }
    if (!candidate_slot_inventory_active(inventory)) {
        return CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE;
    }
    inventory_status = candidate_slot_inventory_lookup(
        inventory, preflight->candidate.slot_id, &slot);
    if (inventory_status != CANDIDATE_SLOT_INVENTORY_OK) {
        return CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE;
    }
    if (slot.generation != preflight->candidate.generation) {
        return CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH;
    }
    if (!candidate_staged_write_bytes_equal(
            slot.digest, preflight->candidate.bundle.digest,
            SHA256_DIGEST_SIZE)) {
        return CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH;
    }
    if (io->write == NULL || io->readback == NULL) {
        return CANDIDATE_STAGED_WRITE_BAD_REQUEST;
    }
    if (!io->write(io->context, slot.slot_id, data,
                   preflight->candidate.bundle.size)) {
        return CANDIDATE_STAGED_WRITE_WRITE_FAULT;
    }

    if (sha256_init(&context) != SHA256_OK) {
        return CANDIDATE_STAGED_WRITE_READBACK_FAULT;
    }
    while (offset < preflight->candidate.bundle.size) {
        size_t capacity =
            preflight->candidate.bundle.size - offset;
        size_t bytes_read = 0U;

        if (capacity > CANDIDATE_STAGED_WRITE_READBACK_CHUNK_SIZE) {
            capacity = CANDIDATE_STAGED_WRITE_READBACK_CHUNK_SIZE;
        }
        if (!io->readback(io->context, slot.slot_id, offset,
                          chunk, capacity, &bytes_read)) {
            return CANDIDATE_STAGED_WRITE_READBACK_FAULT;
        }
        if (bytes_read > capacity || bytes_read == 0U) {
            return CANDIDATE_STAGED_WRITE_READBACK_FAULT;
        }
        if (sha256_update(&context, chunk, bytes_read) != SHA256_OK) {
            return CANDIDATE_STAGED_WRITE_READBACK_FAULT;
        }
        offset += bytes_read;
    }
    if (sha256_final(&context, readback_digest) != SHA256_OK) {
        return CANDIDATE_STAGED_WRITE_READBACK_FAULT;
    }
    if (!candidate_staged_write_bytes_equal(
            readback_digest, preflight->candidate.bundle.digest,
            SHA256_DIGEST_SIZE)) {
        return CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH;
    }

    candidate_staged_write_zero_bytes((char *)&draft, sizeof(draft));
    draft.version = CANDIDATE_STAGED_WRITE_VERSION;
    draft.slot_id = slot.slot_id;
    draft.generation = slot.generation;
    candidate_staged_write_copy_bytes(
        (char *)draft.digest,
        (const char *)slot.digest, SHA256_DIGEST_SIZE);
    draft.bundle_size = preflight->candidate.bundle.size;
    draft.ready = true;
    candidate_staged_write_copy_bytes(
        (char *)gate, (const char *)&draft, sizeof(draft));
    return CANDIDATE_STAGED_WRITE_OK;
}

const char *candidate_staged_write_status_string(
    enum candidate_staged_write_status status)
{
    switch (status) {
    case CANDIDATE_STAGED_WRITE_OK:
        return "CANDIDATE_STAGED_WRITE_OK";
    case CANDIDATE_STAGED_WRITE_NULL:
        return "CANDIDATE_STAGED_WRITE_NULL";
    case CANDIDATE_STAGED_WRITE_BAD_STATE:
        return "CANDIDATE_STAGED_WRITE_BAD_STATE";
    case CANDIDATE_STAGED_WRITE_BAD_REQUEST:
        return "CANDIDATE_STAGED_WRITE_BAD_REQUEST";
    case CANDIDATE_STAGED_WRITE_BAD_BUNDLE:
        return "CANDIDATE_STAGED_WRITE_BAD_BUNDLE";
    case CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED:
        return "CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED";
    case CANDIDATE_STAGED_WRITE_BAD_SLOT:
        return "CANDIDATE_STAGED_WRITE_BAD_SLOT";
    case CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE:
        return "CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE";
    case CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH:
        return "CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH";
    case CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH:
        return "CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH";
    case CANDIDATE_STAGED_WRITE_WRITE_FAULT:
        return "CANDIDATE_STAGED_WRITE_WRITE_FAULT";
    case CANDIDATE_STAGED_WRITE_READBACK_FAULT:
        return "CANDIDATE_STAGED_WRITE_READBACK_FAULT";
    default:
        return NULL;
    }
}
