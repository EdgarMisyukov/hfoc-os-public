#ifndef HFOC_VIRTIO_NET_RX_H
#define HFOC_VIRTIO_NET_RX_H

#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_net_queue_composition.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_RX_VERSION 1U
#define VIRTIO_NET_RX_CAPACITY_MIN 1U
#define VIRTIO_NET_RX_CAPACITY_MAX 64U
#define VIRTIO_NET_RX_BUFFER_LENGTH_MIN 64U
#define VIRTIO_NET_RX_BUFFER_LENGTH_MAX 1048576U
#define VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MIN 64U
#define VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MAX 1048576U
#define VIRTIO_NET_RX_GENERATION_MIN 1U
#define VIRTIO_NET_RX_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_NET_RX_ETHERNET_FRAME_MIN 60U
#define VIRTIO_NET_RX_ETHERNET_FRAME_MAX 1518U

#define VIRTIO_NET_RX_DMA_DIRECTION VIRTIO_DMA_DIRECTION_DEVICE_WRITE
#define VIRTIO_NET_RX_DMA_LIFETIME VIRTIO_DMA_LIFETIME_PAYLOAD
#define VIRTIO_NET_RX_DMA_ZEROIZATION VIRTIO_DMA_ZEROIZATION_ON_RELEASE

struct virtio_net_rx_buffer_descriptor {
    uint64_t physical_base;
    uint32_t length;
};

struct virtio_net_rx_config {
    const struct virtio_net_rx_buffer_descriptor *buffers;
    uint32_t buffer_count;
    uint32_t minimum_frame_capacity;
    struct virtio_dma_window_policy dma_window;
    struct virtio_dma_zeroize dma_zeroize;
    uint32_t dma_capacity;
};

struct virtio_net_rx_buffer {
    bool posted;
    uint32_t generation;
    uint64_t physical_base;
    uint32_t length;
    struct virtio_descriptor_token descriptor;
    struct virtio_dma_region_token dma;
};

struct virtio_net_rx_frame {
    const void *data;
    uint64_t physical_base;
    uint32_t length;
    uint32_t buffer_index;
    uint32_t generation;
};

struct virtio_net_rx {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t capacity;
    uint32_t posted_count;
    uint32_t minimum_frame_capacity;
    uint32_t queue_size;
    struct virtio_net_queue_composition *composition;
    struct virtio_dma_region_registry dma;
    struct virtio_net_rx_buffer buffers[VIRTIO_NET_RX_CAPACITY_MAX];
};

enum virtio_net_rx_status {
    VIRTIO_NET_RX_OK = 0,
    VIRTIO_NET_RX_NULL = 1,
    VIRTIO_NET_RX_INVALID_VERSION = 2,
    VIRTIO_NET_RX_INVALID_GENERATION = 3,
    VIRTIO_NET_RX_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_RX_UNPUBLISHED = 5,
    VIRTIO_NET_RX_INVALID_COMPOSITION = 6,
    VIRTIO_NET_RX_INVALID_CONFIG = 7,
    VIRTIO_NET_RX_INVALID_BUFFER_COUNT = 8,
    VIRTIO_NET_RX_QUEUE_FULL = 9,
    VIRTIO_NET_RX_DMA_SHORTAGE = 10,
    VIRTIO_NET_RX_INVALID_FRAME_CAPACITY = 11,
    VIRTIO_NET_RX_INVALID_BUFFER = 12,
    VIRTIO_NET_RX_BUFFER_OVERFLOW = 13,
    VIRTIO_NET_RX_BUFFER_UNALIGNED = 14,
    VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW = 15,
    VIRTIO_NET_RX_BUFFER_ALIAS = 16,
    VIRTIO_NET_RX_INVALID_DMA = 17,
    VIRTIO_NET_RX_DMA_BIND_FAILED = 18,
    VIRTIO_NET_RX_DESCRIPTOR_FAILED = 19,
    VIRTIO_NET_RX_AVAILABLE_FAILED = 20,
    VIRTIO_NET_RX_TEARDOWN_FAILED = 21,
    VIRTIO_NET_RX_INVALID_STATE = 22,
    VIRTIO_NET_RX_INVALID_HEADER = 23,
    VIRTIO_NET_RX_SHORT_FRAME = 24,
    VIRTIO_NET_RX_OVERSIZED_FRAME = 25,
    VIRTIO_NET_RX_STALE_COMPLETION = 26,
    VIRTIO_NET_RX_USED_EMPTY = 27,
    VIRTIO_NET_RX_USED_INVALID = 28,
    VIRTIO_NET_RX_RELEASE_FAILED = 29,
};

enum virtio_net_rx_status virtio_net_rx_activate(
    struct virtio_net_rx *rx,
    struct virtio_net_queue_composition *composition,
    const struct virtio_net_rx_config *config);
enum virtio_net_rx_status virtio_net_rx_teardown(
    struct virtio_net_rx *rx);
enum virtio_net_rx_status virtio_net_rx_complete(
    struct virtio_net_rx *rx,
    const void *payload,
    struct virtio_net_rx_frame *frame);
bool virtio_net_rx_published(const struct virtio_net_rx *rx);
uint32_t virtio_net_rx_generation(const struct virtio_net_rx *rx);
uint32_t virtio_net_rx_capacity(const struct virtio_net_rx *rx);
uint32_t virtio_net_rx_posted_count(const struct virtio_net_rx *rx);
const struct virtio_net_rx_buffer *
virtio_net_rx_buffer_at(const struct virtio_net_rx *rx, uint32_t index);
const char *virtio_net_rx_status_string(
    enum virtio_net_rx_status status);

#endif
