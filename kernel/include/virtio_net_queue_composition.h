#ifndef HFOC_VIRTIO_NET_QUEUE_COMPOSITION_H
#define HFOC_VIRTIO_NET_QUEUE_COMPOSITION_H

#include "virtio_available_publication.h"
#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_net_feature_profile.h"
#include "virtio_queue_layout.h"
#include "virtio_used_consumption.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_QUEUE_COMPOSITION_VERSION 1U
#define VIRTIO_NET_QUEUE_COMPOSITION_GENERATION_MIN 1U
#define VIRTIO_NET_QUEUE_COMPOSITION_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX 0U
#define VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX 1U
#define VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_SIZE_MIN 1U
#define VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_SIZE_MAX 64U
#define VIRTIO_NET_QUEUE_COMPOSITION_DMA_CAPACITY_MIN 2U
#define VIRTIO_NET_QUEUE_COMPOSITION_DMA_CAPACITY_MAX 64U
#define VIRTIO_NET_QUEUE_COMPOSITION_MAX_USED_LENGTH 1048576U

enum virtio_net_queue_direction {
    VIRTIO_NET_QUEUE_RECEIVE = 0,
    VIRTIO_NET_QUEUE_TRANSMIT = 1,
};

struct virtio_net_queue_config {
    enum virtio_net_queue_direction direction;
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

struct virtio_net_queue {
    bool published;
    uint32_t version;
    enum virtio_net_queue_direction direction;
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

struct virtio_net_queue_composition {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue receive;
    struct virtio_net_queue transmit;
};

enum virtio_net_queue_composition_status {
    VIRTIO_NET_QUEUE_COMPOSITION_OK = 0,
    VIRTIO_NET_QUEUE_COMPOSITION_NULL = 1,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_VERSION = 2,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_GENERATION = 3,
    VIRTIO_NET_QUEUE_COMPOSITION_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_QUEUE_COMPOSITION_UNPUBLISHED = 5,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE = 6,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_CONFIG = 7,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DIRECTION = 8,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX = 9,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE = 10,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_LAYOUT = 11,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING = 12,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_BARRIER = 13,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RELEASE = 14,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_MAX_LENGTH = 15,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DMA = 16,
    VIRTIO_NET_QUEUE_COMPOSITION_DMA_BIND_FAILED = 17,
    VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INIT_FAILED = 18,
    VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED = 19,
    VIRTIO_NET_QUEUE_COMPOSITION_INVALID_STATE = 20,
};

enum virtio_net_queue_composition_status
virtio_net_queue_composition_activate(
    struct virtio_net_queue_composition *composition,
    const struct virtio_net_feature_profile *profile,
    const struct virtio_net_queue_config *receive_config,
    const struct virtio_net_queue_config *transmit_config,
    uint32_t generation);
enum virtio_net_queue_composition_status
virtio_net_queue_composition_teardown(
    struct virtio_net_queue_composition *composition);
bool virtio_net_queue_composition_published(
    const struct virtio_net_queue_composition *composition);
uint32_t virtio_net_queue_composition_generation(
    const struct virtio_net_queue_composition *composition);
const struct virtio_net_queue *
virtio_net_queue_composition_receive(
    const struct virtio_net_queue_composition *composition);
const struct virtio_net_queue *
virtio_net_queue_composition_transmit(
    const struct virtio_net_queue_composition *composition);
const char *virtio_net_queue_composition_status_string(
    enum virtio_net_queue_composition_status status);

#endif
