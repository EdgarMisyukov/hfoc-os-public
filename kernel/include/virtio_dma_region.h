#ifndef HFOC_VIRTIO_DMA_REGION_H
#define HFOC_VIRTIO_DMA_REGION_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_DMA_REGION_VERSION 1U
#define VIRTIO_DMA_REGION_CAPACITY_MIN 1U
#define VIRTIO_DMA_REGION_CAPACITY_MAX 64U
#define VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MIN 1U
#define VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX 4096U
#define VIRTIO_DMA_REGION_LENGTH_MAX 1048576U

enum virtio_dma_direction {
    VIRTIO_DMA_DIRECTION_DEVICE_READ = 0,
    VIRTIO_DMA_DIRECTION_DEVICE_WRITE = 1,
    VIRTIO_DMA_DIRECTION_BIDIRECTIONAL = 2,
};

enum virtio_dma_lifetime {
    VIRTIO_DMA_LIFETIME_QUEUE = 0,
    VIRTIO_DMA_LIFETIME_PAYLOAD = 1,
};

enum virtio_dma_zeroization {
    VIRTIO_DMA_ZEROIZATION_NONE = 0,
    VIRTIO_DMA_ZEROIZATION_ON_RELEASE = 1,
};

enum virtio_dma_region_status {
    VIRTIO_DMA_REGION_OK = 0,
    VIRTIO_DMA_REGION_NULL = 1,
    VIRTIO_DMA_REGION_BAD_STATE = 2,
    VIRTIO_DMA_REGION_BAD_CAPACITY = 3,
    VIRTIO_DMA_REGION_ALREADY_PUBLISHED = 4,
    VIRTIO_DMA_REGION_UNPUBLISHED = 5,
    VIRTIO_DMA_REGION_INVALID_VERSION = 6,
    VIRTIO_DMA_REGION_INVALID_WINDOW = 7,
    VIRTIO_DMA_REGION_INVALID_ALIGNMENT = 8,
    VIRTIO_DMA_REGION_INVALID_LIFETIME = 9,
    VIRTIO_DMA_REGION_INVALID_DIRECTION = 10,
    VIRTIO_DMA_REGION_INVALID_ZEROIZATION = 11,
    VIRTIO_DMA_REGION_INVALID_RANGE = 12,
    VIRTIO_DMA_REGION_OVERFLOW = 13,
    VIRTIO_DMA_REGION_UNALIGNED = 14,
    VIRTIO_DMA_REGION_FORBIDDEN_RANGE = 15,
    VIRTIO_DMA_REGION_ALIAS = 16,
    VIRTIO_DMA_REGION_EXHAUSTED = 17,
    VIRTIO_DMA_REGION_BAD_TOKEN = 18,
    VIRTIO_DMA_REGION_STALE_TOKEN = 19,
    VIRTIO_DMA_REGION_ZEROIZE_REJECTED = 20,
    VIRTIO_DMA_REGION_LIVE_OWNERS = 21,
};

struct virtio_dma_window_policy {
    uint64_t base;
    uint64_t length;
    uint32_t alignment;
};

struct virtio_dma_region_request {
    uint64_t physical_base;
    uint64_t length;
    enum virtio_dma_direction direction;
    enum virtio_dma_lifetime lifetime;
    enum virtio_dma_zeroization zeroization;
};

struct virtio_dma_region {
    bool published;
    uint32_t version;
    uint32_t index;
    uint32_t generation;
    uint64_t physical_base;
    uint64_t physical_end;
    uint64_t length;
    enum virtio_dma_direction direction;
    enum virtio_dma_lifetime lifetime;
    enum virtio_dma_zeroization zeroization;
};

struct virtio_dma_region_token {
    uint32_t index;
    uint32_t generation;
};

struct virtio_dma_zeroize {
    bool (*zeroize)(
        void *context,
        uint64_t physical_base,
        uint64_t length);
    void *context;
};

struct virtio_dma_region_slot {
    bool bound;
    uint32_t generation;
    struct virtio_dma_region region;
};

struct virtio_dma_region_registry {
    bool published;
    uint32_t version;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t bound_count;
    struct virtio_dma_window_policy window;
    struct virtio_dma_zeroize zeroize;
    struct virtio_dma_region_slot
        slots[VIRTIO_DMA_REGION_CAPACITY_MAX];
};

enum virtio_dma_region_status virtio_dma_region_init(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_window_policy *window,
    const struct virtio_dma_zeroize *zeroize,
    uint32_t capacity);
enum virtio_dma_region_status virtio_dma_region_bind(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_request *request,
    struct virtio_dma_region_token *token,
    struct virtio_dma_region *region_out);
enum virtio_dma_region_status virtio_dma_region_unbind(
    struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_token *token);
enum virtio_dma_region_status virtio_dma_region_teardown(
    struct virtio_dma_region_registry *registry);
bool virtio_dma_region_published(
    const struct virtio_dma_region_registry *registry);
uint32_t virtio_dma_region_free_count(
    const struct virtio_dma_region_registry *registry);
uint32_t virtio_dma_region_bound_count(
    const struct virtio_dma_region_registry *registry);
const char *virtio_dma_region_status_string(
    enum virtio_dma_region_status status);

#endif
