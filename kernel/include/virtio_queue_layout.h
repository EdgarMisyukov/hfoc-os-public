#ifndef HFOC_VIRTIO_QUEUE_LAYOUT_H
#define HFOC_VIRTIO_QUEUE_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_QUEUE_LAYOUT_VERSION 1U
#define VIRTIO_QUEUE_LAYOUT_RESULT_VERSION 1U
#define VIRTIO_QUEUE_SIZE_MIN 1U
#define VIRTIO_QUEUE_SIZE_MAX 32768U
#define VIRTIO_QUEUE_LAYOUT_ALIGNMENT 16U
#define VIRTIO_QUEUE_DESCRIPTOR_ELEMENT_SIZE 16U
#define VIRTIO_QUEUE_AVAILABLE_ELEMENT_SIZE 2U
#define VIRTIO_QUEUE_USED_ELEMENT_SIZE 8U
#define VIRTIO_QUEUE_RING_FIXED_SIZE 4U
#define VIRTIO_QUEUE_EVENT_INDEX_SIZE 2U
#define VIRTIO_QUEUE_DESCRIPTOR_ALIGNMENT 16U
#define VIRTIO_QUEUE_AVAILABLE_ALIGNMENT 2U
#define VIRTIO_QUEUE_USED_ALIGNMENT 4U

struct virtio_queue_layout_policy {
    uint32_t queue_size;
    bool event_index;
};

struct virtio_queue_ring_layout {
    uint64_t offset;
    uint64_t size;
    uint32_t alignment;
};

struct virtio_queue_layout_result {
    bool published;
    uint32_t version;
    uint32_t queue_size;
    bool event_index;
    uint64_t region_base;
    uint64_t region_length;
    struct virtio_queue_ring_layout descriptor;
    struct virtio_queue_ring_layout available;
    struct virtio_queue_ring_layout used;
    uint64_t total_size;
};

enum virtio_queue_layout_status {
    VIRTIO_QUEUE_LAYOUT_OK = 0,
    VIRTIO_QUEUE_LAYOUT_NULL = 1,
    VIRTIO_QUEUE_LAYOUT_UNPUBLISHED = 2,
    VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION = 3,
    VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE = 4,
    VIRTIO_QUEUE_LAYOUT_INVALID_RANGE = 5,
    VIRTIO_QUEUE_LAYOUT_OVERFLOW = 6,
    VIRTIO_QUEUE_LAYOUT_UNALIGNED = 7,
    VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL = 8,
    VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT = 9,
};

enum virtio_queue_layout_status virtio_queue_layout_calculate(
    const struct virtio_queue_layout_policy *policy,
    uint64_t region_base,
    uint64_t region_length,
    struct virtio_queue_layout_result *result);
enum virtio_queue_layout_status virtio_queue_layout_result_validate(
    const struct virtio_queue_layout_result *result);
bool virtio_queue_layout_result_published(
    const struct virtio_queue_layout_result *result);
const char *virtio_queue_layout_status_string(
    enum virtio_queue_layout_status status);

#endif
