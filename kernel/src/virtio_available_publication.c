#include "virtio_available_publication.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_available_publication_generation_valid(
    uint32_t generation)
{
    return generation >= VIRTIO_AVAILABLE_PUBLICATION_GENERATION_MIN &&
           generation <= VIRTIO_AVAILABLE_PUBLICATION_GENERATION_MAX;
}

static bool virtio_available_publication_queue_size_valid(
    uint32_t queue_size)
{
    return queue_size >= VIRTIO_QUEUE_SIZE_MIN &&
           queue_size <= VIRTIO_QUEUE_SIZE_MAX &&
           (queue_size & (queue_size - 1U)) == 0U;
}

static bool virtio_available_publication_barrier_valid(
    const struct virtio_available_barrier *barrier)
{
    return barrier != NULL &&
           barrier->fence != NULL &&
           barrier->context != NULL;
}

static enum virtio_available_publication_status
virtio_available_publication_controller_valid(
    const struct virtio_available_publication *controller)
{
    if (controller == NULL) {
        return VIRTIO_AVAILABLE_PUBLICATION_NULL;
    }
    if (!controller->published) {
        return VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED;
    }
    if (controller->version != VIRTIO_AVAILABLE_PUBLICATION_VERSION) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION;
    }
    if (!virtio_available_publication_generation_valid(
            controller->generation)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION;
    }
    if (!virtio_available_publication_queue_size_valid(
            controller->queue_size)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE;
    }
    if (controller->ring == NULL ||
        !virtio_available_publication_barrier_valid(
            &controller->barrier)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER;
    }
    if (controller->published_count > controller->queue_size ||
        controller->next_sequence == 0U) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE;
    }
    if (controller->pending) {
        if (controller->pending_length <
                VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MIN ||
            controller->pending_length >
                VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX) {
            return VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE;
        }
    }
    return VIRTIO_AVAILABLE_PUBLICATION_OK;
}

static bool virtio_available_publication_index_changed(
    const struct virtio_available_publication *controller)
{
    return controller->ring->idx != controller->expected_idx;
}

static uint32_t virtio_available_publication_next_sequence(
    uint32_t sequence)
{
    return sequence == UINT32_MAX ? 1U : sequence + 1U;
}

static void virtio_available_publication_clear_pending(
    struct virtio_available_publication *controller)
{
    controller->pending = false;
    controller->pending_sequence = 0U;
    controller->pending_length = 0U;
}

enum virtio_available_publication_status
virtio_available_publication_init(
    struct virtio_available_publication *controller,
    const struct virtio_queue_layout_result *layout,
    struct virtio_available_ring *ring,
    const struct virtio_available_barrier *barrier,
    uint32_t generation)
{
    if (controller == NULL ||
        layout == NULL ||
        ring == NULL ||
        barrier == NULL) {
        return VIRTIO_AVAILABLE_PUBLICATION_NULL;
    }
    if (controller->published) {
        return VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED;
    }
    if (controller->version != 0U) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION;
    }
    if (virtio_queue_layout_result_validate(layout) !=
        VIRTIO_QUEUE_LAYOUT_OK) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT;
    }
    if (!virtio_available_publication_queue_size_valid(
            layout->queue_size)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE;
    }
    if (!virtio_available_publication_barrier_valid(barrier)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER;
    }
    if (!virtio_available_publication_generation_valid(generation)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION;
    }

    controller->published = true;
    controller->version = VIRTIO_AVAILABLE_PUBLICATION_VERSION;
    controller->generation = generation;
    controller->queue_size = layout->queue_size;
    controller->event_index = layout->event_index;
    controller->expected_idx = ring->idx;
    controller->published_count = 0U;
    controller->next_sequence = 1U;
    controller->pending = false;
    controller->pending_sequence = 0U;
    controller->pending_length = 0U;
    controller->ring = ring;
    controller->barrier = *barrier;
    return VIRTIO_AVAILABLE_PUBLICATION_OK;
}

enum virtio_available_publication_status
virtio_available_publication_prepare(
    struct virtio_available_publication *controller,
    const uint16_t *chain,
    uint32_t chain_length,
    struct virtio_available_token *token)
{
    enum virtio_available_publication_status status;
    uint32_t index;
    uint32_t prior;

    if (controller == NULL || chain == NULL || token == NULL) {
        return VIRTIO_AVAILABLE_PUBLICATION_NULL;
    }
    status = virtio_available_publication_controller_valid(controller);
    if (status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return status;
    }
    if (controller->pending) {
        return VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL;
    }
    if (virtio_available_publication_index_changed(controller)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED;
    }
    if (chain_length < VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MIN ||
        chain_length > VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX) {
        return VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN;
    }
    if (chain_length >
        controller->queue_size - controller->published_count) {
        return VIRTIO_AVAILABLE_PUBLICATION_RING_FULL;
    }
    for (index = 0U; index < chain_length; ++index) {
        if (chain[index] >= controller->queue_size) {
            return VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN;
        }
        for (prior = 0U; prior < index; ++prior) {
            if (chain[prior] == chain[index]) {
                return VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR;
            }
        }
    }

    memcpy(
        controller->pending_chain,
        chain,
        chain_length * sizeof(controller->pending_chain[0]));
    controller->pending = true;
    controller->pending_sequence = controller->next_sequence;
    controller->pending_length = chain_length;
    token->sequence = controller->next_sequence;
    token->generation = controller->generation;
    controller->next_sequence =
        virtio_available_publication_next_sequence(
            controller->next_sequence);
    return VIRTIO_AVAILABLE_PUBLICATION_OK;
}

enum virtio_available_publication_status
virtio_available_publication_publish(
    struct virtio_available_publication *controller,
    const struct virtio_available_token *token)
{
    enum virtio_available_publication_status status;
    uint32_t head;
    uint16_t next_idx;

    if (controller == NULL || token == NULL) {
        return VIRTIO_AVAILABLE_PUBLICATION_NULL;
    }
    status = virtio_available_publication_controller_valid(controller);
    if (status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return status;
    }
    if (!controller->pending) {
        return VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING;
    }
    if (token->generation != controller->generation ||
        token->sequence != controller->pending_sequence) {
        return VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN;
    }
    if (virtio_available_publication_index_changed(controller)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED;
    }
    if (controller->pending_length >
        controller->queue_size - controller->published_count) {
        return VIRTIO_AVAILABLE_PUBLICATION_RING_FULL;
    }

    head = controller->expected_idx % controller->queue_size;
    controller->ring->ring[head] = controller->pending_chain[0];
    controller->barrier.fence(controller->barrier.context);

    next_idx = (uint16_t)(controller->expected_idx + 1U);
    controller->ring->idx = next_idx;
    controller->barrier.fence(controller->barrier.context);

    controller->expected_idx = next_idx;
    controller->published_count += 1U;
    virtio_available_publication_clear_pending(controller);
    return VIRTIO_AVAILABLE_PUBLICATION_OK;
}

enum virtio_available_publication_status
virtio_available_publication_cancel(
    struct virtio_available_publication *controller,
    const struct virtio_available_token *token)
{
    enum virtio_available_publication_status status;

    if (controller == NULL || token == NULL) {
        return VIRTIO_AVAILABLE_PUBLICATION_NULL;
    }
    status = virtio_available_publication_controller_valid(controller);
    if (status != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return status;
    }
    if (!controller->pending) {
        return VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING;
    }
    if (token->generation != controller->generation ||
        token->sequence != controller->pending_sequence) {
        return VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN;
    }
    if (virtio_available_publication_index_changed(controller)) {
        return VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED;
    }
    virtio_available_publication_clear_pending(controller);
    return VIRTIO_AVAILABLE_PUBLICATION_OK;
}

bool virtio_available_publication_published(
    const struct virtio_available_publication *controller)
{
    return virtio_available_publication_controller_valid(controller) ==
               VIRTIO_AVAILABLE_PUBLICATION_OK &&
           !virtio_available_publication_index_changed(controller);
}

uint32_t virtio_available_publication_visible_index(
    const struct virtio_available_publication *controller)
{
    if (virtio_available_publication_controller_valid(controller) !=
        VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return 0U;
    }
    return controller->ring->idx;
}

uint32_t virtio_available_publication_published_count(
    const struct virtio_available_publication *controller)
{
    if (virtio_available_publication_controller_valid(controller) !=
        VIRTIO_AVAILABLE_PUBLICATION_OK) {
        return 0U;
    }
    return controller->published_count;
}

uint32_t virtio_available_publication_pending_length(
    const struct virtio_available_publication *controller)
{
    if (virtio_available_publication_controller_valid(controller) !=
            VIRTIO_AVAILABLE_PUBLICATION_OK ||
        !controller->pending) {
        return 0U;
    }
    return controller->pending_length;
}

const char *virtio_available_publication_status_string(
    enum virtio_available_publication_status status)
{
    switch (status) {
    case VIRTIO_AVAILABLE_PUBLICATION_OK:
        return "VIRTIO_AVAILABLE_PUBLICATION_OK";
    case VIRTIO_AVAILABLE_PUBLICATION_NULL:
        return "VIRTIO_AVAILABLE_PUBLICATION_NULL";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION";
    case VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED:
        return "VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED";
    case VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED:
        return "VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN";
    case VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR:
        return "VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR";
    case VIRTIO_AVAILABLE_PUBLICATION_RING_FULL:
        return "VIRTIO_AVAILABLE_PUBLICATION_RING_FULL";
    case VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL:
        return "VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL";
    case VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING:
        return "VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING";
    case VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN:
        return "VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN";
    case VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED:
        return "VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED";
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE:
        return "VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE";
    default:
        return NULL;
    }
}
