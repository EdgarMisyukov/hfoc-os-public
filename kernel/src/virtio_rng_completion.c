#include "virtio_rng_completion.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct virtio_rng_completion_reclaim_context {
    struct virtio_rng_queue_composition *composition;
    const struct virtio_descriptor_token *descriptor;
};

static void virtio_rng_completion_zeroize(
    uint8_t *destination,
    uint32_t length)
{
    volatile uint8_t *bytes = destination;
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool virtio_rng_completion_used_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct virtio_rng_completion_reclaim_context *reclaim_context =
        (struct virtio_rng_completion_reclaim_context *)context;
    struct virtio_rng_queue *queue;

    if (reclaim_context == NULL ||
        element == NULL ||
        token == NULL ||
        reclaim_context->composition == NULL ||
        reclaim_context->descriptor == NULL) {
        return false;
    }
    if (element->id != reclaim_context->descriptor->index) {
        return false;
    }
    if (token->generation != reclaim_context->composition->generation) {
        return false;
    }
    queue = &reclaim_context->composition->queue;
    return virtio_descriptor_ownership_consume(
               &queue->descriptors,
               reclaim_context->descriptor) ==
           VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

static enum virtio_rng_completion_status
virtio_rng_completion_consume_used(
    struct virtio_rng_queue_composition *composition,
    const struct virtio_descriptor_token *descriptor,
    const struct virtio_used_token *token)
{
    struct virtio_rng_queue *queue;
    struct virtio_used_reclaim original_reclaim;
    struct virtio_rng_completion_reclaim_context reclaim_context;
    enum virtio_used_consumption_status used_status;

    queue = &composition->queue;
    original_reclaim = queue->used.reclaim;
    reclaim_context.composition = composition;
    reclaim_context.descriptor = descriptor;
    queue->used.reclaim.reclaim = virtio_rng_completion_used_reclaim;
    queue->used.reclaim.context = &reclaim_context;
    used_status = virtio_used_consumption_consume(
        &queue->used, token);
    queue->used.reclaim = original_reclaim;

    return used_status == VIRTIO_USED_CONSUMPTION_OK ?
               VIRTIO_RNG_COMPLETION_OK :
               VIRTIO_RNG_COMPLETION_RELEASE_FAILED;
}

static enum virtio_rng_completion_status
virtio_rng_completion_fail_request(
    struct virtio_rng_request_api *api)
{
    enum virtio_rng_request_api_status status;

    status = virtio_rng_request_api_complete(
        api, VIRTIO_RNG_REQUEST_RESULT_FAILED);
    return status == VIRTIO_RNG_REQUEST_API_OK ?
               VIRTIO_RNG_COMPLETION_OK :
               VIRTIO_RNG_COMPLETION_INVALID_STATE;
}

enum virtio_rng_completion_status
virtio_rng_completion_complete(
    struct virtio_rng_request_api *api,
    struct virtio_rng_queue_composition *composition,
    const struct virtio_descriptor_token *request_descriptor,
    const void *payload,
    struct virtio_rng_completion_result *result)
{
    struct virtio_rng_queue *queue;
    struct virtio_used_element entry;
    struct virtio_used_token token;
    const struct virtio_rng_request *current;
    const uint8_t *bytes;
    uint8_t *destination;
    enum virtio_used_consumption_status used_status;
    enum virtio_rng_completion_status status;
    enum virtio_rng_request_api_status request_status;
    uint32_t destination_length;

    if (api == NULL ||
        composition == NULL ||
        request_descriptor == NULL ||
        payload == NULL ||
        result == NULL) {
        return VIRTIO_RNG_COMPLETION_NULL;
    }
    memset(result, 0, sizeof(*result));

    if (!virtio_rng_request_api_initialized(api)) {
        return VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API;
    }
    if (!virtio_rng_request_api_pending(api)) {
        return VIRTIO_RNG_COMPLETION_NOT_PENDING;
    }
    if (!virtio_rng_queue_composition_published(composition)) {
        return VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION;
    }
    if (virtio_rng_request_api_generation(api) !=
        virtio_rng_queue_composition_generation(composition)) {
        return VIRTIO_RNG_COMPLETION_INVALID_STATE;
    }

    queue = &composition->queue;
    if (request_descriptor->index >= queue->queue_size ||
        queue->descriptors.generations[request_descriptor->index] !=
            request_descriptor->generation ||
        queue->descriptors.states[request_descriptor->index] !=
            VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
        return VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR;
    }

    current = virtio_rng_request_api_current(api);
    if (current == NULL ||
        current->destination == NULL ||
        current->destination_length == 0U) {
        return VIRTIO_RNG_COMPLETION_INVALID_STATE;
    }
    destination = current->destination;
    destination_length = current->destination_length;
    bytes = (const uint8_t *)payload;

    used_status = virtio_used_consumption_prepare(
        &queue->used, &entry, &token);
    if (used_status == VIRTIO_USED_CONSUMPTION_EMPTY) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        if (virtio_rng_completion_fail_request(api) !=
            VIRTIO_RNG_COMPLETION_OK) {
            return VIRTIO_RNG_COMPLETION_INVALID_STATE;
        }
        return VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION;
    }
    if (used_status != VIRTIO_USED_CONSUMPTION_OK) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        if (virtio_rng_completion_fail_request(api) !=
            VIRTIO_RNG_COMPLETION_OK) {
            return VIRTIO_RNG_COMPLETION_INVALID_STATE;
        }
        return VIRTIO_RNG_COMPLETION_USED_INVALID;
    }

    if (entry.id != request_descriptor->index ||
        queue->descriptors.generations[request_descriptor->index] !=
            request_descriptor->generation ||
        queue->descriptors.states[request_descriptor->index] !=
            VIRTIO_DESCRIPTOR_STATE_PUBLISHED) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        (void)virtio_used_consumption_cancel(&queue->used, &token);
        if (virtio_rng_completion_fail_request(api) !=
            VIRTIO_RNG_COMPLETION_OK) {
            return VIRTIO_RNG_COMPLETION_INVALID_STATE;
        }
        return VIRTIO_RNG_COMPLETION_STALE_COMPLETION;
    }

    if (entry.len < destination_length) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        status = virtio_rng_completion_consume_used(
            composition, request_descriptor, &token);
        if (virtio_rng_completion_fail_request(api) !=
            VIRTIO_RNG_COMPLETION_OK) {
            return VIRTIO_RNG_COMPLETION_INVALID_STATE;
        }
        return status == VIRTIO_RNG_COMPLETION_OK ?
                   VIRTIO_RNG_COMPLETION_SHORT_COMPLETION :
                   VIRTIO_RNG_COMPLETION_RELEASE_FAILED;
    }
    if (entry.len > destination_length) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        status = virtio_rng_completion_consume_used(
            composition, request_descriptor, &token);
        if (virtio_rng_completion_fail_request(api) !=
            VIRTIO_RNG_COMPLETION_OK) {
            return VIRTIO_RNG_COMPLETION_INVALID_STATE;
        }
        return status == VIRTIO_RNG_COMPLETION_OK ?
                   VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION :
                   VIRTIO_RNG_COMPLETION_RELEASE_FAILED;
    }

    status = virtio_rng_completion_consume_used(
        composition, request_descriptor, &token);
    if (status != VIRTIO_RNG_COMPLETION_OK) {
        virtio_rng_completion_zeroize(destination, destination_length);
        result->request_result = VIRTIO_RNG_REQUEST_RESULT_FAILED;
        (void)virtio_rng_completion_fail_request(api);
        return VIRTIO_RNG_COMPLETION_RELEASE_FAILED;
    }

    memcpy(destination, bytes, destination_length);
    request_status = virtio_rng_request_api_complete(
        api, VIRTIO_RNG_REQUEST_RESULT_READY);
    if (request_status != VIRTIO_RNG_REQUEST_API_OK) {
        return VIRTIO_RNG_COMPLETION_INVALID_STATE;
    }
    result->bytes_written = destination_length;
    result->request_result = VIRTIO_RNG_REQUEST_RESULT_READY;
    return VIRTIO_RNG_COMPLETION_OK;
}

const char *virtio_rng_completion_status_string(
    enum virtio_rng_completion_status status)
{
    switch (status) {
    case VIRTIO_RNG_COMPLETION_OK:
        return "VIRTIO_RNG_COMPLETION_OK";
    case VIRTIO_RNG_COMPLETION_NULL:
        return "VIRTIO_RNG_COMPLETION_NULL";
    case VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API:
        return "VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API";
    case VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION:
        return "VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION";
    case VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR:
        return "VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR";
    case VIRTIO_RNG_COMPLETION_NOT_PENDING:
        return "VIRTIO_RNG_COMPLETION_NOT_PENDING";
    case VIRTIO_RNG_COMPLETION_INVALID_STATE:
        return "VIRTIO_RNG_COMPLETION_INVALID_STATE";
    case VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION:
        return "VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION";
    case VIRTIO_RNG_COMPLETION_USED_INVALID:
        return "VIRTIO_RNG_COMPLETION_USED_INVALID";
    case VIRTIO_RNG_COMPLETION_SHORT_COMPLETION:
        return "VIRTIO_RNG_COMPLETION_SHORT_COMPLETION";
    case VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION:
        return "VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION";
    case VIRTIO_RNG_COMPLETION_STALE_COMPLETION:
        return "VIRTIO_RNG_COMPLETION_STALE_COMPLETION";
    case VIRTIO_RNG_COMPLETION_RELEASE_FAILED:
        return "VIRTIO_RNG_COMPLETION_RELEASE_FAILED";
    default:
        return NULL;
    }
}
