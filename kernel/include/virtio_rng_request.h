#ifndef HFOC_VIRTIO_RNG_REQUEST_H
#define HFOC_VIRTIO_RNG_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_RNG_REQUEST_VERSION 1U
#define VIRTIO_RNG_REQUEST_LENGTH_MIN 1U
#define VIRTIO_RNG_REQUEST_LENGTH_MAX 4096U
#define VIRTIO_RNG_REQUEST_GENERATION_MIN 1U
#define VIRTIO_RNG_REQUEST_GENERATION_MAX UINT32_C(0xFFFFFFFE)

enum virtio_rng_caller_kind {
    VIRTIO_RNG_CALLER_CONTEXT_UNKNOWN = 0U,
    VIRTIO_RNG_CALLER_CONTEXT_KERNEL = 1U,
    VIRTIO_RNG_CALLER_CONTEXT_PROCESS = 2U,
    VIRTIO_RNG_CALLER_CONTEXT_SERVICE = 3U,
    VIRTIO_RNG_CALLER_CONTEXT_INTERRUPT = 4U,
};

enum virtio_rng_request_result {
    VIRTIO_RNG_REQUEST_RESULT_UNKNOWN = 0U,
    VIRTIO_RNG_REQUEST_RESULT_PENDING = 1U,
    VIRTIO_RNG_REQUEST_RESULT_READY = 2U,
    VIRTIO_RNG_REQUEST_RESULT_FAILED = 3U,
};

struct virtio_rng_caller_context {
    uint32_t kind;
    uint32_t id;
};

struct virtio_rng_request {
    uint32_t version;
    uint8_t *destination;
    uint32_t destination_length;
    struct virtio_rng_caller_context context;
    uint32_t generation;
    enum virtio_rng_request_result result;
};

struct virtio_rng_request_api {
    bool initialized;
    uint32_t version;
    uint32_t generation;
    bool pending;
    struct virtio_rng_request current;
};

enum virtio_rng_request_api_status {
    VIRTIO_RNG_REQUEST_API_OK = 0,
    VIRTIO_RNG_REQUEST_API_NULL = 1,
    VIRTIO_RNG_REQUEST_API_BAD_STATE = 2,
    VIRTIO_RNG_REQUEST_API_BAD_VERSION = 3,
    VIRTIO_RNG_REQUEST_API_BAD_GENERATION = 4,
    VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED = 5,
    VIRTIO_RNG_REQUEST_API_UNPUBLISHED = 6,
    VIRTIO_RNG_REQUEST_API_BUSY = 7,
    VIRTIO_RNG_REQUEST_API_ZERO_LENGTH = 8,
    VIRTIO_RNG_REQUEST_API_TOO_LONG = 9,
    VIRTIO_RNG_REQUEST_API_NULL_DESTINATION = 10,
    VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION = 11,
    VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT = 12,
    VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT = 13,
    VIRTIO_RNG_REQUEST_API_INVALID_RESULT = 14,
    VIRTIO_RNG_REQUEST_API_NOT_PENDING = 15,
};

enum virtio_rng_request_api_status
virtio_rng_request_api_init(
    struct virtio_rng_request_api *api,
    uint32_t generation);
enum virtio_rng_request_api_status
virtio_rng_request_api_submit(
    struct virtio_rng_request_api *api,
    const struct virtio_rng_request *request);
enum virtio_rng_request_api_status
virtio_rng_request_api_complete(
    struct virtio_rng_request_api *api,
    enum virtio_rng_request_result result);
bool virtio_rng_request_api_initialized(
    const struct virtio_rng_request_api *api);
bool virtio_rng_request_api_pending(
    const struct virtio_rng_request_api *api);
uint32_t virtio_rng_request_api_generation(
    const struct virtio_rng_request_api *api);
const struct virtio_rng_request *
virtio_rng_request_api_current(
    const struct virtio_rng_request_api *api);
const char *virtio_rng_request_api_status_string(
    enum virtio_rng_request_api_status status);

#endif
