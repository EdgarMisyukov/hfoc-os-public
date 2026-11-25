#ifndef HFOC_VIRTIO_USED_CONSUMPTION_H
#define HFOC_VIRTIO_USED_CONSUMPTION_H

#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_USED_CONSUMPTION_VERSION 1U
#define VIRTIO_USED_CONSUMPTION_GENERATION_MIN 1U
#define VIRTIO_USED_CONSUMPTION_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_USED_CONSUMPTION_LENGTH_MIN 0U
#define VIRTIO_USED_CONSUMPTION_LENGTH_MAX 1048576U

struct virtio_used_element {
    volatile uint32_t id;
    volatile uint32_t len;
};

struct virtio_used_ring {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile struct virtio_used_element ring[VIRTIO_QUEUE_SIZE_MAX];
    volatile uint16_t used_event;
};

struct virtio_used_token {
    uint32_t sequence;
    uint32_t generation;
};

struct virtio_used_reclaim {
    bool (*reclaim)(
        void *context,
        const struct virtio_used_element *element,
        const struct virtio_used_token *token);
    void *context;
};

struct virtio_used_consumption {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t queue_size;
    bool event_index;
    uint32_t max_length;
    uint32_t expected_idx;
    uint32_t consumed_count;
    uint32_t next_sequence;
    bool pending;
    uint32_t pending_sequence;
    struct virtio_used_element pending_element;
    struct virtio_used_ring *ring;
    struct virtio_used_reclaim reclaim;
};

enum virtio_used_consumption_status {
    VIRTIO_USED_CONSUMPTION_OK = 0,
    VIRTIO_USED_CONSUMPTION_NULL = 1,
    VIRTIO_USED_CONSUMPTION_INVALID_VERSION = 2,
    VIRTIO_USED_CONSUMPTION_INVALID_GENERATION = 3,
    VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED = 4,
    VIRTIO_USED_CONSUMPTION_UNPUBLISHED = 5,
    VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT = 6,
    VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE = 7,
    VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH = 8,
    VIRTIO_USED_CONSUMPTION_INVALID_RING = 9,
    VIRTIO_USED_CONSUMPTION_INVALID_RELEASE = 10,
    VIRTIO_USED_CONSUMPTION_EMPTY = 11,
    VIRTIO_USED_CONSUMPTION_RING_OVERFLOW = 12,
    VIRTIO_USED_CONSUMPTION_INVALID_HEAD = 13,
    VIRTIO_USED_CONSUMPTION_INVALID_LENGTH = 14,
    VIRTIO_USED_CONSUMPTION_PENDING_FULL = 15,
    VIRTIO_USED_CONSUMPTION_NO_PENDING = 16,
    VIRTIO_USED_CONSUMPTION_STALE_TOKEN = 17,
    VIRTIO_USED_CONSUMPTION_INDEX_CHANGED = 18,
    VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED = 19,
    VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED = 20,
    VIRTIO_USED_CONSUMPTION_INVALID_STATE = 21,
};

enum virtio_used_consumption_status
virtio_used_consumption_init(
    struct virtio_used_consumption *controller,
    const struct virtio_queue_layout_result *layout,
    struct virtio_used_ring *ring,
    const struct virtio_used_reclaim *reclaim,
    uint32_t generation,
    uint32_t max_length);
enum virtio_used_consumption_status
virtio_used_consumption_prepare(
    struct virtio_used_consumption *controller,
    struct virtio_used_element *entry,
    struct virtio_used_token *token);
enum virtio_used_consumption_status
virtio_used_consumption_consume(
    struct virtio_used_consumption *controller,
    const struct virtio_used_token *token);
enum virtio_used_consumption_status
virtio_used_consumption_cancel(
    struct virtio_used_consumption *controller,
    const struct virtio_used_token *token);
bool virtio_used_consumption_published(
    const struct virtio_used_consumption *controller);
uint32_t virtio_used_consumption_visible_index(
    const struct virtio_used_consumption *controller);
uint32_t virtio_used_consumption_consumed_count(
    const struct virtio_used_consumption *controller);
uint32_t virtio_used_consumption_pending_head(
    const struct virtio_used_consumption *controller);
const char *virtio_used_consumption_status_string(
    enum virtio_used_consumption_status status);

#endif
