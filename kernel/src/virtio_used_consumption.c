#include "virtio_used_consumption.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool virtio_used_consumption_generation_valid(
    uint32_t generation)
{
    return generation >= VIRTIO_USED_CONSUMPTION_GENERATION_MIN &&
           generation <= VIRTIO_USED_CONSUMPTION_GENERATION_MAX;
}

static bool virtio_used_consumption_queue_size_valid(
    uint32_t queue_size)
{
    return queue_size >= VIRTIO_QUEUE_SIZE_MIN &&
           queue_size <= VIRTIO_QUEUE_SIZE_MAX &&
           (queue_size & (queue_size - 1U)) == 0U;
}

static uint32_t virtio_used_consumption_distance(
    uint16_t producer_idx,
    uint32_t consumer_idx)
{
    return (uint32_t)(uint16_t)(
        producer_idx - (uint16_t)consumer_idx);
}

static enum virtio_used_consumption_status
virtio_used_consumption_controller_valid(
    const struct virtio_used_consumption *controller)
{
    if (controller == NULL) {
        return VIRTIO_USED_CONSUMPTION_NULL;
    }
    if (!controller->published) {
        return VIRTIO_USED_CONSUMPTION_UNPUBLISHED;
    }
    if (controller->version != VIRTIO_USED_CONSUMPTION_VERSION) {
        return VIRTIO_USED_CONSUMPTION_INVALID_VERSION;
    }
    if (!virtio_used_consumption_generation_valid(
            controller->generation)) {
        return VIRTIO_USED_CONSUMPTION_INVALID_GENERATION;
    }
    if (!virtio_used_consumption_queue_size_valid(
            controller->queue_size)) {
        return VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE;
    }
    if (controller->max_length >
        VIRTIO_USED_CONSUMPTION_LENGTH_MAX) {
        return VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH;
    }
    if (controller->ring == NULL) {
        return VIRTIO_USED_CONSUMPTION_INVALID_RING;
    }
    if (controller->reclaim.reclaim == NULL ||
        controller->reclaim.context == NULL) {
        return VIRTIO_USED_CONSUMPTION_INVALID_RELEASE;
    }
    if (controller->expected_idx > UINT16_MAX ||
        controller->consumed_count > controller->queue_size ||
        controller->next_sequence == 0U) {
        return VIRTIO_USED_CONSUMPTION_INVALID_STATE;
    }
    if (controller->pending) {
        if (controller->pending_sequence == 0U ||
            controller->pending_element.id >= controller->queue_size ||
            controller->pending_element.len > controller->max_length) {
            return VIRTIO_USED_CONSUMPTION_INVALID_STATE;
        }
    }
    return VIRTIO_USED_CONSUMPTION_OK;
}

static uint32_t virtio_used_consumption_next_sequence(
    uint32_t sequence)
{
    return sequence == UINT32_MAX ? 1U : sequence + 1U;
}

static void virtio_used_consumption_clear_pending(
    struct virtio_used_consumption *controller)
{
    controller->pending = false;
    controller->pending_sequence = 0U;
    controller->pending_element.id = 0U;
    controller->pending_element.len = 0U;
}

enum virtio_used_consumption_status
virtio_used_consumption_init(
    struct virtio_used_consumption *controller,
    const struct virtio_queue_layout_result *layout,
    struct virtio_used_ring *ring,
    const struct virtio_used_reclaim *reclaim,
    uint32_t generation,
    uint32_t max_length)
{
    if (controller == NULL ||
        layout == NULL ||
        ring == NULL ||
        reclaim == NULL) {
        return VIRTIO_USED_CONSUMPTION_NULL;
    }
    if (controller->published) {
        return VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED;
    }
    if (controller->version != 0U) {
        return VIRTIO_USED_CONSUMPTION_INVALID_VERSION;
    }
    if (virtio_queue_layout_result_validate(layout) !=
        VIRTIO_QUEUE_LAYOUT_OK) {
        return VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT;
    }
    if (!virtio_used_consumption_queue_size_valid(
            layout->queue_size)) {
        return VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE;
    }
    if (max_length > VIRTIO_USED_CONSUMPTION_LENGTH_MAX) {
        return VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH;
    }
    if (reclaim->reclaim == NULL || reclaim->context == NULL) {
        return VIRTIO_USED_CONSUMPTION_INVALID_RELEASE;
    }
    if (!virtio_used_consumption_generation_valid(generation)) {
        return VIRTIO_USED_CONSUMPTION_INVALID_GENERATION;
    }

    controller->published = true;
    controller->version = VIRTIO_USED_CONSUMPTION_VERSION;
    controller->generation = generation;
    controller->queue_size = layout->queue_size;
    controller->event_index = layout->event_index;
    controller->max_length = max_length;
    controller->expected_idx = 0U;
    controller->consumed_count = 0U;
    controller->next_sequence = 1U;
    controller->pending = false;
    controller->pending_sequence = 0U;
    controller->pending_element.id = 0U;
    controller->pending_element.len = 0U;
    controller->ring = ring;
    controller->reclaim = *reclaim;
    return VIRTIO_USED_CONSUMPTION_OK;
}

enum virtio_used_consumption_status
virtio_used_consumption_prepare(
    struct virtio_used_consumption *controller,
    struct virtio_used_element *entry,
    struct virtio_used_token *token)
{
    enum virtio_used_consumption_status status;
    struct virtio_used_element candidate;
    uint32_t distance;
    uint32_t slot;

    if (controller == NULL || entry == NULL || token == NULL) {
        return VIRTIO_USED_CONSUMPTION_NULL;
    }
    status = virtio_used_consumption_controller_valid(controller);
    if (status != VIRTIO_USED_CONSUMPTION_OK) {
        return status;
    }
    if (controller->pending) {
        return VIRTIO_USED_CONSUMPTION_PENDING_FULL;
    }
    distance = virtio_used_consumption_distance(
        controller->ring->idx, controller->expected_idx);
    if (distance == 0U) {
        return VIRTIO_USED_CONSUMPTION_EMPTY;
    }
    if (distance > controller->queue_size) {
        return VIRTIO_USED_CONSUMPTION_RING_OVERFLOW;
    }

    slot = controller->expected_idx % controller->queue_size;
    candidate = controller->ring->ring[slot];
    if (candidate.id >= controller->queue_size) {
        return VIRTIO_USED_CONSUMPTION_INVALID_HEAD;
    }
    if (candidate.len > controller->max_length) {
        return VIRTIO_USED_CONSUMPTION_INVALID_LENGTH;
    }

    controller->pending_element = candidate;
    controller->pending_sequence = controller->next_sequence;
    controller->pending = true;
    token->sequence = controller->next_sequence;
    token->generation = controller->generation;
    controller->next_sequence =
        virtio_used_consumption_next_sequence(
            controller->next_sequence);
    *entry = candidate;
    return VIRTIO_USED_CONSUMPTION_OK;
}

static enum virtio_used_consumption_status
virtio_used_consumption_validate_pending(
    struct virtio_used_consumption *controller,
    const struct virtio_used_token *token)
{
    struct virtio_used_element current;
    uint32_t distance;
    uint32_t slot;

    if (!controller->pending) {
        return VIRTIO_USED_CONSUMPTION_NO_PENDING;
    }
    if (token->generation != controller->generation ||
        token->sequence != controller->pending_sequence) {
        return VIRTIO_USED_CONSUMPTION_STALE_TOKEN;
    }
    distance = virtio_used_consumption_distance(
        controller->ring->idx, controller->expected_idx);
    if (distance == 0U) {
        return VIRTIO_USED_CONSUMPTION_INDEX_CHANGED;
    }
    if (distance > controller->queue_size) {
        return VIRTIO_USED_CONSUMPTION_RING_OVERFLOW;
    }
    slot = controller->expected_idx % controller->queue_size;
    current = controller->ring->ring[slot];
    if (current.id != controller->pending_element.id ||
        current.len != controller->pending_element.len) {
        return VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED;
    }
    return VIRTIO_USED_CONSUMPTION_OK;
}

enum virtio_used_consumption_status
virtio_used_consumption_consume(
    struct virtio_used_consumption *controller,
    const struct virtio_used_token *token)
{
    enum virtio_used_consumption_status status;

    if (controller == NULL || token == NULL) {
        return VIRTIO_USED_CONSUMPTION_NULL;
    }
    status = virtio_used_consumption_controller_valid(controller);
    if (status != VIRTIO_USED_CONSUMPTION_OK) {
        return status;
    }
    status = virtio_used_consumption_validate_pending(
        controller, token);
    if (status != VIRTIO_USED_CONSUMPTION_OK) {
        return status;
    }
    if (!controller->reclaim.reclaim(
            controller->reclaim.context,
            &controller->pending_element,
            token)) {
        return VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED;
    }

    controller->expected_idx = (uint32_t)(uint16_t)(
        controller->expected_idx + 1U);
    ++controller->consumed_count;
    virtio_used_consumption_clear_pending(controller);
    return VIRTIO_USED_CONSUMPTION_OK;
}

enum virtio_used_consumption_status
virtio_used_consumption_cancel(
    struct virtio_used_consumption *controller,
    const struct virtio_used_token *token)
{
    enum virtio_used_consumption_status status;

    if (controller == NULL || token == NULL) {
        return VIRTIO_USED_CONSUMPTION_NULL;
    }
    status = virtio_used_consumption_controller_valid(controller);
    if (status != VIRTIO_USED_CONSUMPTION_OK) {
        return status;
    }
    status = virtio_used_consumption_validate_pending(
        controller, token);
    if (status != VIRTIO_USED_CONSUMPTION_OK) {
        return status;
    }
    virtio_used_consumption_clear_pending(controller);
    return VIRTIO_USED_CONSUMPTION_OK;
}

bool virtio_used_consumption_published(
    const struct virtio_used_consumption *controller)
{
    return virtio_used_consumption_controller_valid(controller) ==
               VIRTIO_USED_CONSUMPTION_OK &&
           virtio_used_consumption_distance(
               controller->ring->idx,
               controller->expected_idx) <= controller->queue_size;
}

uint32_t virtio_used_consumption_visible_index(
    const struct virtio_used_consumption *controller)
{
    if (virtio_used_consumption_controller_valid(controller) !=
        VIRTIO_USED_CONSUMPTION_OK) {
        return 0U;
    }
    return controller->ring->idx;
}

uint32_t virtio_used_consumption_consumed_count(
    const struct virtio_used_consumption *controller)
{
    if (virtio_used_consumption_controller_valid(controller) !=
        VIRTIO_USED_CONSUMPTION_OK) {
        return 0U;
    }
    return controller->consumed_count;
}

uint32_t virtio_used_consumption_pending_head(
    const struct virtio_used_consumption *controller)
{
    if (virtio_used_consumption_controller_valid(controller) !=
            VIRTIO_USED_CONSUMPTION_OK ||
        !controller->pending) {
        return 0U;
    }
    return controller->pending_element.id;
}

const char *virtio_used_consumption_status_string(
    enum virtio_used_consumption_status status)
{
    switch (status) {
    case VIRTIO_USED_CONSUMPTION_OK:
        return "VIRTIO_USED_CONSUMPTION_OK";
    case VIRTIO_USED_CONSUMPTION_NULL:
        return "VIRTIO_USED_CONSUMPTION_NULL";
    case VIRTIO_USED_CONSUMPTION_INVALID_VERSION:
        return "VIRTIO_USED_CONSUMPTION_INVALID_VERSION";
    case VIRTIO_USED_CONSUMPTION_INVALID_GENERATION:
        return "VIRTIO_USED_CONSUMPTION_INVALID_GENERATION";
    case VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED:
        return "VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED";
    case VIRTIO_USED_CONSUMPTION_UNPUBLISHED:
        return "VIRTIO_USED_CONSUMPTION_UNPUBLISHED";
    case VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT:
        return "VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT";
    case VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE:
        return "VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE";
    case VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH:
        return "VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH";
    case VIRTIO_USED_CONSUMPTION_INVALID_RING:
        return "VIRTIO_USED_CONSUMPTION_INVALID_RING";
    case VIRTIO_USED_CONSUMPTION_INVALID_RELEASE:
        return "VIRTIO_USED_CONSUMPTION_INVALID_RELEASE";
    case VIRTIO_USED_CONSUMPTION_EMPTY:
        return "VIRTIO_USED_CONSUMPTION_EMPTY";
    case VIRTIO_USED_CONSUMPTION_RING_OVERFLOW:
        return "VIRTIO_USED_CONSUMPTION_RING_OVERFLOW";
    case VIRTIO_USED_CONSUMPTION_INVALID_HEAD:
        return "VIRTIO_USED_CONSUMPTION_INVALID_HEAD";
    case VIRTIO_USED_CONSUMPTION_INVALID_LENGTH:
        return "VIRTIO_USED_CONSUMPTION_INVALID_LENGTH";
    case VIRTIO_USED_CONSUMPTION_PENDING_FULL:
        return "VIRTIO_USED_CONSUMPTION_PENDING_FULL";
    case VIRTIO_USED_CONSUMPTION_NO_PENDING:
        return "VIRTIO_USED_CONSUMPTION_NO_PENDING";
    case VIRTIO_USED_CONSUMPTION_STALE_TOKEN:
        return "VIRTIO_USED_CONSUMPTION_STALE_TOKEN";
    case VIRTIO_USED_CONSUMPTION_INDEX_CHANGED:
        return "VIRTIO_USED_CONSUMPTION_INDEX_CHANGED";
    case VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED:
        return "VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED";
    case VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED:
        return "VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED";
    case VIRTIO_USED_CONSUMPTION_INVALID_STATE:
        return "VIRTIO_USED_CONSUMPTION_INVALID_STATE";
    default:
        return NULL;
    }
}
