#ifndef HFOC_VIRTIO_NET_TX_H
#define HFOC_VIRTIO_NET_TX_H

#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_net_queue_composition.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_TX_VERSION 1U
#define VIRTIO_NET_TX_CAPACITY_MIN 1U
#define VIRTIO_NET_TX_CAPACITY_MAX 16U
#define VIRTIO_NET_TX_HEADER_LENGTH_MIN 2U
#define VIRTIO_NET_TX_HEADER_LENGTH_MAX 128U
#define VIRTIO_NET_TX_FRAME_LENGTH_MIN 60U
#define VIRTIO_NET_TX_FRAME_LENGTH_MAX 1518U
#define VIRTIO_NET_TX_GENERATION_MIN 1U
#define VIRTIO_NET_TX_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_NET_TX_DMA_DIRECTION VIRTIO_DMA_DIRECTION_DEVICE_READ
#define VIRTIO_NET_TX_DMA_LIFETIME VIRTIO_DMA_LIFETIME_PAYLOAD
#define VIRTIO_NET_TX_DMA_ZEROIZATION VIRTIO_DMA_ZEROIZATION_ON_RELEASE

struct virtio_net_tx_header_descriptor {
    uint64_t physical_base;
    uint32_t length;
    void *alias;
};

struct virtio_net_tx_config {
    const struct virtio_net_tx_header_descriptor *headers;
    uint32_t header_count;
    uint32_t maximum_frame_length;
    struct virtio_dma_window_policy dma_window;
    struct virtio_dma_zeroize dma_zeroize;
    uint32_t dma_capacity;
};

struct virtio_net_tx_frame {
    const void *data;
    uint64_t physical_base;
    uint32_t length;
};

struct virtio_net_tx_submission {
    uint32_t header_descriptor_index;
    uint32_t frame_descriptor_index;
    uint32_t slot_index;
    uint32_t generation;
};

struct virtio_net_tx_completion {
    const void *data;
    uint64_t physical_base;
    uint32_t length;
    uint32_t slot_index;
    uint32_t generation;
};

struct virtio_net_tx_notify {
    bool (*notify)(
        void *context,
        const struct virtio_net_tx_submission *submission);
    void *context;
};

struct virtio_net_tx_slot {
    bool active;
    uint32_t generation;
    const void *frame_data;
    uint64_t frame_physical_base;
    uint32_t frame_length;
    struct virtio_descriptor_token header_descriptor;
    struct virtio_descriptor_token frame_descriptor;
    struct virtio_dma_region_token header_dma;
    struct virtio_dma_region_token frame_dma;
};

struct virtio_net_tx {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t capacity;
    uint32_t submitted_count;
    uint32_t reclaimed_count;
    uint32_t invalid_head_count;
    uint32_t invalid_length_count;
    uint32_t stale_completion_count;
    uint32_t maximum_frame_length;
    uint32_t queue_size;
    struct virtio_net_tx_header_descriptor
        headers[VIRTIO_NET_TX_CAPACITY_MAX];
    struct virtio_net_queue_composition *composition;
    struct virtio_net_tx_notify notify;
    struct virtio_dma_region_registry dma;
    struct virtio_net_tx_slot slots[VIRTIO_NET_TX_CAPACITY_MAX];
};

enum virtio_net_tx_status {
    VIRTIO_NET_TX_OK = 0,
    VIRTIO_NET_TX_NULL = 1,
    VIRTIO_NET_TX_INVALID_VERSION = 2,
    VIRTIO_NET_TX_INVALID_GENERATION = 3,
    VIRTIO_NET_TX_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_TX_UNPUBLISHED = 5,
    VIRTIO_NET_TX_INVALID_COMPOSITION = 6,
    VIRTIO_NET_TX_INVALID_CONFIG = 7,
    VIRTIO_NET_TX_INVALID_HEADER_COUNT = 8,
    VIRTIO_NET_TX_QUEUE_FULL = 9,
    VIRTIO_NET_TX_DMA_SHORTAGE = 10,
    VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH = 11,
    VIRTIO_NET_TX_INVALID_HEADER = 12,
    VIRTIO_NET_TX_HEADER_OVERFLOW = 13,
    VIRTIO_NET_TX_HEADER_UNALIGNED = 14,
    VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW = 15,
    VIRTIO_NET_TX_HEADER_ALIAS = 16,
    VIRTIO_NET_TX_INVALID_DMA = 17,
    VIRTIO_NET_TX_DESCRIPTOR_FAILED = 18,
    VIRTIO_NET_TX_AVAILABLE_FAILED = 19,
    VIRTIO_NET_TX_NOTIFY_FAILED = 20,
    VIRTIO_NET_TX_INVALID_FRAME = 21,
    VIRTIO_NET_TX_TEARDOWN_FAILED = 22,
    VIRTIO_NET_TX_INVALID_STATE = 23,
    VIRTIO_NET_TX_USED_EMPTY = 24,
    VIRTIO_NET_TX_INVALID_HEAD = 25,
    VIRTIO_NET_TX_INVALID_LENGTH = 26,
    VIRTIO_NET_TX_STALE_COMPLETION = 27,
    VIRTIO_NET_TX_RELEASE_FAILED = 28,
    VIRTIO_NET_TX_USED_INVALID = 29,
};

enum virtio_net_tx_status virtio_net_tx_activate(
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition,
    const struct virtio_net_tx_config *config,
    const struct virtio_net_tx_notify *notify);
enum virtio_net_tx_status virtio_net_tx_teardown(
    struct virtio_net_tx *tx);
enum virtio_net_tx_status virtio_net_tx_submit(
    struct virtio_net_tx *tx,
    const struct virtio_net_tx_frame *frame,
    struct virtio_net_tx_submission *submission);
enum virtio_net_tx_status virtio_net_tx_reclaim(
    struct virtio_net_tx *tx,
    struct virtio_net_tx_completion *completion);
bool virtio_net_tx_published(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_generation(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_capacity(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_submitted_count(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_reclaimed_count(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_invalid_head_count(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_invalid_length_count(const struct virtio_net_tx *tx);
uint32_t virtio_net_tx_stale_completion_count(
    const struct virtio_net_tx *tx);
const char *virtio_net_tx_status_string(
    enum virtio_net_tx_status status);

#endif
