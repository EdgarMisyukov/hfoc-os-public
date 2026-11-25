#include "virtio_rng_request.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_rng_request_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_RNG_REQUEST_GENERATION_MIN &&
           generation <= VIRTIO_RNG_REQUEST_GENERATION_MAX;
}

static bool virtio_rng_request_context_valid(uint32_t kind)
{
    return kind == VIRTIO_RNG_CALLER_CONTEXT_KERNEL ||
           kind == VIRTIO_RNG_CALLER_CONTEXT_PROCESS ||
           kind == VIRTIO_RNG_CALLER_CONTEXT_SERVICE;
}

static bool virtio_rng_request_api_valid(
    const struct virtio_rng_request_api *api)
{
    return api != NULL &&
           api->initialized &&
           api->version == VIRTIO_RNG_REQUEST_VERSION &&
           virtio_rng_request_generation_valid(api->generation);
}

static bool virtio_rng_request_current_valid(
    const struct virtio_rng_request *request,
    uint32_t generation)
{
    return request != NULL &&
           request->version == VIRTIO_RNG_REQUEST_VERSION &&
           request->destination != NULL &&
           request->destination_length >= VIRTIO_RNG_REQUEST_LENGTH_MIN &&
           request->destination_length <= VIRTIO_RNG_REQUEST_LENGTH_MAX &&
           virtio_rng_request_context_valid(request->context.kind) &&
           request->generation == generation &&
           request->result == VIRTIO_RNG_REQUEST_RESULT_PENDING;
}

static bool virtio_rng_request_ranges_overlap(
    const uint8_t *first,
    size_t first_length,
    const uint8_t *second,
    size_t second_length)
{
    uintptr_t first_base = (uintptr_t)first;
    uintptr_t second_base = (uintptr_t)second;

    if (first_length == 0U || second_length == 0U) {
        return false;
    }
    if (first_base < second_base) {
        return (uintptr_t)(second_base - first_base) < first_length;
    }
    return (uintptr_t)(first_base - second_base) < second_length;
}

enum virtio_rng_request_api_status
virtio_rng_request_api_init(
    struct virtio_rng_request_api *api,
    uint32_t generation)
{
    if (api == NULL) {
        return VIRTIO_RNG_REQUEST_API_NULL;
    }
    if (api->initialized) {
        return VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED;
    }
    if (api->version != 0U) {
        return VIRTIO_RNG_REQUEST_API_BAD_STATE;
    }
    if (!virtio_rng_request_generation_valid(generation)) {
        return VIRTIO_RNG_REQUEST_API_BAD_GENERATION;
    }

    memset(api, 0, sizeof(*api));
    api->version = VIRTIO_RNG_REQUEST_VERSION;
    api->generation = generation;
    api->initialized = true;
    return VIRTIO_RNG_REQUEST_API_OK;
}

enum virtio_rng_request_api_status
virtio_rng_request_api_submit(
    struct virtio_rng_request_api *api,
    const struct virtio_rng_request *request)
{
    if (api == NULL || request == NULL) {
        return VIRTIO_RNG_REQUEST_API_NULL;
    }
    if (!api->initialized) {
        return VIRTIO_RNG_REQUEST_API_UNPUBLISHED;
    }
    if (api->version != VIRTIO_RNG_REQUEST_VERSION ||
        !virtio_rng_request_generation_valid(api->generation)) {
        return VIRTIO_RNG_REQUEST_API_BAD_STATE;
    }
    if (request->version != VIRTIO_RNG_REQUEST_VERSION) {
        return VIRTIO_RNG_REQUEST_API_BAD_VERSION;
    }
    if (request->generation != api->generation) {
        return VIRTIO_RNG_REQUEST_API_BAD_GENERATION;
    }
    if (request->destination == NULL) {
        return VIRTIO_RNG_REQUEST_API_NULL_DESTINATION;
    }
    if (request->destination_length == 0U) {
        return VIRTIO_RNG_REQUEST_API_ZERO_LENGTH;
    }
    if (request->destination_length >
        VIRTIO_RNG_REQUEST_LENGTH_MAX) {
        return VIRTIO_RNG_REQUEST_API_TOO_LONG;
    }
    if (!virtio_rng_request_context_valid(request->context.kind)) {
        if (request->context.kind ==
            VIRTIO_RNG_CALLER_CONTEXT_INTERRUPT) {
            return VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT;
        }
        return VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT;
    }
    if (request->result != VIRTIO_RNG_REQUEST_RESULT_PENDING) {
        return VIRTIO_RNG_REQUEST_API_INVALID_RESULT;
    }
    if (virtio_rng_request_ranges_overlap(
            request->destination,
            (size_t)request->destination_length,
            (const uint8_t *)request,
            sizeof(*request))) {
        return VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION;
    }
    if (virtio_rng_request_ranges_overlap(
            request->destination,
            (size_t)request->destination_length,
            (const uint8_t *)api,
            sizeof(*api))) {
        return VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION;
    }
    if (api->pending) {
        if (virtio_rng_request_ranges_overlap(
                request->destination,
                (size_t)request->destination_length,
                api->current.destination,
                (size_t)api->current.destination_length)) {
            return VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION;
        }
        return VIRTIO_RNG_REQUEST_API_BUSY;
    }

    memset(&api->current, 0, sizeof(api->current));
    api->current = *request;
    api->pending = true;
    return VIRTIO_RNG_REQUEST_API_OK;
}

enum virtio_rng_request_api_status
virtio_rng_request_api_complete(
    struct virtio_rng_request_api *api,
    enum virtio_rng_request_result result)
{
    if (api == NULL) {
        return VIRTIO_RNG_REQUEST_API_NULL;
    }
    if (!api->initialized) {
        return VIRTIO_RNG_REQUEST_API_UNPUBLISHED;
    }
    if (!virtio_rng_request_api_valid(api)) {
        return VIRTIO_RNG_REQUEST_API_BAD_STATE;
    }
    if (!api->pending) {
        return VIRTIO_RNG_REQUEST_API_NOT_PENDING;
    }
    if (!virtio_rng_request_current_valid(
            &api->current, api->generation)) {
        return VIRTIO_RNG_REQUEST_API_BAD_STATE;
    }
    if (result != VIRTIO_RNG_REQUEST_RESULT_READY &&
        result != VIRTIO_RNG_REQUEST_RESULT_FAILED) {
        return VIRTIO_RNG_REQUEST_API_INVALID_RESULT;
    }

    api->current.result = result;
    api->pending = false;
    return VIRTIO_RNG_REQUEST_API_OK;
}

bool virtio_rng_request_api_initialized(
    const struct virtio_rng_request_api *api)
{
    return virtio_rng_request_api_valid(api);
}

bool virtio_rng_request_api_pending(
    const struct virtio_rng_request_api *api)
{
    return virtio_rng_request_api_valid(api) &&
           api->pending &&
           virtio_rng_request_current_valid(
               &api->current, api->generation);
}

uint32_t virtio_rng_request_api_generation(
    const struct virtio_rng_request_api *api)
{
    if (!virtio_rng_request_api_valid(api)) {
        return 0U;
    }
    return api->generation;
}

const struct virtio_rng_request *
virtio_rng_request_api_current(
    const struct virtio_rng_request_api *api)
{
    if (!virtio_rng_request_api_valid(api)) {
        return NULL;
    }
    return &api->current;
}

const char *virtio_rng_request_api_status_string(
    enum virtio_rng_request_api_status status)
{
    switch (status) {
    case VIRTIO_RNG_REQUEST_API_OK:
        return "VIRTIO_RNG_REQUEST_API_OK";
    case VIRTIO_RNG_REQUEST_API_NULL:
        return "VIRTIO_RNG_REQUEST_API_NULL";
    case VIRTIO_RNG_REQUEST_API_BAD_STATE:
        return "VIRTIO_RNG_REQUEST_API_BAD_STATE";
    case VIRTIO_RNG_REQUEST_API_BAD_VERSION:
        return "VIRTIO_RNG_REQUEST_API_BAD_VERSION";
    case VIRTIO_RNG_REQUEST_API_BAD_GENERATION:
        return "VIRTIO_RNG_REQUEST_API_BAD_GENERATION";
    case VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED:
        return "VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED";
    case VIRTIO_RNG_REQUEST_API_UNPUBLISHED:
        return "VIRTIO_RNG_REQUEST_API_UNPUBLISHED";
    case VIRTIO_RNG_REQUEST_API_BUSY:
        return "VIRTIO_RNG_REQUEST_API_BUSY";
    case VIRTIO_RNG_REQUEST_API_ZERO_LENGTH:
        return "VIRTIO_RNG_REQUEST_API_ZERO_LENGTH";
    case VIRTIO_RNG_REQUEST_API_TOO_LONG:
        return "VIRTIO_RNG_REQUEST_API_TOO_LONG";
    case VIRTIO_RNG_REQUEST_API_NULL_DESTINATION:
        return "VIRTIO_RNG_REQUEST_API_NULL_DESTINATION";
    case VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION:
        return "VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION";
    case VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT:
        return "VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT";
    case VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT:
        return "VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT";
    case VIRTIO_RNG_REQUEST_API_INVALID_RESULT:
        return "VIRTIO_RNG_REQUEST_API_INVALID_RESULT";
    case VIRTIO_RNG_REQUEST_API_NOT_PENDING:
        return "VIRTIO_RNG_REQUEST_API_NOT_PENDING";
    default:
        return NULL;
    }
}
