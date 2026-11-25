#include "packet_slice.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool packet_slice_ranges_overlap(
    uintptr_t left,
    size_t left_size,
    uintptr_t right,
    size_t right_size)
{
    if (left_size == 0U || right_size == 0U) {
        return false;
    }
    if (left < right) {
        return right - left < left_size;
    }
    return left - right < right_size;
}

static bool packet_slice_overlaps_storage(
    const struct packet_slice *slice,
    const void *buffer,
    size_t size)
{
    uintptr_t slot_base;
    size_t slot_size;

    slot_base = (uintptr_t)slice->pool->storage +
        (uintptr_t)slice->slot.slot *
            ((uintptr_t)slice->pool->headroom +
             (uintptr_t)slice->pool->capacity);
    slot_size = (size_t)slice->pool->headroom +
        (size_t)slice->pool->capacity;
    return packet_slice_ranges_overlap(
        (uintptr_t)buffer, size, slot_base, slot_size);
}

static enum packet_slice_status packet_slice_validate(
    const struct packet_slice *slice)
{
    struct packet_pool_slot_descriptor current;
    uint64_t end;

    if (slice == NULL) {
        return PACKET_SLICE_NULL;
    }
    if (slice->version != PACKET_SLICE_VERSION ||
        slice->pool == NULL) {
        return PACKET_SLICE_BAD_STATE;
    }
    if (!packet_pool_is_published(slice->pool)) {
        return PACKET_SLICE_BAD_POOL;
    }
    if (slice->slot.slot >= packet_pool_count(slice->pool)) {
        return PACKET_SLICE_BAD_STATE;
    }
    if (packet_pool_slot(
            slice->pool, slice->slot.slot, &current) !=
        PACKET_POOL_OK) {
        return PACKET_SLICE_BAD_STATE;
    }
    if (current.generation != slice->slot.generation) {
        return PACKET_SLICE_STALE_DESCRIPTOR;
    }
    if (current.owner != PACKET_POOL_OWNER_ACTIVE ||
        slice->slot.owner != PACKET_POOL_OWNER_ACTIVE ||
        current.direction != slice->slot.direction ||
        slice->slot.direction == PACKET_POOL_DIRECTION_NONE) {
        return PACKET_SLICE_WRONG_OWNER;
    }

    end = (uint64_t)slice->start + (uint64_t)slice->length;
    if (slice->start >
            (uint64_t)slice->pool->headroom +
                (uint64_t)slice->pool->capacity ||
        slice->length > slice->pool->length ||
        slice->length > slice->pool->capacity ||
        end > (uint64_t)slice->pool->headroom +
                  (uint64_t)slice->pool->capacity) {
        return PACKET_SLICE_BAD_RANGE;
    }
    if (slice->cursor > slice->length) {
        return PACKET_SLICE_BAD_CURSOR;
    }
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_init(
    struct packet_slice *slice,
    const struct packet_pool_descriptor *pool,
    const struct packet_pool_slot_descriptor *slot,
    uint32_t length)
{
    struct packet_pool_slot_descriptor current;
    uint64_t end;

    if (slice == NULL || pool == NULL || slot == NULL) {
        return PACKET_SLICE_NULL;
    }
    if (!packet_pool_is_published(pool)) {
        return PACKET_SLICE_BAD_POOL;
    }
    if (slot->slot >= packet_pool_count(pool)) {
        return PACKET_SLICE_BAD_STATE;
    }
    if (packet_pool_slot(pool, slot->slot, &current) !=
        PACKET_POOL_OK) {
        return PACKET_SLICE_BAD_STATE;
    }
    if (current.generation != slot->generation) {
        return PACKET_SLICE_STALE_DESCRIPTOR;
    }
    if (current.owner != PACKET_POOL_OWNER_ACTIVE ||
        slot->owner != PACKET_POOL_OWNER_ACTIVE ||
        current.direction != slot->direction ||
        slot->direction == PACKET_POOL_DIRECTION_NONE) {
        return PACKET_SLICE_WRONG_OWNER;
    }
    if (length > pool->length || length > pool->capacity) {
        return PACKET_SLICE_BAD_LENGTH;
    }

    end = (uint64_t)pool->headroom + (uint64_t)length;
    if (end > (uint64_t)pool->headroom +
                  (uint64_t)pool->capacity) {
        return PACKET_SLICE_BAD_LENGTH;
    }

    memset(slice, 0, sizeof(*slice));
    slice->version = PACKET_SLICE_VERSION;
    slice->pool = pool;
    slice->slot = *slot;
    slice->start = pool->headroom;
    slice->length = length;
    slice->cursor = 0U;
    return PACKET_SLICE_OK;
}

bool packet_slice_is_valid(const struct packet_slice *slice)
{
    return packet_slice_validate(slice) == PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_prepend(
    struct packet_slice *slice,
    const void *data,
    size_t size)
{
    enum packet_slice_status status;
    uint8_t *target;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (data == NULL && size != 0U) {
        return PACKET_SLICE_NULL;
    }
    if (size == 0U) {
        return PACKET_SLICE_OK;
    }
    if (size > slice->pool->capacity ||
        size > slice->pool->length) {
        return PACKET_SLICE_OVERFLOW;
    }
    if ((uint64_t)size > slice->start) {
        return PACKET_SLICE_OVERFLOW;
    }
    if ((uint64_t)slice->length + (uint64_t)size >
        slice->pool->length) {
        return PACKET_SLICE_OVERFLOW;
    }
    if (packet_slice_overlaps_storage(
            slice, data, size)) {
        return PACKET_SLICE_OVERLAP;
    }

    target = slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start - size;
    memcpy(target, data, size);
    slice->start -= (uint32_t)size;
    slice->length += (uint32_t)size;
    if (slice->cursor != 0U) {
        slice->cursor += (uint32_t)size;
    }
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_append(
    struct packet_slice *slice,
    const void *data,
    size_t size)
{
    enum packet_slice_status status;
    uint64_t end;
    uint8_t *target;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (data == NULL && size != 0U) {
        return PACKET_SLICE_NULL;
    }
    if (size == 0U) {
        return PACKET_SLICE_OK;
    }
    if (size > slice->pool->capacity ||
        size > slice->pool->length) {
        return PACKET_SLICE_OVERFLOW;
    }
    if ((uint64_t)slice->length + (uint64_t)size >
        slice->pool->length) {
        return PACKET_SLICE_OVERFLOW;
    }

    end = (uint64_t)slice->start + (uint64_t)slice->length +
        (uint64_t)size;
    if (end > (uint64_t)slice->pool->headroom +
                  (uint64_t)slice->pool->capacity) {
        return PACKET_SLICE_OVERFLOW;
    }
    if (packet_slice_overlaps_storage(
            slice, data, size)) {
        return PACKET_SLICE_OVERLAP;
    }

    target = slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start + (size_t)slice->length;
    memcpy(target, data, size);
    slice->length += (uint32_t)size;
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_trim(
    struct packet_slice *slice,
    uint32_t amount)
{
    enum packet_slice_status status;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (amount > slice->length) {
        return PACKET_SLICE_UNDERFLOW;
    }
    if (amount == 0U) {
        return PACKET_SLICE_OK;
    }

    slice->length -= amount;
    if (slice->cursor > slice->length) {
        slice->cursor = slice->length;
    }
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_read(
    struct packet_slice *slice,
    void *destination,
    size_t size)
{
    enum packet_slice_status status;
    uint8_t *source;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (destination == NULL && size != 0U) {
        return PACKET_SLICE_NULL;
    }
    if (size == 0U) {
        return PACKET_SLICE_OK;
    }
    if (size > slice->pool->capacity ||
        (uint64_t)slice->cursor + (uint64_t)size >
            slice->length) {
        return PACKET_SLICE_UNDERFLOW;
    }
    if (packet_slice_overlaps_storage(
            slice, destination, size)) {
        return PACKET_SLICE_OVERLAP;
    }

    source = slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start + (size_t)slice->cursor;
    memcpy(destination, source, size);
    slice->cursor += (uint32_t)size;
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_write(
    struct packet_slice *slice,
    const void *data,
    size_t size)
{
    enum packet_slice_status status;
    uint8_t *target;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (data == NULL && size != 0U) {
        return PACKET_SLICE_NULL;
    }
    if (size == 0U) {
        return PACKET_SLICE_OK;
    }
    if (size > slice->pool->capacity ||
        (uint64_t)slice->cursor + (uint64_t)size >
            slice->length) {
        return PACKET_SLICE_UNDERFLOW;
    }
    if (packet_slice_overlaps_storage(
            slice, data, size)) {
        return PACKET_SLICE_OVERLAP;
    }

    target = slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start + (size_t)slice->cursor;
    memcpy(target, data, size);
    slice->cursor += (uint32_t)size;
    return PACKET_SLICE_OK;
}

enum packet_slice_status packet_slice_seek(
    struct packet_slice *slice,
    uint32_t offset)
{
    enum packet_slice_status status;

    status = packet_slice_validate(slice);
    if (status != PACKET_SLICE_OK) {
        return status;
    }
    if (offset > slice->length) {
        return PACKET_SLICE_BAD_CURSOR;
    }
    slice->cursor = offset;
    return PACKET_SLICE_OK;
}

uint32_t packet_slice_start(const struct packet_slice *slice)
{
    return packet_slice_is_valid(slice) ? slice->start : 0U;
}

uint32_t packet_slice_length(const struct packet_slice *slice)
{
    return packet_slice_is_valid(slice) ? slice->length : 0U;
}

uint32_t packet_slice_cursor(const struct packet_slice *slice)
{
    return packet_slice_is_valid(slice) ? slice->cursor : 0U;
}

const char *packet_slice_status_string(
    enum packet_slice_status status)
{
    switch (status) {
    case PACKET_SLICE_OK:
        return "PACKET_SLICE_OK";
    case PACKET_SLICE_NULL:
        return "PACKET_SLICE_NULL";
    case PACKET_SLICE_BAD_STATE:
        return "PACKET_SLICE_BAD_STATE";
    case PACKET_SLICE_BAD_POOL:
        return "PACKET_SLICE_BAD_POOL";
    case PACKET_SLICE_STALE_DESCRIPTOR:
        return "PACKET_SLICE_STALE_DESCRIPTOR";
    case PACKET_SLICE_WRONG_OWNER:
        return "PACKET_SLICE_WRONG_OWNER";
    case PACKET_SLICE_BAD_LENGTH:
        return "PACKET_SLICE_BAD_LENGTH";
    case PACKET_SLICE_BAD_RANGE:
        return "PACKET_SLICE_BAD_RANGE";
    case PACKET_SLICE_BAD_CURSOR:
        return "PACKET_SLICE_BAD_CURSOR";
    case PACKET_SLICE_UNDERFLOW:
        return "PACKET_SLICE_UNDERFLOW";
    case PACKET_SLICE_OVERFLOW:
        return "PACKET_SLICE_OVERFLOW";
    case PACKET_SLICE_OVERLAP:
        return "PACKET_SLICE_OVERLAP";
    default:
        return NULL;
    }
}
