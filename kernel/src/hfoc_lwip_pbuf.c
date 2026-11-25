#include "hfoc_lwip_pbuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool hfoc_lwip_pbuf_bridge_valid(
    const struct hfoc_lwip_pbuf_bridge *bridge)
{
    return bridge != NULL &&
           bridge->published &&
           bridge->version == HFOC_LWIP_PBUF_VERSION &&
           bridge->generation != 0U &&
           bridge->pool != NULL &&
           packet_pool_is_published(bridge->pool) &&
           bridge->used_count <= HFOC_LWIP_PBUF_SLOT_COUNT;
}

static uint8_t *hfoc_lwip_pbuf_slice_payload(
    const struct packet_slice *slice)
{
    return slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start;
}

static bool hfoc_lwip_pbuf_packet_imported(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct packet_slice *slice)
{
    const struct hfoc_lwip_pbuf_packet_import *entry;

    if (slice->slot.slot >= PACKET_POOL_COUNT_MAX) {
        return true;
    }
    entry = &bridge->packet_imports[slice->slot.slot];
    return entry->imported &&
           entry->generation == slice->slot.generation;
}

static void hfoc_lwip_pbuf_packet_mark_imported(
    struct hfoc_lwip_pbuf_bridge *bridge,
    const struct packet_slice *slice)
{
    struct hfoc_lwip_pbuf_packet_import *entry;

    if (slice->slot.slot >= PACKET_POOL_COUNT_MAX) {
        return;
    }
    entry = &bridge->packet_imports[slice->slot.slot];
    entry->imported = true;
    entry->generation = slice->slot.generation;
}

static void hfoc_lwip_pbuf_packet_mark_released(
    struct hfoc_lwip_pbuf_bridge *bridge,
    const struct packet_pool_slot_descriptor *packet)
{
    struct hfoc_lwip_pbuf_packet_import *entry;

    if (packet->slot >= PACKET_POOL_COUNT_MAX) {
        return;
    }
    entry = &bridge->packet_imports[packet->slot];
    if (entry->imported &&
        entry->generation == packet->generation) {
        memset(entry, 0, sizeof(*entry));
    }
}

static enum hfoc_lwip_pbuf_status
hfoc_lwip_pbuf_map_pool_status(enum packet_pool_status status)
{
    switch (status) {
    case PACKET_POOL_OK:
        return HFOC_LWIP_PBUF_OK;
    case PACKET_POOL_NULL:
        return HFOC_LWIP_PBUF_NULL;
    case PACKET_POOL_BAD_STATE:
    case PACKET_POOL_BAD_COUNT:
    case PACKET_POOL_BAD_CAPACITY:
    case PACKET_POOL_BAD_HEADROOM:
    case PACKET_POOL_BAD_LENGTH:
    case PACKET_POOL_BAD_STORAGE:
    case PACKET_POOL_OVERFLOW:
        return HFOC_LWIP_PBUF_BAD_STATE;
    case PACKET_POOL_BAD_SLOT:
        return HFOC_LWIP_PBUF_BAD_SLICE;
    case PACKET_POOL_EXHAUSTED:
        return HFOC_LWIP_PBUF_EXHAUSTED;
    case PACKET_POOL_BAD_DIRECTION:
    case PACKET_POOL_WRONG_OWNER:
        return HFOC_LWIP_PBUF_WRONG_DIRECTION;
    case PACKET_POOL_STALE_DESCRIPTOR:
        return HFOC_LWIP_PBUF_STALE_DESCRIPTOR;
    case PACKET_POOL_DOUBLE_FREE:
        return HFOC_LWIP_PBUF_STALE_CHAIN;
    default:
        return HFOC_LWIP_PBUF_BAD_STATE;
    }
}

static uint32_t hfoc_lwip_pbuf_find_free_slot(
    const struct hfoc_lwip_pbuf_bridge *bridge)
{
    uint32_t slot;

    for (slot = 0U; slot < HFOC_LWIP_PBUF_SLOT_COUNT; ++slot) {
        if (!bridge->used[slot]) {
            return slot;
        }
    }
    return HFOC_LWIP_PBUF_SLOT_COUNT;
}

static uint32_t hfoc_lwip_pbuf_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

static enum hfoc_lwip_pbuf_status
hfoc_lwip_pbuf_release_slot(
    struct hfoc_lwip_pbuf_bridge *bridge,
    uint32_t slot,
    uint32_t expected_generation)
{
    enum packet_pool_status pool_status;
    struct packet_pool_slot_descriptor packet;

    if (slot >= HFOC_LWIP_PBUF_SLOT_COUNT ||
        !bridge->used[slot] ||
        bridge->slot_generations[slot] != expected_generation) {
        return HFOC_LWIP_PBUF_STALE_CHAIN;
    }

    packet = bridge->packet_slots[slot];
    pool_status = packet_pool_release(bridge->pool, &packet);
    if (pool_status != PACKET_POOL_OK) {
        return hfoc_lwip_pbuf_map_pool_status(pool_status);
    }

    hfoc_lwip_pbuf_packet_mark_released(bridge, &packet);
    memset(&bridge->pbufs[slot], 0, sizeof(bridge->pbufs[slot]));
    memset(&bridge->packet_slots[slot], 0,
           sizeof(bridge->packet_slots[slot]));
    bridge->used[slot] = false;
    --bridge->used_count;
    return HFOC_LWIP_PBUF_OK;
}

static void hfoc_lwip_pbuf_custom_free(struct pbuf *pbuf)
{
    struct hfoc_lwip_pbuf_bridge *bridge;
    struct pbuf_custom *custom;
    uintptr_t base;
    uintptr_t address;
    uintptr_t stride;
    uint32_t slot;

    if (pbuf == NULL) {
        return;
    }
    bridge = (struct hfoc_lwip_pbuf_bridge *)pbuf->hfoc_lwip_bridge;
    if (bridge == NULL || !hfoc_lwip_pbuf_bridge_valid(bridge)) {
        return;
    }

    custom = (struct pbuf_custom *)pbuf;
    base = (uintptr_t)&bridge->pbufs[0];
    address = (uintptr_t)custom;
    stride = sizeof(bridge->pbufs[0]);
    if (address < base ||
        address >= base + stride * HFOC_LWIP_PBUF_SLOT_COUNT ||
        (address - base) % stride != 0U) {
        return;
    }

    slot = (uint32_t)((address - base) / stride);
    if (!bridge->used[slot]) {
        return;
    }
    (void)hfoc_lwip_pbuf_release_slot(
        bridge, slot, bridge->slot_generations[slot]);
}

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_bridge_init(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct packet_pool_descriptor *pool)
{
    if (bridge == NULL || pool == NULL) {
        return bridge == NULL ? HFOC_LWIP_PBUF_NULL :
            HFOC_LWIP_PBUF_BAD_POOL;
    }
    if (bridge->published) {
        return HFOC_LWIP_PBUF_ALREADY_PUBLISHED;
    }
    if (!packet_pool_is_published(pool)) {
        return HFOC_LWIP_PBUF_BAD_POOL;
    }
    if (packet_pool_length(pool) > HFOC_LWIP_PBUF_TOT_LEN_MAX) {
        return HFOC_LWIP_PBUF_BAD_POOL;
    }

    memset(bridge, 0, sizeof(*bridge));
    bridge->version = HFOC_LWIP_PBUF_VERSION;
    bridge->generation = 1U;
    bridge->pool = pool;
    bridge->published = true;
    return HFOC_LWIP_PBUF_OK;
}

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_bridge_shutdown(
    struct hfoc_lwip_pbuf_bridge *bridge)
{
    if (bridge == NULL) {
        return HFOC_LWIP_PBUF_NULL;
    }
    if (!bridge->published) {
        return HFOC_LWIP_PBUF_UNPUBLISHED;
    }
    if (bridge->version != HFOC_LWIP_PBUF_VERSION) {
        return HFOC_LWIP_PBUF_INVALID_VERSION;
    }
    if (bridge->used_count != 0U) {
        return HFOC_LWIP_PBUF_LIVE_CHAIN;
    }

    memset(bridge, 0, sizeof(*bridge));
    return HFOC_LWIP_PBUF_OK;
}

bool hfoc_lwip_pbuf_bridge_published(
    const struct hfoc_lwip_pbuf_bridge *bridge)
{
    return hfoc_lwip_pbuf_bridge_valid(bridge);
}

uint32_t hfoc_lwip_pbuf_bridge_used_count(
    const struct hfoc_lwip_pbuf_bridge *bridge)
{
    return hfoc_lwip_pbuf_bridge_valid(bridge)
        ? bridge->used_count
        : 0U;
}

uint32_t hfoc_lwip_pbuf_bridge_free_count(
    const struct hfoc_lwip_pbuf_bridge *bridge)
{
    return hfoc_lwip_pbuf_bridge_valid(bridge)
        ? HFOC_LWIP_PBUF_SLOT_COUNT - bridge->used_count
        : 0U;
}

static enum hfoc_lwip_pbuf_status
hfoc_lwip_pbuf_validate_rx_import(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct packet_slice *slice,
    enum hfoc_lwip_pbuf_result *result)
{
    struct packet_pool_slot_descriptor current;

    if (slice->pool != bridge->pool ||
        !packet_slice_is_valid(slice) ||
        slice->slot.direction != PACKET_POOL_DIRECTION_RX) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (slice->length == 0U ||
        slice->length > HFOC_LWIP_PBUF_TOT_LEN_MAX) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (packet_pool_slot(
            bridge->pool, slice->slot.slot, &current) !=
        PACKET_POOL_OK) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (current.owner != PACKET_POOL_OWNER_ACTIVE ||
        current.direction != PACKET_POOL_DIRECTION_RX ||
        current.generation != slice->slot.generation) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (hfoc_lwip_pbuf_packet_imported(bridge, slice)) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    return HFOC_LWIP_PBUF_OK;
}

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_import_rx(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct packet_slice *slice,
    struct hfoc_lwip_pbuf_chain *chain,
    enum hfoc_lwip_pbuf_result *result)
{
    struct pbuf *pbuf;
    enum hfoc_lwip_pbuf_status status;
    uint32_t generation;
    uint32_t slot;

    if (bridge == NULL || slice == NULL || chain == NULL ||
        result == NULL) {
        return HFOC_LWIP_PBUF_NULL;
    }
    if (!hfoc_lwip_pbuf_bridge_valid(bridge)) {
        return HFOC_LWIP_PBUF_BAD_STATE;
    }

    *result = HFOC_LWIP_PBUF_ACCEPTED;
    status = hfoc_lwip_pbuf_validate_rx_import(
        bridge, slice, result);
    if (status != HFOC_LWIP_PBUF_OK) {
        return status;
    }
    if (*result == HFOC_LWIP_PBUF_REJECTED) {
        return HFOC_LWIP_PBUF_OK;
    }

    slot = hfoc_lwip_pbuf_find_free_slot(bridge);
    if (slot == HFOC_LWIP_PBUF_SLOT_COUNT) {
        *result = HFOC_LWIP_PBUF_BACKPRESSURE;
        return HFOC_LWIP_PBUF_OK;
    }

    generation = hfoc_lwip_pbuf_next_generation(
        bridge->slot_generations[slot]);
    memset(chain, 0, sizeof(*chain));
    chain->version = HFOC_LWIP_PBUF_VERSION;
    chain->bridge_generation = bridge->generation;
    chain->slot = slot;
    chain->slot_generation = generation;
    chain->packet = slice->slot;
    chain->head = &bridge->pbufs[slot].pbuf;
    chain->published = true;

    bridge->packet_slots[slot] = slice->slot;
    bridge->slot_generations[slot] = generation;
    bridge->used[slot] = true;
    hfoc_lwip_pbuf_packet_mark_imported(bridge, slice);
    ++bridge->used_count;

    pbuf = &bridge->pbufs[slot].pbuf;
    memset(pbuf, 0, sizeof(*pbuf));
    pbuf->next = NULL;
    pbuf->payload = hfoc_lwip_pbuf_slice_payload(slice);
    pbuf->tot_len = (uint16_t)slice->length;
    pbuf->len = (uint16_t)slice->length;
    pbuf->type_internal = (uint8_t)PBUF_REF;
    pbuf->flags = PBUF_FLAG_IS_CUSTOM;
    pbuf->ref = 1U;
    pbuf->if_idx = 0U;
    pbuf->hfoc_lwip_bridge = bridge;
    bridge->pbufs[slot].custom_free_function =
        hfoc_lwip_pbuf_custom_free;

    *result = HFOC_LWIP_PBUF_ACCEPTED;
    return HFOC_LWIP_PBUF_OK;
}

static bool hfoc_lwip_pbuf_range_overlap(
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

static bool hfoc_lwip_pbuf_chain_collect(
    const struct pbuf *chain,
    uint32_t *total,
    uint32_t *count)
{
    const struct pbuf *current;
    uint32_t sum;
    uint32_t nodes;

    if (chain == NULL) {
        return false;
    }
    sum = 0U;
    nodes = 0U;
    current = chain;
    while (current != NULL) {
        uint32_t tail_total;

        if (nodes >= HFOC_LWIP_PBUF_CHAIN_MAX) {
            return false;
        }
        if (current->len > current->tot_len) {
            return false;
        }
        if (current->len > 0U && current->payload == NULL) {
            return false;
        }
        tail_total = current->next != NULL
            ? current->next->tot_len : 0U;
        if ((uint32_t)current->tot_len !=
            (uint32_t)current->len + tail_total) {
            return false;
        }
        sum += current->len;
        if (sum > HFOC_LWIP_PBUF_TOT_LEN_MAX) {
            return false;
        }
        ++nodes;
        current = current->next;
    }
    if (nodes == 0U || sum == 0U) {
        return false;
    }
    *total = sum;
    *count = nodes;
    return true;
}

static bool hfoc_lwip_pbuf_export_slice_compatible(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct packet_slice *slice)
{
    return slice != NULL &&
           slice->pool == bridge->pool &&
           packet_slice_is_valid(slice) &&
           slice->slot.direction == PACKET_POOL_DIRECTION_TX;
}

static bool hfoc_lwip_pbuf_chain_overlaps_slice(
    const struct pbuf *chain,
    const struct packet_slice *slice)
{
    uintptr_t slot_base;
    size_t slot_size;
    const struct pbuf *current;

    slot_base = (uintptr_t)slice->pool->storage +
        (uintptr_t)slice->slot.slot *
            ((uintptr_t)slice->pool->headroom +
             (uintptr_t)slice->pool->capacity);
    slot_size = (size_t)slice->pool->headroom +
        (size_t)slice->pool->capacity;

    for (current = chain; current != NULL;
         current = current->next) {
        if (current->len > 0U &&
            hfoc_lwip_pbuf_range_overlap(
                (uintptr_t)current->payload,
                (size_t)current->len,
                slot_base,
                slot_size)) {
            return true;
        }
    }
    return false;
}

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_export_tx(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct pbuf *chain,
    struct packet_slice *slice,
    enum hfoc_lwip_pbuf_result *result)
{
    const struct pbuf *current;
    uint32_t total;
    uint32_t count;

    if (bridge == NULL || chain == NULL || slice == NULL ||
        result == NULL) {
        return HFOC_LWIP_PBUF_NULL;
    }
    if (!hfoc_lwip_pbuf_bridge_valid(bridge)) {
        return HFOC_LWIP_PBUF_BAD_STATE;
    }
    if (!hfoc_lwip_pbuf_chain_collect(chain, &total, &count)) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (!hfoc_lwip_pbuf_export_slice_compatible(bridge, slice) ||
        slice->length != total ||
        total > HFOC_LWIP_PBUF_TOT_LEN_MAX) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }
    if (hfoc_lwip_pbuf_chain_overlaps_slice(chain, slice)) {
        *result = HFOC_LWIP_PBUF_REJECTED;
        return HFOC_LWIP_PBUF_OK;
    }

    if (packet_slice_seek(slice, 0U) != PACKET_SLICE_OK) {
        return HFOC_LWIP_PBUF_BAD_SLICE;
    }
    for (current = chain; current != NULL;
         current = current->next) {
        if (current->len == 0U) {
            continue;
        }
        if (packet_slice_write(
                slice, current->payload,
                current->len) != PACKET_SLICE_OK) {
            *result = HFOC_LWIP_PBUF_REJECTED;
            return HFOC_LWIP_PBUF_COPY_FAILED;
        }
    }

    *result = HFOC_LWIP_PBUF_ACCEPTED;
    return HFOC_LWIP_PBUF_OK;
}

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_release_chain(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct hfoc_lwip_pbuf_chain *chain)
{
    enum hfoc_lwip_pbuf_status status;

    if (bridge == NULL || chain == NULL) {
        return HFOC_LWIP_PBUF_NULL;
    }
    if (!hfoc_lwip_pbuf_bridge_valid(bridge)) {
        return HFOC_LWIP_PBUF_BAD_STATE;
    }
    if (!chain->published ||
        chain->version != HFOC_LWIP_PBUF_VERSION ||
        chain->bridge_generation != bridge->generation) {
        return HFOC_LWIP_PBUF_STALE_CHAIN;
    }

    status = hfoc_lwip_pbuf_release_slot(
        bridge, chain->slot, chain->slot_generation);
    if (status == HFOC_LWIP_PBUF_OK) {
        memset(chain, 0, sizeof(*chain));
    }
    return status;
}

const char *hfoc_lwip_pbuf_result_string(
    enum hfoc_lwip_pbuf_result result)
{
    switch (result) {
    case HFOC_LWIP_PBUF_ACCEPTED:
        return "HFOC_LWIP_PBUF_ACCEPTED";
    case HFOC_LWIP_PBUF_BACKPRESSURE:
        return "HFOC_LWIP_PBUF_BACKPRESSURE";
    case HFOC_LWIP_PBUF_REJECTED:
        return "HFOC_LWIP_PBUF_REJECTED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_pbuf_status_string(
    enum hfoc_lwip_pbuf_status status)
{
    switch (status) {
    case HFOC_LWIP_PBUF_OK:
        return "HFOC_LWIP_PBUF_OK";
    case HFOC_LWIP_PBUF_NULL:
        return "HFOC_LWIP_PBUF_NULL";
    case HFOC_LWIP_PBUF_BAD_STATE:
        return "HFOC_LWIP_PBUF_BAD_STATE";
    case HFOC_LWIP_PBUF_INVALID_VERSION:
        return "HFOC_LWIP_PBUF_INVALID_VERSION";
    case HFOC_LWIP_PBUF_ALREADY_PUBLISHED:
        return "HFOC_LWIP_PBUF_ALREADY_PUBLISHED";
    case HFOC_LWIP_PBUF_UNPUBLISHED:
        return "HFOC_LWIP_PBUF_UNPUBLISHED";
    case HFOC_LWIP_PBUF_BAD_POOL:
        return "HFOC_LWIP_PBUF_BAD_POOL";
    case HFOC_LWIP_PBUF_BAD_SLICE:
        return "HFOC_LWIP_PBUF_BAD_SLICE";
    case HFOC_LWIP_PBUF_WRONG_DIRECTION:
        return "HFOC_LWIP_PBUF_WRONG_DIRECTION";
    case HFOC_LWIP_PBUF_STALE_DESCRIPTOR:
        return "HFOC_LWIP_PBUF_STALE_DESCRIPTOR";
    case HFOC_LWIP_PBUF_INVALID_LENGTH:
        return "HFOC_LWIP_PBUF_INVALID_LENGTH";
    case HFOC_LWIP_PBUF_INVALID_CHAIN:
        return "HFOC_LWIP_PBUF_INVALID_CHAIN";
    case HFOC_LWIP_PBUF_EXHAUSTED:
        return "HFOC_LWIP_PBUF_EXHAUSTED";
    case HFOC_LWIP_PBUF_LIVE_CHAIN:
        return "HFOC_LWIP_PBUF_LIVE_CHAIN";
    case HFOC_LWIP_PBUF_STALE_CHAIN:
        return "HFOC_LWIP_PBUF_STALE_CHAIN";
    case HFOC_LWIP_PBUF_COPY_FAILED:
        return "HFOC_LWIP_PBUF_COPY_FAILED";
    default:
        return NULL;
    }
}
