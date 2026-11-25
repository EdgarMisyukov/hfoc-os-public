#include "virtio_dma_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_dma_region_alignment_valid(uint32_t alignment)
{
    return alignment >= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MIN &&
           alignment <= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX &&
           (alignment & (alignment - 1U)) == 0U;
}

static enum virtio_dma_region_status
virtio_dma_region_window_validate(
    const struct virtio_dma_window_policy *window)
{
    if (window == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    if (window->length == 0U) {
        return VIRTIO_DMA_REGION_INVALID_WINDOW;
    }
    if (window->base > UINT64_MAX - window->length) {
        return VIRTIO_DMA_REGION_OVERFLOW;
    }
    if (!virtio_dma_region_alignment_valid(window->alignment)) {
        return VIRTIO_DMA_REGION_INVALID_ALIGNMENT;
    }
    if (window->base % window->alignment != 0U) {
        return VIRTIO_DMA_REGION_UNALIGNED;
    }
    return VIRTIO_DMA_REGION_OK;
}

static bool virtio_dma_region_direction_valid(
    enum virtio_dma_direction direction)
{
    return direction == VIRTIO_DMA_DIRECTION_DEVICE_READ ||
           direction == VIRTIO_DMA_DIRECTION_DEVICE_WRITE ||
           direction == VIRTIO_DMA_DIRECTION_BIDIRECTIONAL;
}

static bool virtio_dma_region_lifetime_valid(
    enum virtio_dma_lifetime lifetime)
{
    return lifetime == VIRTIO_DMA_LIFETIME_QUEUE ||
           lifetime == VIRTIO_DMA_LIFETIME_PAYLOAD;
}

static bool virtio_dma_region_zeroization_valid(
    enum virtio_dma_zeroization zeroization)
{
    return zeroization == VIRTIO_DMA_ZEROIZATION_NONE ||
           zeroization == VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
}

static enum virtio_dma_region_status
virtio_dma_region_registry_validate(
    const struct virtio_dma_region_registry *registry)
{
    enum virtio_dma_region_status status;
    uint32_t bound_count;
    uint32_t index;

    if (registry == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    if (!registry->published) {
        return VIRTIO_DMA_REGION_UNPUBLISHED;
    }
    if (registry->version != VIRTIO_DMA_REGION_VERSION) {
        return VIRTIO_DMA_REGION_INVALID_VERSION;
    }
    if (registry->capacity < VIRTIO_DMA_REGION_CAPACITY_MIN ||
        registry->capacity > VIRTIO_DMA_REGION_CAPACITY_MAX ||
        registry->free_count > registry->capacity ||
        registry->bound_count > registry->capacity ||
        registry->free_count + registry->bound_count !=
            registry->capacity) {
        return VIRTIO_DMA_REGION_BAD_STATE;
    }
    status = virtio_dma_region_window_validate(&registry->window);
    if (status != VIRTIO_DMA_REGION_OK) {
        return VIRTIO_DMA_REGION_BAD_STATE;
    }
    if (registry->zeroize.zeroize == NULL ||
        registry->zeroize.context == NULL) {
        return VIRTIO_DMA_REGION_BAD_STATE;
    }

    bound_count = 0U;
    for (index = 0U; index < registry->capacity; ++index) {
        const struct virtio_dma_region_slot *slot = &registry->slots[index];
        const struct virtio_dma_region *region = &slot->region;

        if (slot->generation == 0U) {
            return VIRTIO_DMA_REGION_BAD_STATE;
        }
        if (!slot->bound) {
            if (region->published) {
                return VIRTIO_DMA_REGION_BAD_STATE;
            }
            continue;
        }

        ++bound_count;
        if (!region->published ||
            region->version != VIRTIO_DMA_REGION_VERSION ||
            region->index != index ||
            region->generation != slot->generation ||
            region->length == 0U ||
            region->length > VIRTIO_DMA_REGION_LENGTH_MAX ||
            region->physical_base > UINT64_MAX - region->length ||
            region->physical_end != region->physical_base + region->length ||
            region->physical_base < registry->window.base ||
            region->physical_end >
                registry->window.base + registry->window.length ||
            region->physical_base % registry->window.alignment != 0U ||
            !virtio_dma_region_direction_valid(region->direction) ||
            !virtio_dma_region_lifetime_valid(region->lifetime) ||
            !virtio_dma_region_zeroization_valid(region->zeroization)) {
            return VIRTIO_DMA_REGION_BAD_STATE;
        }
    }
    if (bound_count != registry->bound_count) {
        return VIRTIO_DMA_REGION_BAD_STATE;
    }
    return VIRTIO_DMA_REGION_OK;
}

static uint32_t virtio_dma_region_next_generation(uint32_t generation)
{
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

enum virtio_dma_region_status virtio_dma_region_init(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_window_policy *window,
    const struct virtio_dma_zeroize *zeroize,
    uint32_t capacity)
{
    struct virtio_dma_region_registry draft;
    enum virtio_dma_region_status status;
    uint32_t index;

    if (registry == NULL ||
        window == NULL ||
        zeroize == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    if (registry->published) {
        return VIRTIO_DMA_REGION_ALREADY_PUBLISHED;
    }
    if (registry->version != 0U) {
        return VIRTIO_DMA_REGION_INVALID_VERSION;
    }
    if (capacity < VIRTIO_DMA_REGION_CAPACITY_MIN ||
        capacity > VIRTIO_DMA_REGION_CAPACITY_MAX) {
        return VIRTIO_DMA_REGION_BAD_CAPACITY;
    }
    status = virtio_dma_region_window_validate(window);
    if (status != VIRTIO_DMA_REGION_OK) {
        return status;
    }
    if (zeroize->zeroize == NULL || zeroize->context == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }

    memset(&draft, 0, sizeof(draft));
    draft.window = *window;
    draft.zeroize = *zeroize;
    draft.capacity = capacity;
    draft.free_count = capacity;
    draft.version = VIRTIO_DMA_REGION_VERSION;
    for (index = 0U; index < capacity; ++index) {
        draft.slots[index].generation = 1U;
    }
    draft.published = true;
    *registry = draft;
    return VIRTIO_DMA_REGION_OK;
}

static enum virtio_dma_region_status
virtio_dma_region_request_validate(
    const struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_request *request)
{
    uint64_t physical_end;

    if (request->length == 0U ||
        request->length > VIRTIO_DMA_REGION_LENGTH_MAX) {
        return VIRTIO_DMA_REGION_INVALID_RANGE;
    }
    if (request->physical_base > UINT64_MAX - request->length) {
        return VIRTIO_DMA_REGION_OVERFLOW;
    }
    if (request->physical_base % registry->window.alignment != 0U) {
        return VIRTIO_DMA_REGION_UNALIGNED;
    }
    physical_end = request->physical_base + request->length;
    if (request->physical_base < registry->window.base ||
        physical_end > registry->window.base + registry->window.length) {
        return VIRTIO_DMA_REGION_FORBIDDEN_RANGE;
    }
    if (!virtio_dma_region_direction_valid(request->direction)) {
        return VIRTIO_DMA_REGION_INVALID_DIRECTION;
    }
    if (!virtio_dma_region_lifetime_valid(request->lifetime)) {
        return VIRTIO_DMA_REGION_INVALID_LIFETIME;
    }
    if (!virtio_dma_region_zeroization_valid(request->zeroization)) {
        return VIRTIO_DMA_REGION_INVALID_ZEROIZATION;
    }
    return VIRTIO_DMA_REGION_OK;
}

static bool virtio_dma_region_aliases(
    const struct virtio_dma_region_registry *registry,
    uint64_t physical_base,
    uint64_t physical_end)
{
    uint32_t index;

    for (index = 0U; index < registry->capacity; ++index) {
        const struct virtio_dma_region_slot *slot = &registry->slots[index];

        if (slot->bound &&
            physical_base < slot->region.physical_end &&
            slot->region.physical_base < physical_end) {
            return true;
        }
    }
    return false;
}

enum virtio_dma_region_status virtio_dma_region_bind(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_request *request,
    struct virtio_dma_region_token *token,
    struct virtio_dma_region *region_out)
{
    enum virtio_dma_region_status status;
    struct virtio_dma_region draft;
    uint64_t physical_end;
    uint32_t index;

    if (registry == NULL ||
        request == NULL ||
        token == NULL ||
        region_out == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    status = virtio_dma_region_registry_validate(registry);
    if (status != VIRTIO_DMA_REGION_OK) {
        return status;
    }
    status = virtio_dma_region_request_validate(registry, request);
    if (status != VIRTIO_DMA_REGION_OK) {
        return status;
    }
    if (registry->free_count == 0U) {
        return VIRTIO_DMA_REGION_EXHAUSTED;
    }
    physical_end = request->physical_base + request->length;
    if (virtio_dma_region_aliases(
            registry, request->physical_base, physical_end)) {
        return VIRTIO_DMA_REGION_ALIAS;
    }

    for (index = 0U; index < registry->capacity; ++index) {
        struct virtio_dma_region_slot *slot = &registry->slots[index];

        if (slot->bound) {
            continue;
        }

        memset(&draft, 0, sizeof(draft));
        draft.published = true;
        draft.version = VIRTIO_DMA_REGION_VERSION;
        draft.index = index;
        draft.generation = slot->generation;
        draft.physical_base = request->physical_base;
        draft.physical_end = physical_end;
        draft.length = request->length;
        draft.direction = request->direction;
        draft.lifetime = request->lifetime;
        draft.zeroization = request->zeroization;

        slot->bound = true;
        slot->region = draft;
        --registry->free_count;
        ++registry->bound_count;
        token->index = index;
        token->generation = slot->generation;
        *region_out = draft;
        return VIRTIO_DMA_REGION_OK;
    }
    return VIRTIO_DMA_REGION_BAD_STATE;
}

enum virtio_dma_region_status virtio_dma_region_unbind(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_token *token)
{
    enum virtio_dma_region_status status;
    struct virtio_dma_region_slot *slot;

    if (registry == NULL || token == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    status = virtio_dma_region_registry_validate(registry);
    if (status != VIRTIO_DMA_REGION_OK) {
        return status;
    }
    if (token->index >= registry->capacity) {
        return VIRTIO_DMA_REGION_BAD_TOKEN;
    }
    slot = &registry->slots[token->index];
    if (!slot->bound) {
        return VIRTIO_DMA_REGION_BAD_TOKEN;
    }
    if (token->generation != slot->generation) {
        return VIRTIO_DMA_REGION_STALE_TOKEN;
    }
    if (slot->region.zeroization == VIRTIO_DMA_ZEROIZATION_ON_RELEASE &&
        !registry->zeroize.zeroize(
            registry->zeroize.context,
            slot->region.physical_base,
            slot->region.length)) {
        return VIRTIO_DMA_REGION_ZEROIZE_REJECTED;
    }

    slot->bound = false;
    slot->generation = virtio_dma_region_next_generation(
        slot->generation);
    memset(&slot->region, 0, sizeof(slot->region));
    ++registry->free_count;
    --registry->bound_count;
    return VIRTIO_DMA_REGION_OK;
}

enum virtio_dma_region_status virtio_dma_region_teardown(
    struct virtio_dma_region_registry *registry)
{
    enum virtio_dma_region_status status;

    if (registry == NULL) {
        return VIRTIO_DMA_REGION_NULL;
    }
    status = virtio_dma_region_registry_validate(registry);
    if (status != VIRTIO_DMA_REGION_OK) {
        return status;
    }
    if (registry->bound_count != 0U) {
        return VIRTIO_DMA_REGION_LIVE_OWNERS;
    }
    memset(registry, 0, sizeof(*registry));
    return VIRTIO_DMA_REGION_OK;
}

bool virtio_dma_region_published(
    const struct virtio_dma_region_registry *registry)
{
    return virtio_dma_region_registry_validate(registry) ==
           VIRTIO_DMA_REGION_OK;
}

uint32_t virtio_dma_region_free_count(
    const struct virtio_dma_region_registry *registry)
{
    return virtio_dma_region_registry_validate(registry) ==
               VIRTIO_DMA_REGION_OK ?
               registry->free_count : 0U;
}

uint32_t virtio_dma_region_bound_count(
    const struct virtio_dma_region_registry *registry)
{
    return virtio_dma_region_registry_validate(registry) ==
               VIRTIO_DMA_REGION_OK ?
               registry->bound_count : 0U;
}

const char *virtio_dma_region_status_string(
    enum virtio_dma_region_status status)
{
    switch (status) {
    case VIRTIO_DMA_REGION_OK:
        return "VIRTIO_DMA_REGION_OK";
    case VIRTIO_DMA_REGION_NULL:
        return "VIRTIO_DMA_REGION_NULL";
    case VIRTIO_DMA_REGION_BAD_STATE:
        return "VIRTIO_DMA_REGION_BAD_STATE";
    case VIRTIO_DMA_REGION_BAD_CAPACITY:
        return "VIRTIO_DMA_REGION_BAD_CAPACITY";
    case VIRTIO_DMA_REGION_ALREADY_PUBLISHED:
        return "VIRTIO_DMA_REGION_ALREADY_PUBLISHED";
    case VIRTIO_DMA_REGION_UNPUBLISHED:
        return "VIRTIO_DMA_REGION_UNPUBLISHED";
    case VIRTIO_DMA_REGION_INVALID_VERSION:
        return "VIRTIO_DMA_REGION_INVALID_VERSION";
    case VIRTIO_DMA_REGION_INVALID_WINDOW:
        return "VIRTIO_DMA_REGION_INVALID_WINDOW";
    case VIRTIO_DMA_REGION_INVALID_ALIGNMENT:
        return "VIRTIO_DMA_REGION_INVALID_ALIGNMENT";
    case VIRTIO_DMA_REGION_INVALID_LIFETIME:
        return "VIRTIO_DMA_REGION_INVALID_LIFETIME";
    case VIRTIO_DMA_REGION_INVALID_DIRECTION:
        return "VIRTIO_DMA_REGION_INVALID_DIRECTION";
    case VIRTIO_DMA_REGION_INVALID_ZEROIZATION:
        return "VIRTIO_DMA_REGION_INVALID_ZEROIZATION";
    case VIRTIO_DMA_REGION_INVALID_RANGE:
        return "VIRTIO_DMA_REGION_INVALID_RANGE";
    case VIRTIO_DMA_REGION_OVERFLOW:
        return "VIRTIO_DMA_REGION_OVERFLOW";
    case VIRTIO_DMA_REGION_UNALIGNED:
        return "VIRTIO_DMA_REGION_UNALIGNED";
    case VIRTIO_DMA_REGION_FORBIDDEN_RANGE:
        return "VIRTIO_DMA_REGION_FORBIDDEN_RANGE";
    case VIRTIO_DMA_REGION_ALIAS:
        return "VIRTIO_DMA_REGION_ALIAS";
    case VIRTIO_DMA_REGION_EXHAUSTED:
        return "VIRTIO_DMA_REGION_EXHAUSTED";
    case VIRTIO_DMA_REGION_BAD_TOKEN:
        return "VIRTIO_DMA_REGION_BAD_TOKEN";
    case VIRTIO_DMA_REGION_STALE_TOKEN:
        return "VIRTIO_DMA_REGION_STALE_TOKEN";
    case VIRTIO_DMA_REGION_ZEROIZE_REJECTED:
        return "VIRTIO_DMA_REGION_ZEROIZE_REJECTED";
    case VIRTIO_DMA_REGION_LIVE_OWNERS:
        return "VIRTIO_DMA_REGION_LIVE_OWNERS";
    default:
        return NULL;
    }
}
