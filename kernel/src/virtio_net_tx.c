#include "virtio_net_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_tx_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_TX_GENERATION_MIN &&
           generation <= VIRTIO_NET_TX_GENERATION_MAX;
}

static bool virtio_net_tx_capacity_valid(uint32_t capacity)
{
    return capacity >= VIRTIO_NET_TX_CAPACITY_MIN &&
           capacity <= VIRTIO_NET_TX_CAPACITY_MAX;
}

static bool virtio_net_tx_header_length_valid(uint32_t length)
{
    return length >= VIRTIO_NET_TX_HEADER_LENGTH_MIN &&
           length <= VIRTIO_NET_TX_HEADER_LENGTH_MAX;
}

static bool virtio_net_tx_frame_length_valid(uint32_t length)
{
    return length >= VIRTIO_NET_TX_FRAME_LENGTH_MIN &&
           length <= VIRTIO_NET_TX_FRAME_LENGTH_MAX;
}

static bool virtio_net_tx_dma_alignment_valid(uint32_t alignment)
{
    return alignment >= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MIN &&
           alignment <= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX &&
           (alignment & (alignment - 1U)) == 0U;
}

static bool virtio_net_tx_dma_window_valid(
    const struct virtio_dma_window_policy *window)
{
    return window != NULL &&
           window->length != 0U &&
           window->base <= UINT64_MAX - window->length &&
           virtio_net_tx_dma_alignment_valid(window->alignment) &&
           window->base % window->alignment == 0U;
}

static bool virtio_net_tx_zeroize_valid(
    const struct virtio_dma_zeroize *zeroize)
{
    return zeroize != NULL &&
           zeroize->zeroize != NULL &&
           zeroize->context != NULL;
}

static bool virtio_net_tx_ranges_overlap(
    uint64_t first_base,
    uint64_t first_end,
    uint64_t second_base,
    uint64_t second_end)
{
    return first_base < second_end && second_base < first_end;
}

static enum virtio_net_tx_status
virtio_net_tx_config_validate(
    const struct virtio_net_tx_config *config,
    const struct virtio_net_queue *queue,
    uint32_t header_size)
{
    uint64_t window_end;
    uint64_t queue_end;
    uint32_t index;

    if (config == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    if (config->headers == NULL) {
        return VIRTIO_NET_TX_INVALID_CONFIG;
    }
    if (!virtio_net_tx_capacity_valid(config->header_count)) {
        return VIRTIO_NET_TX_INVALID_HEADER_COUNT;
    }
    if (config->header_count > queue->queue_size / 2U) {
        return VIRTIO_NET_TX_QUEUE_FULL;
    }
    if (!virtio_net_tx_frame_length_valid(
            config->maximum_frame_length)) {
        return VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH;
    }
    if (config->dma_capacity < config->header_count * 2U) {
        return VIRTIO_NET_TX_DMA_SHORTAGE;
    }
    if (!virtio_net_tx_dma_window_valid(&config->dma_window) ||
        !virtio_net_tx_zeroize_valid(&config->dma_zeroize)) {
        return VIRTIO_NET_TX_INVALID_DMA;
    }

    window_end = config->dma_window.base + config->dma_window.length;
    queue_end = queue->layout.region_base + queue->layout.total_size;
    for (index = 0U; index < config->header_count; ++index) {
        const struct virtio_net_tx_header_descriptor *header =
            &config->headers[index];
        uint64_t header_end;
        uint32_t prior;

        if (!virtio_net_tx_header_length_valid(header->length) ||
            header->length < header_size ||
            header->alias == NULL) {
            return VIRTIO_NET_TX_INVALID_HEADER;
        }
        if (header->physical_base > UINT64_MAX - header->length) {
            return VIRTIO_NET_TX_HEADER_OVERFLOW;
        }
        if (header->physical_base % config->dma_window.alignment != 0U) {
            return VIRTIO_NET_TX_HEADER_UNALIGNED;
        }

        header_end = header->physical_base + header->length;
        if (header->physical_base < config->dma_window.base ||
            header_end > window_end) {
            return VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW;
        }
        if (virtio_net_tx_ranges_overlap(
                header->physical_base,
                header_end,
                queue->layout.region_base,
                queue_end)) {
            return VIRTIO_NET_TX_HEADER_ALIAS;
        }
        for (prior = 0U; prior < index; ++prior) {
            const struct virtio_net_tx_header_descriptor *previous =
                &config->headers[prior];
            uint64_t previous_end =
                previous->physical_base + previous->length;

            if (virtio_net_tx_ranges_overlap(
                    header->physical_base,
                    header_end,
                    previous->physical_base,
                    previous_end)) {
                return VIRTIO_NET_TX_HEADER_ALIAS;
            }
        }
    }
    return VIRTIO_NET_TX_OK;
}

static enum virtio_net_tx_status
virtio_net_tx_composition_validate(
    const struct virtio_net_queue_composition *composition)
{
    const struct virtio_net_queue *queue;

    if (composition == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    if (!composition->published ||
        composition->version != VIRTIO_NET_QUEUE_COMPOSITION_VERSION ||
        !virtio_net_tx_generation_valid(composition->generation) ||
        !composition->profile.published) {
        return VIRTIO_NET_TX_INVALID_COMPOSITION;
    }

    queue = &composition->transmit;
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
        return VIRTIO_NET_TX_INVALID_COMPOSITION;
    }
    return VIRTIO_NET_TX_OK;
}

static enum virtio_net_tx_status
virtio_net_tx_controller_validate(
    const struct virtio_net_tx *tx)
{
    const struct virtio_net_queue *queue;
    uint32_t active_seen;
    uint32_t index;

    if (tx == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    if (!tx->published) {
        return VIRTIO_NET_TX_UNPUBLISHED;
    }
    if (tx->version != VIRTIO_NET_TX_VERSION) {
        return VIRTIO_NET_TX_INVALID_VERSION;
    }
    if (!virtio_net_tx_generation_valid(tx->generation) ||
        !virtio_net_tx_capacity_valid(tx->capacity) ||
        tx->submitted_count > tx->capacity ||
        tx->reclaimed_count > tx->capacity ||
        !virtio_net_tx_frame_length_valid(tx->maximum_frame_length) ||
        tx->composition == NULL ||
        !virtio_net_queue_composition_published(tx->composition) ||
        tx->notify.notify == NULL ||
        tx->notify.context == NULL) {
        return VIRTIO_NET_TX_INVALID_STATE;
    }

    queue = &tx->composition->transmit;
    if (!virtio_descriptor_ownership_published(&queue->descriptors) ||
        !virtio_available_publication_published(&queue->available) ||
        virtio_available_publication_published_count(
            &queue->available) !=
            (tx->submitted_count + tx->reclaimed_count) ||
        !virtio_used_consumption_published(&queue->used) ||
        queue->used.generation != tx->generation ||
        !virtio_dma_region_published(&tx->dma) ||
        virtio_dma_region_bound_count(&tx->dma) !=
            tx->submitted_count * 2U) {
        return VIRTIO_NET_TX_INVALID_STATE;
    }

    active_seen = 0U;
    for (index = 0U; index < tx->capacity; ++index) {
        const struct virtio_net_tx_slot *slot = &tx->slots[index];
        const struct virtio_net_tx_header_descriptor *header =
            &tx->headers[index];

        if (header->alias == NULL ||
            !virtio_net_tx_header_length_valid(header->length) ||
            header->length < tx->composition->profile.header_size) {
            return VIRTIO_NET_TX_INVALID_STATE;
        }
        if (!slot->active) {
            if (slot->frame_data != NULL ||
                slot->frame_physical_base != 0U ||
                slot->frame_length != 0U) {
                return VIRTIO_NET_TX_INVALID_STATE;
            }
            continue;
        }
        ++active_seen;
        if (slot->generation != tx->generation ||
            slot->frame_data == NULL ||
            !virtio_net_tx_frame_length_valid(slot->frame_length) ||
            slot->frame_length > tx->maximum_frame_length ||
            slot->frame_physical_base > UINT64_MAX - slot->frame_length ||
            slot->frame_physical_base % tx->dma.window.alignment != 0U ||
            slot->header_descriptor.index >= queue->queue_size ||
            slot->frame_descriptor.index >= queue->queue_size ||
            queue->descriptors.generations[
                slot->header_descriptor.index] !=
                slot->header_descriptor.generation ||
            queue->descriptors.states[
                slot->header_descriptor.index] !=
                VIRTIO_DESCRIPTOR_STATE_PUBLISHED ||
            queue->descriptors.generations[
                slot->frame_descriptor.index] !=
                slot->frame_descriptor.generation ||
            queue->descriptors.states[
                slot->frame_descriptor.index] !=
                VIRTIO_DESCRIPTOR_STATE_PUBLISHED ||
            slot->header_dma.index >= tx->dma.capacity ||
            slot->frame_dma.index >= tx->dma.capacity ||
            !tx->dma.slots[slot->header_dma.index].bound ||
            tx->dma.slots[slot->header_dma.index].generation !=
                slot->header_dma.generation ||
            !tx->dma.slots[slot->frame_dma.index].bound ||
            tx->dma.slots[slot->frame_dma.index].generation !=
                slot->frame_dma.generation) {
            return VIRTIO_NET_TX_INVALID_STATE;
        }
    }
    if (active_seen != tx->submitted_count) {
        return VIRTIO_NET_TX_INVALID_STATE;
    }
    return VIRTIO_NET_TX_OK;
}

static void virtio_net_tx_release_descriptor(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token)
{
    if (pool->states[token->index] ==
        VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
        (void)virtio_descriptor_ownership_consume(
            pool, token);
    } else {
        (void)virtio_descriptor_ownership_rollback(
            pool, token);
    }
}

struct virtio_net_tx_reclaim_context {
    struct virtio_net_tx *tx;
    struct virtio_net_queue *queue;
    struct virtio_net_tx_completion *completion;
};

static struct virtio_net_tx_slot *virtio_net_tx_find_active_slot(
    struct virtio_net_tx *tx,
    uint32_t head)
{
    uint32_t index;

    for (index = 0U; index < tx->capacity; ++index) {
        struct virtio_net_tx_slot *slot = &tx->slots[index];

        if (slot->active &&
            slot->header_descriptor.index == head) {
            return slot;
        }
    }
    return NULL;
}

static bool virtio_net_tx_reclaim_used(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct virtio_net_tx_reclaim_context *reclaim =
        (struct virtio_net_tx_reclaim_context *)context;
    struct virtio_net_tx_slot *slot;

    if (reclaim == NULL ||
        element == NULL ||
        token == NULL ||
        reclaim->tx == NULL ||
        reclaim->queue == NULL ||
        reclaim->completion == NULL ||
        token->generation != reclaim->tx->generation) {
        return false;
    }
    slot = virtio_net_tx_find_active_slot(reclaim->tx, element->id);
    if (slot == NULL) {
        return false;
    }
    if (virtio_descriptor_ownership_consume(
            &reclaim->queue->descriptors,
            &slot->header_descriptor) !=
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK ||
        virtio_descriptor_ownership_consume(
            &reclaim->queue->descriptors,
            &slot->frame_descriptor) !=
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK ||
        virtio_dma_region_unbind(
            &reclaim->tx->dma,
            &slot->header_dma) != VIRTIO_DMA_REGION_OK ||
        virtio_dma_region_unbind(
            &reclaim->tx->dma,
            &slot->frame_dma) != VIRTIO_DMA_REGION_OK) {
        return false;
    }

    reclaim->completion->data = slot->frame_data;
    reclaim->completion->physical_base = slot->frame_physical_base;
    reclaim->completion->length = slot->frame_length;
    reclaim->completion->slot_index = (uint32_t)(
        slot - reclaim->tx->slots);
    reclaim->completion->generation = reclaim->tx->generation;
    memset(slot, 0, sizeof(*slot));
    --reclaim->tx->submitted_count;
    ++reclaim->tx->reclaimed_count;
    return true;
}

static bool virtio_net_tx_invalid_length_used(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    uint32_t *counter = (uint32_t *)context;

    if (counter == NULL || element == NULL || token == NULL) {
        return false;
    }
    ++*counter;
    return true;
}

static bool virtio_net_tx_stale_used(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    uint32_t *counter = (uint32_t *)context;

    if (counter == NULL || element == NULL || token == NULL) {
        return false;
    }
    ++*counter;
    return true;
}

enum virtio_net_tx_status virtio_net_tx_activate(
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition,
    const struct virtio_net_tx_config *config,
    const struct virtio_net_tx_notify *notify)
{
    struct virtio_net_tx draft;
    enum virtio_net_tx_status status;
    enum virtio_dma_region_status dma_status;
    uint32_t index;

    if (tx == NULL || composition == NULL || config == NULL ||
        notify == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    if (tx->published) {
        return VIRTIO_NET_TX_ALREADY_PUBLISHED;
    }
    if (tx->version != 0U) {
        return VIRTIO_NET_TX_INVALID_VERSION;
    }
    if (notify->notify == NULL || notify->context == NULL) {
        return VIRTIO_NET_TX_INVALID_CONFIG;
    }
    status = virtio_net_tx_composition_validate(composition);
    if (status != VIRTIO_NET_TX_OK) {
        return status;
    }
    status = virtio_net_tx_config_validate(
        config,
        &composition->transmit,
        composition->profile.header_size);
    if (status != VIRTIO_NET_TX_OK) {
        return status;
    }

    memset(&draft, 0, sizeof(draft));
    dma_status = virtio_dma_region_init(
        &draft.dma,
        &config->dma_window,
        &config->dma_zeroize,
        config->dma_capacity);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        return VIRTIO_NET_TX_INVALID_DMA;
    }

    draft.published = true;
    draft.version = VIRTIO_NET_TX_VERSION;
    draft.generation = composition->generation;
    draft.capacity = config->header_count;
    draft.maximum_frame_length = config->maximum_frame_length;
    draft.queue_size = composition->transmit.queue_size;
    draft.composition = composition;
    draft.notify = *notify;
    for (index = 0U; index < config->header_count; ++index) {
        draft.headers[index] = config->headers[index];
    }

    *tx = draft;
    return VIRTIO_NET_TX_OK;
}

static void virtio_net_tx_reset_available_ring(
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

enum virtio_net_tx_status virtio_net_tx_teardown(
    struct virtio_net_tx *tx)
{
    struct virtio_net_tx draft;
    struct virtio_net_queue draft_queue;
    struct virtio_available_publication fresh_available;
    struct virtio_available_ring *available_ring;
    struct virtio_available_barrier barrier;
    struct virtio_queue_layout_result layout;
    uint32_t generation;
    enum virtio_net_tx_status status;
    enum virtio_dma_region_status dma_status;
    uint32_t index;

    status = virtio_net_tx_controller_validate(tx);
    if (status != VIRTIO_NET_TX_OK) {
        return status;
    }

    draft = *tx;
    draft_queue = tx->composition->transmit;
    for (index = draft.capacity; index > 0U; --index) {
        const struct virtio_net_tx_slot *slot = &draft.slots[index - 1U];

        if (!slot->active) {
            continue;
        }
        if (virtio_descriptor_ownership_consume(
                &draft_queue.descriptors,
                &slot->header_descriptor) !=
                VIRTIO_DESCRIPTOR_OWNERSHIP_OK ||
            virtio_descriptor_ownership_consume(
                &draft_queue.descriptors,
                &slot->frame_descriptor) !=
                VIRTIO_DESCRIPTOR_OWNERSHIP_OK ||
            virtio_dma_region_unbind(
                &draft.dma,
                &slot->header_dma) != VIRTIO_DMA_REGION_OK ||
            virtio_dma_region_unbind(
                &draft.dma,
                &slot->frame_dma) != VIRTIO_DMA_REGION_OK) {
            return VIRTIO_NET_TX_TEARDOWN_FAILED;
        }
    }

    available_ring = draft_queue.available.ring;
    layout = draft_queue.layout;
    barrier = draft_queue.available.barrier;
    generation = draft_queue.available.generation;
    virtio_net_tx_reset_available_ring(
        available_ring, layout.queue_size);

    memset(&fresh_available, 0, sizeof(fresh_available));
    if (virtio_available_publication_init(
            &fresh_available,
            &layout,
            available_ring,
            &barrier,
            generation) != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return VIRTIO_NET_TX_TEARDOWN_FAILED;
    }
    fresh_available.ring = available_ring;

    dma_status = virtio_dma_region_teardown(&draft.dma);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        return VIRTIO_NET_TX_TEARDOWN_FAILED;
    }

    draft_queue.available = fresh_available;
    tx->composition->transmit = draft_queue;
    memset(&draft, 0, sizeof(draft));
    *tx = draft;
    return VIRTIO_NET_TX_OK;
}

enum virtio_net_tx_status virtio_net_tx_submit(
    struct virtio_net_tx *tx,
    const struct virtio_net_tx_frame *frame,
    struct virtio_net_tx_submission *submission)
{
    struct virtio_net_queue *queue;
    struct virtio_net_tx_slot *slot;
    const struct virtio_net_tx_header_descriptor *header;
    struct virtio_descriptor_token header_descriptor;
    struct virtio_descriptor_token frame_descriptor;
    struct virtio_dma_region_token header_dma;
    struct virtio_dma_region_token frame_dma;
    struct virtio_dma_region ignored;
    struct virtio_dma_region_request request;
    struct virtio_available_token available_token;
    enum virtio_net_tx_status status;
    enum virtio_descriptor_ownership_status descriptor_status;
    enum virtio_dma_region_status dma_status;
    enum virtio_available_publication_status available_status;
    struct virtio_net_tx_submission prepared;
    uint16_t chain[2];
    uint32_t slot_index;
    uint32_t header_size;

    if (tx == NULL || frame == NULL || submission == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    status = virtio_net_tx_controller_validate(tx);
    if (status != VIRTIO_NET_TX_OK) {
        return status;
    }
    if (frame->data == NULL ||
        !virtio_net_tx_frame_length_valid(frame->length) ||
        frame->length > tx->maximum_frame_length ||
        frame->physical_base > UINT64_MAX - frame->length ||
        frame->physical_base % tx->dma.window.alignment != 0U) {
        return VIRTIO_NET_TX_INVALID_FRAME;
    }
    if (tx->submitted_count >= tx->capacity) {
        return VIRTIO_NET_TX_QUEUE_FULL;
    }

    queue = &tx->composition->transmit;
    slot_index = tx->submitted_count;
    slot = &tx->slots[slot_index];
    header = &tx->headers[slot_index];
    header_size = tx->composition->profile.header_size;

    descriptor_status = virtio_descriptor_ownership_reserve(
        &queue->descriptors, &header_descriptor);
    if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        return VIRTIO_NET_TX_DESCRIPTOR_FAILED;
    }
    descriptor_status = virtio_descriptor_ownership_reserve(
        &queue->descriptors, &frame_descriptor);
    if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        return VIRTIO_NET_TX_DESCRIPTOR_FAILED;
    }

    memset(&request, 0, sizeof(request));
    request.physical_base = header->physical_base;
    request.length = header->length;
    request.direction = VIRTIO_NET_TX_DMA_DIRECTION;
    request.lifetime = VIRTIO_NET_TX_DMA_LIFETIME;
    request.zeroization = VIRTIO_NET_TX_DMA_ZEROIZATION;
    dma_status = virtio_dma_region_bind(
        &tx->dma, &request, &header_dma, &ignored);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_INVALID_DMA;
    }

    memset(&request, 0, sizeof(request));
    request.physical_base = frame->physical_base;
    request.length = frame->length;
    request.direction = VIRTIO_NET_TX_DMA_DIRECTION;
    request.lifetime = VIRTIO_NET_TX_DMA_LIFETIME;
    request.zeroization = VIRTIO_NET_TX_DMA_ZEROIZATION;
    dma_status = virtio_dma_region_bind(
        &tx->dma, &request, &frame_dma, &ignored);
    if (dma_status != VIRTIO_DMA_REGION_OK) {
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_INVALID_DMA;
    }

    descriptor_status = virtio_descriptor_ownership_publish(
        &queue->descriptors, &header_descriptor);
    if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        (void)virtio_dma_region_unbind(&tx->dma, &frame_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_DESCRIPTOR_FAILED;
    }
    descriptor_status = virtio_descriptor_ownership_publish(
        &queue->descriptors, &frame_descriptor);
    if (descriptor_status != VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        (void)virtio_dma_region_unbind(&tx->dma, &frame_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_DESCRIPTOR_FAILED;
    }

    memset(header->alias, 0, header_size);
    chain[0] = (uint16_t)header_descriptor.index;
    chain[1] = (uint16_t)frame_descriptor.index;
    available_status = virtio_available_publication_prepare(
        &queue->available, chain, 2U, &available_token);
    if (available_status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        (void)virtio_dma_region_unbind(&tx->dma, &frame_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_AVAILABLE_FAILED;
    }

    prepared.header_descriptor_index = header_descriptor.index;
    prepared.frame_descriptor_index = frame_descriptor.index;
    prepared.slot_index = slot_index;
    prepared.generation = tx->generation;
    if (!tx->notify.notify(tx->notify.context, &prepared)) {
        (void)virtio_available_publication_cancel(
            &queue->available, &available_token);
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        (void)virtio_dma_region_unbind(&tx->dma, &frame_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_NOTIFY_FAILED;
    }

    available_status = virtio_available_publication_publish(
        &queue->available, &available_token);
    if (available_status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        (void)virtio_dma_region_unbind(&tx->dma, &header_dma);
        (void)virtio_dma_region_unbind(&tx->dma, &frame_dma);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &header_descriptor);
        virtio_net_tx_release_descriptor(
            &queue->descriptors, &frame_descriptor);
        return VIRTIO_NET_TX_AVAILABLE_FAILED;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = tx->generation;
    slot->frame_data = frame->data;
    slot->frame_physical_base = frame->physical_base;
    slot->frame_length = frame->length;
    slot->header_descriptor = header_descriptor;
    slot->frame_descriptor = frame_descriptor;
    slot->header_dma = header_dma;
    slot->frame_dma = frame_dma;
    ++tx->submitted_count;
    *submission = prepared;
    return VIRTIO_NET_TX_OK;
}

enum virtio_net_tx_status virtio_net_tx_reclaim(
    struct virtio_net_tx *tx,
    struct virtio_net_tx_completion *completion)
{
    struct virtio_net_tx draft;
    struct virtio_net_queue draft_queue;
    struct virtio_used_reclaim original_reclaim;
    struct virtio_net_tx_reclaim_context reclaim;
    struct virtio_used_element entry;
    struct virtio_used_token token;
    struct virtio_net_tx_slot *slot;
    enum virtio_used_consumption_status used_status;
    enum virtio_net_tx_status status;

    if (tx == NULL || completion == NULL) {
        return VIRTIO_NET_TX_NULL;
    }
    status = virtio_net_tx_controller_validate(tx);
    if (status != VIRTIO_NET_TX_OK) {
        return status;
    }

    draft = *tx;
    draft_queue = tx->composition->transmit;
    used_status = virtio_used_consumption_prepare(
        &draft_queue.used, &entry, &token);
    if (used_status == VIRTIO_USED_CONSUMPTION_EMPTY) {
        return VIRTIO_NET_TX_USED_EMPTY;
    }
    if (used_status == VIRTIO_USED_CONSUMPTION_INVALID_HEAD) {
        ++draft.invalid_head_count;
        *tx = draft;
        return VIRTIO_NET_TX_INVALID_HEAD;
    }
    if (used_status == VIRTIO_USED_CONSUMPTION_INVALID_LENGTH) {
        ++draft.invalid_length_count;
        *tx = draft;
        return VIRTIO_NET_TX_INVALID_LENGTH;
    }
    if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
        return VIRTIO_NET_TX_USED_INVALID;
    }

    original_reclaim = draft_queue.used.reclaim;
    slot = virtio_net_tx_find_active_slot(&draft, entry.id);
    if (slot == NULL) {
        draft_queue.used.reclaim.reclaim = virtio_net_tx_stale_used;
        draft_queue.used.reclaim.context =
            &draft.stale_completion_count;
        used_status = virtio_used_consumption_consume(
            &draft_queue.used, &token);
        draft_queue.used.reclaim = original_reclaim;
        if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
            return VIRTIO_NET_TX_USED_INVALID;
        }
        tx->composition->transmit = draft_queue;
        *tx = draft;
        return VIRTIO_NET_TX_STALE_COMPLETION;
    }

    if (entry.len != 0U) {
        draft_queue.used.reclaim.reclaim =
            virtio_net_tx_invalid_length_used;
        draft_queue.used.reclaim.context =
            &draft.invalid_length_count;
        used_status = virtio_used_consumption_consume(
            &draft_queue.used, &token);
        draft_queue.used.reclaim = original_reclaim;
        if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
            return VIRTIO_NET_TX_USED_INVALID;
        }
        tx->composition->transmit = draft_queue;
        *tx = draft;
        return VIRTIO_NET_TX_INVALID_LENGTH;
    }

    reclaim.tx = &draft;
    reclaim.queue = &draft_queue;
    reclaim.completion = completion;
    draft_queue.used.reclaim.reclaim = virtio_net_tx_reclaim_used;
    draft_queue.used.reclaim.context = &reclaim;
    used_status = virtio_used_consumption_consume(
        &draft_queue.used, &token);
    draft_queue.used.reclaim = original_reclaim;
    if (used_status == VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED) {
        if (virtio_used_consumption_cancel(
                &draft_queue.used, &token) !=
            VIRTIO_USED_CONSUMPTION_OK) {
            return VIRTIO_NET_TX_USED_INVALID;
        }
        return VIRTIO_NET_TX_RELEASE_FAILED;
    }
    if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
        return VIRTIO_NET_TX_USED_INVALID;
    }

    tx->composition->transmit = draft_queue;
    *tx = draft;
    return VIRTIO_NET_TX_OK;
}

bool virtio_net_tx_published(const struct virtio_net_tx *tx)
{
    return virtio_net_tx_controller_validate(tx) == VIRTIO_NET_TX_OK;
}

uint32_t virtio_net_tx_generation(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->generation;
}

uint32_t virtio_net_tx_capacity(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->capacity;
}

uint32_t virtio_net_tx_submitted_count(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->submitted_count;
}

uint32_t virtio_net_tx_reclaimed_count(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->reclaimed_count;
}

uint32_t virtio_net_tx_invalid_head_count(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->invalid_head_count;
}

uint32_t virtio_net_tx_invalid_length_count(const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->invalid_length_count;
}

uint32_t virtio_net_tx_stale_completion_count(
    const struct virtio_net_tx *tx)
{
    if (virtio_net_tx_controller_validate(tx) != VIRTIO_NET_TX_OK) {
        return 0U;
    }
    return tx->stale_completion_count;
}

const char *virtio_net_tx_status_string(
    enum virtio_net_tx_status status)
{
    switch (status) {
    case VIRTIO_NET_TX_OK:
        return "VIRTIO_NET_TX_OK";
    case VIRTIO_NET_TX_NULL:
        return "VIRTIO_NET_TX_NULL";
    case VIRTIO_NET_TX_INVALID_VERSION:
        return "VIRTIO_NET_TX_INVALID_VERSION";
    case VIRTIO_NET_TX_INVALID_GENERATION:
        return "VIRTIO_NET_TX_INVALID_GENERATION";
    case VIRTIO_NET_TX_ALREADY_PUBLISHED:
        return "VIRTIO_NET_TX_ALREADY_PUBLISHED";
    case VIRTIO_NET_TX_UNPUBLISHED:
        return "VIRTIO_NET_TX_UNPUBLISHED";
    case VIRTIO_NET_TX_INVALID_COMPOSITION:
        return "VIRTIO_NET_TX_INVALID_COMPOSITION";
    case VIRTIO_NET_TX_INVALID_CONFIG:
        return "VIRTIO_NET_TX_INVALID_CONFIG";
    case VIRTIO_NET_TX_INVALID_HEADER_COUNT:
        return "VIRTIO_NET_TX_INVALID_HEADER_COUNT";
    case VIRTIO_NET_TX_QUEUE_FULL:
        return "VIRTIO_NET_TX_QUEUE_FULL";
    case VIRTIO_NET_TX_DMA_SHORTAGE:
        return "VIRTIO_NET_TX_DMA_SHORTAGE";
    case VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH:
        return "VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH";
    case VIRTIO_NET_TX_INVALID_HEADER:
        return "VIRTIO_NET_TX_INVALID_HEADER";
    case VIRTIO_NET_TX_HEADER_OVERFLOW:
        return "VIRTIO_NET_TX_HEADER_OVERFLOW";
    case VIRTIO_NET_TX_HEADER_UNALIGNED:
        return "VIRTIO_NET_TX_HEADER_UNALIGNED";
    case VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW:
        return "VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW";
    case VIRTIO_NET_TX_HEADER_ALIAS:
        return "VIRTIO_NET_TX_HEADER_ALIAS";
    case VIRTIO_NET_TX_INVALID_DMA:
        return "VIRTIO_NET_TX_INVALID_DMA";
    case VIRTIO_NET_TX_DESCRIPTOR_FAILED:
        return "VIRTIO_NET_TX_DESCRIPTOR_FAILED";
    case VIRTIO_NET_TX_AVAILABLE_FAILED:
        return "VIRTIO_NET_TX_AVAILABLE_FAILED";
    case VIRTIO_NET_TX_NOTIFY_FAILED:
        return "VIRTIO_NET_TX_NOTIFY_FAILED";
    case VIRTIO_NET_TX_INVALID_FRAME:
        return "VIRTIO_NET_TX_INVALID_FRAME";
    case VIRTIO_NET_TX_TEARDOWN_FAILED:
        return "VIRTIO_NET_TX_TEARDOWN_FAILED";
    case VIRTIO_NET_TX_INVALID_STATE:
        return "VIRTIO_NET_TX_INVALID_STATE";
    case VIRTIO_NET_TX_USED_EMPTY:
        return "VIRTIO_NET_TX_USED_EMPTY";
    case VIRTIO_NET_TX_INVALID_HEAD:
        return "VIRTIO_NET_TX_INVALID_HEAD";
    case VIRTIO_NET_TX_INVALID_LENGTH:
        return "VIRTIO_NET_TX_INVALID_LENGTH";
    case VIRTIO_NET_TX_STALE_COMPLETION:
        return "VIRTIO_NET_TX_STALE_COMPLETION";
    case VIRTIO_NET_TX_RELEASE_FAILED:
        return "VIRTIO_NET_TX_RELEASE_FAILED";
    case VIRTIO_NET_TX_USED_INVALID:
        return "VIRTIO_NET_TX_USED_INVALID";
    default:
        return NULL;
    }
}
