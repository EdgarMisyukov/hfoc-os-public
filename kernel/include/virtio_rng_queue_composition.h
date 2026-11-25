#ifndef HFOC_VIRTIO_RNG_QUEUE_COMPOSITION_H
#define HFOC_VIRTIO_RNG_QUEUE_COMPOSITION_H

#include "virtio_available_publication.h"
#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_queue_layout.h"
#include "virtio_rng_feature_profile.h"
#include "virtio_used_consumption.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_RNG_QUEUE_COMPOSITION_VERSION 1U
#define VIRTIO_RNG_QUEUE_COMPOSITION_GENERATION_MIN 1U
#define VIRTIO_RNG_QUEUE_COMPOSITION_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX 0U
#define VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_SIZE_MIN 1U
#define VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_SIZE_MAX 64U
#define VIRTIO_RNG_QUEUE_COMPOSITION_DMA_CAPACITY_MIN 2U
#define VIRTIO_RNG_QUEUE_COMPOSITION_DMA_CAPACITY_MAX 64U
#define VIRTIO_RNG_QUEUE_COMPOSITION_MAX_USED_LENGTH 1048576U

struct virtio_rng_queue_config {
    uint32_t queue_index;
    uint32_t queue_size;
    bool event_index;
    uint64_t region_base;
    uint64_t region_length;
    struct virtio_available_ring *available_ring;
    struct virtio_used_ring *used_ring;
    struct virtio_available_barrier barrier;
    struct virtio_used_reclaim reclaim;
    uint32_t max_used_length;
    struct virtio_dma_window_policy dma_window;
    struct virtio_dma_zeroize dma_zeroize;
    uint32_t dma_capacity;
};

struct virtio_rng_queue {
    bool published;
    uint32_t version;
    uint32_t queue_index;
    uint32_t queue_size;
    struct virtio_queue_layout_result layout;
    struct virtio_descriptor_ownership descriptors;
    struct virtio_available_publication available;
    struct virtio_used_consumption used;
    struct virtio_dma_region_registry dma;
    struct virtio_dma_region_token dma_token;
    struct virtio_dma_region dma_region;
};

struct virtio_rng_queue_composition {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue queue;
};

enum virtio_rng_queue_composition_status {
    VIRTIO_RNG_QUEUE_COMPOSITION_OK = 0,
    VIRTIO_RNG_QUEUE_COMPOSITION_NULL = 1,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION = 2,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION = 3,
    VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED = 4,
    VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED = 5,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE = 6,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX = 7,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE = 8,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT = 9,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING = 10,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER = 11,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE = 12,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH = 13,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA = 14,
    VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED = 15,
    VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED = 16,
    VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED = 17,
    VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE = 18,
};

enum virtio_rng_queue_composition_status
virtio_rng_queue_composition_activate(
    struct virtio_rng_queue_composition *composition,
    const struct virtio_rng_feature_profile *profile,
    const struct virtio_rng_queue_config *config,
    uint32_t generation);
enum virtio_rng_queue_composition_status
virtio_rng_queue_composition_teardown(
    struct virtio_rng_queue_composition *composition);
bool virtio_rng_queue_composition_published(
    const struct virtio_rng_queue_composition *composition);
uint32_t virtio_rng_queue_composition_generation(
    const struct virtio_rng_queue_composition *composition);
const struct virtio_rng_queue *
virtio_rng_queue_composition_queue(
    const struct virtio_rng_queue_composition *composition);
const char *virtio_rng_queue_composition_status_string(
    enum virtio_rng_queue_composition_status status);

#endif
