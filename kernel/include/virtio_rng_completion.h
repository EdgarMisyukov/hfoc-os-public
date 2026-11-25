#ifndef HFOC_VIRTIO_RNG_COMPLETION_H
#define HFOC_VIRTIO_RNG_COMPLETION_H

#include "virtio_descriptor_ownership.h"
#include "virtio_rng_queue_composition.h"
#include "virtio_rng_request.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_RNG_COMPLETION_VERSION 1U

struct virtio_rng_completion_result {
    uint32_t bytes_written;
    enum virtio_rng_request_result request_result;
};

enum virtio_rng_completion_status {
    VIRTIO_RNG_COMPLETION_OK = 0,
    VIRTIO_RNG_COMPLETION_NULL = 1,
    VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API = 2,
    VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION = 3,
    VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR = 4,
    VIRTIO_RNG_COMPLETION_NOT_PENDING = 5,
    VIRTIO_RNG_COMPLETION_INVALID_STATE = 6,
    VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION = 7,
    VIRTIO_RNG_COMPLETION_USED_INVALID = 8,
    VIRTIO_RNG_COMPLETION_SHORT_COMPLETION = 9,
    VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION = 10,
    VIRTIO_RNG_COMPLETION_STALE_COMPLETION = 11,
    VIRTIO_RNG_COMPLETION_RELEASE_FAILED = 12,
};

enum virtio_rng_completion_status
virtio_rng_completion_complete(
    struct virtio_rng_request_api *api,
    struct virtio_rng_queue_composition *composition,
    const struct virtio_descriptor_token *request_descriptor,
    const void *payload,
    struct virtio_rng_completion_result *result);
const char *virtio_rng_completion_status_string(
    enum virtio_rng_completion_status status);

#endif
