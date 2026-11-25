#include "virtio_net_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_rx_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_RX_GENERATION_MIN &&
           generation <= VIRTIO_NET_RX_GENERATION_MAX;
}

static bool virtio_net_rx_capacity_valid(uint32_t capacity)
{
    return capacity >= VIRTIO_NET_RX_CAPACITY_MIN &&
           capacity <= VIRTIO_NET_RX_CAPACITY_MAX;
}

static bool virtio_net_rx_buffer_length_valid(uint32_t length)
{
    return length >= VIRTIO_NET_RX_BUFFER_LENGTH_MIN &&
           length <= VIRTIO_NET_RX_BUFFER_LENGTH_MAX;
}

static bool virtio_net_rx_frame_capacity_valid(uint32_t capacity)
{
    return capacity >= VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MIN &&
           capacity <= VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MAX;
}

static bool virtio_net_rx_dma_alignment_valid(uint32_t alignment)
{
    return alignment >= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MIN &&
           alignment <= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX &&
           (alignment & (alignment - 1U)) == 0U;
}

static bool virtio_net_rx_dma_window_valid(
    const struct virtio_dma_window_policy *window)
{
    return window != NULL &&
           window->length != 0U &&
           window->base <= UINT64_MAX - window->length &&
           virtio_net_rx_dma_alignment_valid(window->alignment) &&
           window->base % window->alignment == 0U;
}

static bool virtio_net_rx_zeroize_valid(
    const struct virtio_dma_zeroize *zeroize)
{
    return zeroize != NULL &&
           zeroize->zeroize != NULL &&
           zeroize->context != NULL;
}

static bool virtio_net_rx_ranges_overlap(
    uint64_t first_base,
    uint64_t first_end,
    uint64_t second_base,
    uint64_t second_end)
{
    return first_base < second_end && second_base < first_end;
}

static enum virtio_net_rx_status
virtio_net_rx_config_validate(
    const struct virtio_net_rx_config *config,
    const struct virtio_net_queue *queue)
{
    uint64_t window_end;
    uint64_t queue_end;
    uint32_t index;

    if (config == NULL) {
        return VIRTIO_NET_RX_NULL;
    }
    if (config->buffers == NULL) {
        return VIRTIO_NET_RX_INVALID_CONFIG;
    }
    if (!virtio_net_rx_capacity_valid(config->buffer_count)) {
        return VIRTIO_NET_RX_INVALID_BUFFER_COUNT;
    }
    if (config->buffer_count > queue->queue_size) {
        return VIRTIO_NET_RX_QUEUE_FULL;
    }
    if (!virtio_net_rx_frame_capacity_valid(
            config->minimum_frame_capacity)) {
        return VIRTIO_NET_RX_INVALID_FRAME_CAPACITY;
    }
    if (!virtio_net_rx_capacity_valid(config->dma_capacity)) {
        return VIRTIO_NET_RX_INVALID_DMA;
    }
    if (config->dma_capacity < config->buffer_count) {
        return VIRTIO_NET_RX_DMA_SHORTAGE;
    }
    if (!virtio_net_rx_dma_window_valid(&config->dma_window) ||
        !virtio_net_rx_zeroize_valid(&config->dma_zeroize)) {
        return VIRTIO_NET_RX_INVALID_DMA;
    }

    window_end = config->dma_window.base + config->dma_window.length;
    queue_end = queue->layout.region_base + queue->layout.total_size;
    for (index = 0U; index < config->buffer_count; ++index) {
        const struct virtio_net_rx_buffer_descriptor *buffer =
            &config->buffers[index];
        uint64_t buffer_end;
        uint32_t prior;

        if (!virtio_net_rx_buffer_length_valid(buffer->length) ||
            buffer->length < config->minimum_frame_capacity) {
            return VIRTIO_NET_RX_INVALID_BUFFER;
        }
        if (buffer->physical_base > UINT64_MAX - buffer->length) {
            return VIRTIO_NET_RX_BUFFER_OVERFLOW;
        }
        if (buffer->physical_base % config->dma_window.alignment != 0U) {
            return VIRTIO_NET_RX_BUFFER_UNALIGNED;
        }

        buffer_end = buffer->physical_base + buffer->length;
        if (buffer->physical_base < config->dma_window.base ||
            buffer_end > window_end) {
            return VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW;
        }
        if (virtio_net_rx_ranges_overlap(
                buffer->physical_base,
                buffer_end,
                queue->layout.region_base,
                queue_end)) {
            return VIRTIO_NET_RX_BUFFER_ALIAS;
        }
        for (prior = 0U; prior < index; ++prior) {
            const struct virtio_net_rx_buffer_descriptor *previous =
                &config->buffers[prior];
            uint64_t previous_end =
                previous->physical_base + previous->length;

            if (virtio_net_rx_ranges_overlap(
                    buffer->physical_base,
                    buffer_end,
                    previous->physical_base,
                    previous_end)) {
                return VIRTIO_NET_RX_BUFFER_ALIAS;
            }
        }
    }
    return VIRTIO_NET_RX_OK;
}

static enum virtio_net_rx_status
virtio_net_rx_composition_validate(
    const struct virtio_net_queue_composition *composition)
{
    const struct virtio_net_queue *queue;

    if (composition == NULL) {
        return VIRTIO_NET_RX_NULL;
    }
    if (!virtio_net_queue_composition_published(composition)) {
        return VIRTIO_NET_RX_INVALID_COMPOSITION;
    }

    queue = &composition->receive;
    if (!virtio_descriptor_ownership_published(&queue->descriptors) ||
        virtio_descriptor_ownership_free_count(&queue->descriptors) !=
            queue->queue_size ||
        virtio_descriptor_ownership_reserved_count(&queue->descriptors) !=
            0U ||
        virtio_descriptor_ownership_published_count(&queue->descriptors) !=
            0U ||
        !virtio_available_publication_published(&queue->available) ||
        virtio_available_publication_published_count(
            &queue->available) != 0U ||
        virtio_available_publication_pending_length(
            &queue->available) != 0U ||
        !virtio_dma_region_published(&queue->dma) ||
        virtio_dma_region_bound_count(&queue->dma) != 1U) {
        return VIRTIO_NET_RX_INVALID_COMPOSITION;
    }
    return VIRTIO_NET_RX_OK;
}

static enum virtio_net_rx_status
virtio_net_rx_controller_validate(
    const struct virtio_net_rx *rx)
{
    const struct virtio_net_queue *queue;
    uint32_t posted_seen;
    uint32_t index;

    if (rx == NULL) {
        return VIRTIO_NET_RX_NULL;
    }
    if (!rx->published) {
        return VIRTIO_NET_RX_UNPUBLISHED;
    }
    if (rx->version != VIRTIO_NET_RX_VERSION) {
        return VIRTIO_NET_RX_INVALID_VERSION;
    }
    if (!virtio_net_rx_generation_valid(rx->generation)) {
        return VIRTIO_NET_RX_INVALID_GENERATION;
    }
    if (!virtio_net_rx_capacity_valid(rx->capacity) ||
        rx->posted_count > rx->capacity ||
        !virtio_net_rx_frame_capacity_valid(rx->minimum_frame_capacity) ||
        !virtio_net_rx_capacity_valid(rx->queue_size) ||
        rx->composition == NULL ||
        !virtio_net_queue_composition_published(rx->composition)) {
        return VIRTIO_NET_RX_INVALID_STATE;
    }

    queue = &rx->composition->receive;
    if (!virtio_descriptor_ownership_published(&queue->descriptors) ||
        !virtio_available_publication_published(&queue->available) ||
        virtio_available_publication_published_count(
            &queue->available) != rx->capacity ||
        !virtio_dma_region_published(&rx->dma) ||
        virtio_dma_region_bound_count(&rx->dma) != rx->capacity) {
        return VIRTIO_NET_RX_INVALID_STATE;
    }

    posted_seen = 0U;
    for (index = 0U; index < rx->capacity; ++index) {
        const struct virtio_net_rx_buffer *buffer = &rx->buffers[index];
        const struct virtio_dma_region_slot *dma_slot;

        if (!virtio_net_rx_generation_valid(buffer->generation) ||
            !virtio_net_rx_buffer_length_valid(buffer->length) ||
            buffer->length < rx->minimum_frame_capacity ||
            buffer->physical_base > UINT64_MAX - buffer->length ||
            buffer->descriptor.index >= queue->queue_size ||
            buffer->dma.index >= rx->dma.capacity) {
            return VIRTIO_NET_RX_INVALID_STATE;
        }

        dma_slot = &rx->dma.slots[buffer->dma.index];
        if (!dma_slot->bound ||
            dma_slot->generation != buffer->dma.generation ||
            dma_slot->region.physical_base != buffer->physical_base ||
            dma_slot->region.length != buffer->length ||
            dma_slot->region.direction != VIRTIO_NET_RX_DMA_DIRECTION ||
            dma_slot->region.lifetime != VIRTIO_NET_RX_DMA_LIFETIME ||
            dma_slot->region.zeroization != VIRTIO_NET_RX_DMA_ZEROIZATION) {
            return VIRTIO_NET_RX_INVALID_STATE;
        }
        if (!buffer->posted) {
            if (queue->descriptors.states[buffer->descriptor.index] !=
                VIRTIO_DESCRIPTOR_STATE_FREE) {
                return VIRTIO_NET_RX_INVALID_STATE;
            }
            continue;
        }
        ++posted_seen;
        if (queue->descriptors.generations[buffer->descriptor.index] !=
                buffer->descriptor.generation ||
            queue->descriptors.states[buffer->descriptor.index] !=
                VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
            return VIRTIO_NET_RX_INVALID_STATE;
        }
    }
    if (posted_seen != rx->posted_count) {
        return VIRTIO_NET_RX_INVALID_STATE;
    }
    return VIRTIO_NET_RX_OK;
}

static void virtio_net_rx_restore_available_ring(
    struct virtio_available_ring *ring,
    const struct virtio_available_ring *snapshot,
    uint32_t queue_size)
{
    uint32_t index;

    ring->flags = snapshot->flags;
    ring->idx = snapshot->idx;
    for (index = 0U; index < queue_size; ++index) {
        ring->ring[index] = snapshot->ring[index];
    }
    ring->used_event = snapshot->used_event;
}

static void virtio_net_rx_rollback(
    struct virtio_net_rx *draft,
    struct virtio_net_queue *queue,
    const struct virtio_available_ring *ring_snapshot,
    uint32_t posted_count)
{
    uint32_t queue_size = queue->layout.queue_size;
    uint32_t index;

    for (index = posted_count; index > 0U; --index) {
        const struct virtio_net_rx_buffer *buffer =
            &draft->buffers[index - 1U];

        (void)virtio_descriptor_ownership_consume(
            &queue->descriptors, &buffer->descriptor);
        (void)virtio_dma_region_unbind(&draft->dma, &buffer->dma);
    }
    virtio_net_rx_restore_available_ring(
        queue->available.ring, ring_snapshot, queue_size);
}

static void virtio_net_rx_release_descriptor(
    struct virtio_net_queue *queue,
    const struct virtio_descriptor_token *token)
{
    if (queue->descriptors.states[token->index] ==
        VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
        (void)virtio_descriptor_ownership_consume(
            &queue->descriptors, token);
    } else {
        (void)virtio_descriptor_ownership_rollback(
            &queue->descriptors, token);
    }
}

enum virtio_net_rx_status virtio_net_rx_activate(
    struct virtio_net_rx *rx,
    struct virtio_net_queue_composition *composition,
    const struct virtio_net_rx_config *config)
{
    struct virtio_net_rx draft;
    struct virtio_net_queue draft_queue;
    struct virtio_available_ring ring_snapshot;
    struct virtio_dma_region_request dma_request;
    enum virtio_net_rx_status status;
    enum virtio_dma_region_status dma_status;
    enum virtio_descriptor_ownership_status descriptor_status;
    enum virtio_available_publication_status available_status;
    struct virtio_available_token available_token;
    uint32_t posted_count;
    uint32_t index;

    if (rx == NULL || composition == NULL || config == NULL) {
        return VIRTIO_NET_RX_NULL;
    }
    if (rx->published) {
        return VIRTIO_NET_RX_ALREADY_PUBLISHED;
    }
    if (rx->version != 0U) {
        return VIRTIO_NET_RX_INVALID_VERSION;
    }
    status = virtio_net_rx_composition_validate(composition);
    if (status != VIRTIO_NET_RX_OK) {
        return status;
    }
    status = virtio_net_rx_config_validate(
        config, &composition->receive);
    if (status != VIRTIO_NET_RX_OK) {
        return status;
    }

    memset(&draft, 0, sizeof(draft));
    dma_status = virtio_dma_region_init(
        &draft.dma,
        &config->dma_window,
        &config->dma_zeroize,
        config->dma_capacity);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        return VIRTIO_NET_RX_INVALID_DMA;
    }

    draft_queue = composition->receive;
    ring_snapshot = *draft_queue.available.ring;
    posted_count = 0U;
    for (index = 0U; index < config->buffer_count; ++index) {
        const struct virtio_net_rx_buffer_descriptor *buffer =
            &config->buffers[index];
        struct virtio_descriptor_token descriptor_token;
        struct virtio_dma_region_token dma_token;
        struct virtio_dma_region dma_region;
        uint16_t chain[1];

        descriptor_status = virtio_descriptor_ownership_reserve(
            &draft_queue.descriptors, &descriptor_token);
        if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
            virtio_net_rx_rollback(
                &draft, &draft_queue, &ring_snapshot, posted_count);
            return VIRTIO_NET_RX_DESCRIPTOR_FAILED;
        }

        memset(&dma_request, 0, sizeof(dma_request));
        dma_request.physical_base = buffer->physical_base;
        dma_request.length = buffer->length;
        dma_request.direction = VIRTIO_NET_RX_DMA_DIRECTION;
        dma_request.lifetime = VIRTIO_NET_RX_DMA_LIFETIME;
        dma_request.zeroization = VIRTIO_NET_RX_DMA_ZEROIZATION;
        dma_status = virtio_dma_region_bind(
            &draft.dma,
            &dma_request,
            &dma_token,
            &dma_region);
        if (dma_status != VIRTIO_DMA_REGION_OK) {
            virtio_net_rx_release_descriptor(
                &draft_queue, &descriptor_token);
            virtio_net_rx_rollback(
                &draft, &draft_queue, &ring_snapshot, posted_count);
            return VIRTIO_NET_RX_DMA_BIND_FAILED;
        }

        descriptor_status = virtio_descriptor_ownership_publish(
            &draft_queue.descriptors, &descriptor_token);
        if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
            (void)virtio_dma_region_unbind(&draft.dma, &dma_token);
            virtio_net_rx_release_descriptor(
                &draft_queue, &descriptor_token);
            virtio_net_rx_rollback(
                &draft, &draft_queue, &ring_snapshot, posted_count);
            return VIRTIO_NET_RX_DESCRIPTOR_FAILED;
        }

        chain[0] = (uint16_t)descriptor_token.index;
        available_status = virtio_available_publication_prepare(
            &draft_queue.available,
            chain,
            1U,
            &available_token);
        if (available_status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
            (void)virtio_dma_region_unbind(&draft.dma, &dma_token);
            virtio_net_rx_release_descriptor(
                &draft_queue, &descriptor_token);
            virtio_net_rx_rollback(
                &draft, &draft_queue, &ring_snapshot, posted_count);
            return VIRTIO_NET_RX_AVAILABLE_FAILED;
        }
        available_status = virtio_available_publication_publish(
            &draft_queue.available,
            &available_token);
        if (available_status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
            (void)virtio_dma_region_unbind(&draft.dma, &dma_token);
            virtio_net_rx_release_descriptor(
                &draft_queue, &descriptor_token);
            virtio_net_rx_rollback(
                &draft, &draft_queue, &ring_snapshot, posted_count);
            return VIRTIO_NET_RX_AVAILABLE_FAILED;
        }

        draft.buffers[index].posted = true;
        draft.buffers[index].generation = 1U;
        draft.buffers[index].physical_base = buffer->physical_base;
        draft.buffers[index].length = buffer->length;
        draft.buffers[index].descriptor = descriptor_token;
        draft.buffers[index].dma = dma_token;
        ++posted_count;
    }

    draft.published = true;
    draft.version = VIRTIO_NET_RX_VERSION;
    draft.generation = composition->generation;
    draft.capacity = config->buffer_count;
    draft.posted_count = config->buffer_count;
    draft.minimum_frame_capacity = config->minimum_frame_capacity;
    draft.queue_size = composition->receive.queue_size;
    draft.composition = composition;

    composition->receive = draft_queue;
    *rx = draft;
    return VIRTIO_NET_RX_OK;
}

static void virtio_net_rx_reset_available_ring(
    struct virtio_available_ring *ring,
    uint32_t queue_size)
{
    uint16_t flags = ring->flags;
    uint16_t used_event = ring->used_event;
    uint32_t index;

    for (index = 0U; index < queue_size; ++index) {
        ring->ring[index] = 0U;
    }
    ring->idx = 0U;
    ring->flags = flags;
    ring->used_event = used_event;
}

enum virtio_net_rx_status virtio_net_rx_teardown(
    struct virtio_net_rx *rx)
{
    struct virtio_net_rx draft;
    struct virtio_net_queue draft_queue;
    struct virtio_available_publication fresh_available;
    struct virtio_available_ring *available_ring;
    struct virtio_available_barrier barrier;
    struct virtio_queue_layout_result layout;
    uint32_t generation;
    enum virtio_net_rx_status status;
    enum virtio_dma_region_status dma_status;
    uint32_t index;

    status = virtio_net_rx_controller_validate(rx);
    if (status != VIRTIO_NET_RX_OK) {
        return status;
    }

    draft = *rx;
    draft_queue = rx->composition->receive;
    for (index = draft.capacity; index > 0U; --index) {
        const struct virtio_net_rx_buffer *buffer =
            &draft.buffers[index - 1U];

        if (buffer->posted) {
            if (virtio_descriptor_ownership_consume(
                    &draft_queue.descriptors,
                    &buffer->descriptor) !=
                VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
                return VIRTIO_NET_RX_TEARDOWN_FAILED;
            }
        }
        if (virtio_dma_region_unbind(
                &draft.dma,
                &buffer->dma) != VIRTIO_DMA_REGION_OK) {
            return VIRTIO_NET_RX_TEARDOWN_FAILED;
        }
    }

    available_ring = draft_queue.available.ring;
    layout = draft_queue.layout;
    barrier = draft_queue.available.barrier;
    generation = draft_queue.available.generation;
    virtio_net_rx_reset_available_ring(
        available_ring, layout.queue_size);

    memset(&fresh_available, 0, sizeof(fresh_available));
    if (virtio_available_publication_init(
            &fresh_available,
            &layout,
            available_ring,
            &barrier,
            generation) != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return VIRTIO_NET_RX_TEARDOWN_FAILED;
    }
    fresh_available.ring = available_ring;

    dma_status = virtio_dma_region_teardown(&draft.dma);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        return VIRTIO_NET_RX_TEARDOWN_FAILED;
    }

    draft_queue.available = fresh_available;
    rx->composition->receive = draft_queue;
    memset(&draft, 0, sizeof(draft));
    *rx = draft;
    return VIRTIO_NET_RX_OK;
}

static bool virtio_net_rx_used_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct virtio_net_rx *rx = (struct virtio_net_rx *)context;
    struct virtio_net_queue *queue;
    uint32_t index;

    if (rx == NULL ||
        element == NULL ||
        token == NULL ||
        token->generation != rx->generation ||
        rx->posted_count == 0U) {
        return false;
    }
    queue = &rx->composition->receive;
    for (index = 0U; index < rx->capacity; ++index) {
        struct virtio_net_rx_buffer *buffer = &rx->buffers[index];

        if (!buffer->posted ||
            buffer->descriptor.index != element->id) {
            continue;
        }
        if (virtio_descriptor_ownership_consume(
                &queue->descriptors,
                &buffer->descriptor) != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
            return false;
        }
        buffer->posted = false;
        --rx->posted_count;
        return true;
    }
    return false;
}

static enum virtio_net_rx_status virtio_net_rx_consume_completion(
    struct virtio_net_rx *rx,
    const struct virtio_used_token *token)
{
    struct virtio_used_reclaim original_reclaim;
    enum virtio_used_consumption_status used_status;

    original_reclaim = rx->composition->receive.used.reclaim;
    rx->composition->receive.used.reclaim.reclaim =
        virtio_net_rx_used_reclaim;
    rx->composition->receive.used.reclaim.context = rx;
    used_status = virtio_used_consumption_consume(
        &rx->composition->receive.used, token);
    rx->composition->receive.used.reclaim = original_reclaim;
    if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
        return VIRTIO_NET_RX_RELEASE_FAILED;
    }
    return VIRTIO_NET_RX_OK;
}

enum virtio_net_rx_status virtio_net_rx_complete(
    struct virtio_net_rx *rx,
    const void *payload,
    struct virtio_net_rx_frame *frame)
{
    struct virtio_net_queue *queue;
    struct virtio_used_element entry;
    struct virtio_used_token token;
    enum virtio_used_consumption_status used_status;
    enum virtio_net_rx_status status;
    struct virtio_net_rx_buffer *buffer;
    const uint8_t *bytes;
    uint32_t header_size;
    uint32_t frame_length;
    uint32_t index;
    uint32_t buffer_index;
    uint32_t header_index;

    if (rx == NULL || payload == NULL || frame == NULL) {
        return VIRTIO_NET_RX_NULL;
    }
    status = virtio_net_rx_controller_validate(rx);
    if (status != VIRTIO_NET_RX_OK) {
        return status;
    }

    queue = &rx->composition->receive;
    used_status = virtio_used_consumption_prepare(
        &queue->used, &entry, &token);
    if (used_status == VIRTIO_USED_CONSUMPTION_EMPTY) {
        return VIRTIO_NET_RX_USED_EMPTY;
    }
    if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
        return VIRTIO_NET_RX_USED_INVALID;
    }

    buffer = NULL;
    for (index = 0U; index < rx->capacity; ++index) {
        struct virtio_net_rx_buffer *candidate = &rx->buffers[index];

        if (candidate->posted &&
            candidate->descriptor.index == entry.id) {
            buffer = candidate;
            buffer_index = index;
            break;
        }
    }
    if (buffer == NULL ||
        queue->descriptors.generations[buffer->descriptor.index] !=
            buffer->descriptor.generation ||
        queue->descriptors.states[buffer->descriptor.index] !=
            VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
        if (virtio_used_consumption_cancel(
                &queue->used, &token) != VIRTIO_USED_CONSUMPTION_OK) {
            return VIRTIO_NET_RX_USED_INVALID;
        }
        return VIRTIO_NET_RX_STALE_COMPLETION;
    }

    header_size = rx->composition->profile.header_size;
    bytes = (const uint8_t *)payload;
    if (entry.len < header_size + VIRTIO_NET_RX_ETHERNET_FRAME_MIN) {
        status = VIRTIO_NET_RX_SHORT_FRAME;
    } else if (entry.len > buffer->length ||
               entry.len > header_size +
                   VIRTIO_NET_RX_ETHERNET_FRAME_MAX) {
        status = VIRTIO_NET_RX_OVERSIZED_FRAME;
    } else {
        for (header_index = 0U;
             header_index < header_size;
             ++header_index) {
            if (bytes[header_index] != 0U) {
                status = VIRTIO_NET_RX_INVALID_HEADER;
                break;
            }
        }
        if (header_index == header_size) {
            status = VIRTIO_NET_RX_OK;
        }
    }

    if (virtio_net_rx_consume_completion(rx, &token) !=
        VIRTIO_NET_RX_OK) {
        return VIRTIO_NET_RX_RELEASE_FAILED;
    }
    if (status != VIRTIO_NET_RX_OK) {
        return status;
    }

    frame_length = entry.len - header_size;
    frame->data = bytes + header_size;
    frame->physical_base = buffer->physical_base;
    frame->length = frame_length;
    frame->buffer_index = buffer_index;
    frame->generation = rx->generation;
    return VIRTIO_NET_RX_OK;
}

bool virtio_net_rx_published(const struct virtio_net_rx *rx)
{
    return virtio_net_rx_controller_validate(rx) == VIRTIO_NET_RX_OK;
}

uint32_t virtio_net_rx_generation(const struct virtio_net_rx *rx)
{
    if (virtio_net_rx_controller_validate(rx) != VIRTIO_NET_RX_OK) {
        return 0U;
    }
    return rx->generation;
}

uint32_t virtio_net_rx_capacity(const struct virtio_net_rx *rx)
{
    if (virtio_net_rx_controller_validate(rx) != VIRTIO_NET_RX_OK) {
        return 0U;
    }
    return rx->capacity;
}

uint32_t virtio_net_rx_posted_count(const struct virtio_net_rx *rx)
{
    if (virtio_net_rx_controller_validate(rx) != VIRTIO_NET_RX_OK) {
        return 0U;
    }
    return rx->posted_count;
}

const struct virtio_net_rx_buffer *
virtio_net_rx_buffer_at(const struct virtio_net_rx *rx, uint32_t index)
{
    if (virtio_net_rx_controller_validate(rx) != VIRTIO_NET_RX_OK ||
        index >= rx->capacity) {
        return NULL;
    }
    if (!rx->buffers[index].posted) {
        return NULL;
    }
    return &rx->buffers[index];
}

const char *virtio_net_rx_status_string(
    enum virtio_net_rx_status status)
{
    switch (status) {
    case VIRTIO_NET_RX_OK:
        return "VIRTIO_NET_RX_OK";
    case VIRTIO_NET_RX_NULL:
        return "VIRTIO_NET_RX_NULL";
    case VIRTIO_NET_RX_INVALID_VERSION:
        return "VIRTIO_NET_RX_INVALID_VERSION";
    case VIRTIO_NET_RX_INVALID_GENERATION:
        return "VIRTIO_NET_RX_INVALID_GENERATION";
    case VIRTIO_NET_RX_ALREADY_PUBLISHED:
        return "VIRTIO_NET_RX_ALREADY_PUBLISHED";
    case VIRTIO_NET_RX_UNPUBLISHED:
        return "VIRTIO_NET_RX_UNPUBLISHED";
    case VIRTIO_NET_RX_INVALID_COMPOSITION:
        return "VIRTIO_NET_RX_INVALID_COMPOSITION";
    case VIRTIO_NET_RX_INVALID_CONFIG:
        return "VIRTIO_NET_RX_INVALID_CONFIG";
    case VIRTIO_NET_RX_INVALID_BUFFER_COUNT:
        return "VIRTIO_NET_RX_INVALID_BUFFER_COUNT";
    case VIRTIO_NET_RX_QUEUE_FULL:
        return "VIRTIO_NET_RX_QUEUE_FULL";
    case VIRTIO_NET_RX_DMA_SHORTAGE:
        return "VIRTIO_NET_RX_DMA_SHORTAGE";
    case VIRTIO_NET_RX_INVALID_FRAME_CAPACITY:
        return "VIRTIO_NET_RX_INVALID_FRAME_CAPACITY";
    case VIRTIO_NET_RX_INVALID_BUFFER:
        return "VIRTIO_NET_RX_INVALID_BUFFER";
    case VIRTIO_NET_RX_BUFFER_OVERFLOW:
        return "VIRTIO_NET_RX_BUFFER_OVERFLOW";
    case VIRTIO_NET_RX_BUFFER_UNALIGNED:
        return "VIRTIO_NET_RX_BUFFER_UNALIGNED";
    case VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW:
        return "VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW";
    case VIRTIO_NET_RX_BUFFER_ALIAS:
        return "VIRTIO_NET_RX_BUFFER_ALIAS";
    case VIRTIO_NET_RX_INVALID_DMA:
        return "VIRTIO_NET_RX_INVALID_DMA";
    case VIRTIO_NET_RX_DMA_BIND_FAILED:
        return "VIRTIO_NET_RX_DMA_BIND_FAILED";
    case VIRTIO_NET_RX_DESCRIPTOR_FAILED:
        return "VIRTIO_NET_RX_DESCRIPTOR_FAILED";
    case VIRTIO_NET_RX_AVAILABLE_FAILED:
        return "VIRTIO_NET_RX_AVAILABLE_FAILED";
    case VIRTIO_NET_RX_TEARDOWN_FAILED:
        return "VIRTIO_NET_RX_TEARDOWN_FAILED";
    case VIRTIO_NET_RX_INVALID_STATE:
        return "VIRTIO_NET_RX_INVALID_STATE";
    case VIRTIO_NET_RX_INVALID_HEADER:
        return "VIRTIO_NET_RX_INVALID_HEADER";
    case VIRTIO_NET_RX_SHORT_FRAME:
        return "VIRTIO_NET_RX_SHORT_FRAME";
    case VIRTIO_NET_RX_OVERSIZED_FRAME:
        return "VIRTIO_NET_RX_OVERSIZED_FRAME";
    case VIRTIO_NET_RX_STALE_COMPLETION:
        return "VIRTIO_NET_RX_STALE_COMPLETION";
    case VIRTIO_NET_RX_USED_EMPTY:
        return "VIRTIO_NET_RX_USED_EMPTY";
    case VIRTIO_NET_RX_USED_INVALID:
        return "VIRTIO_NET_RX_USED_INVALID";
    case VIRTIO_NET_RX_RELEASE_FAILED:
        return "VIRTIO_NET_RX_RELEASE_FAILED";
    default:
        return NULL;
    }
}
