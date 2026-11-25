#ifndef HFOC_VIRTIO_AVAILABLE_PUBLICATION_H
#define HFOC_VIRTIO_AVAILABLE_PUBLICATION_H

#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_AVAILABLE_PUBLICATION_VERSION 1U
#define VIRTIO_AVAILABLE_PUBLICATION_GENERATION_MIN 1U
#define VIRTIO_AVAILABLE_PUBLICATION_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MIN 1U
#define VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX 64U
#define VIRTIO_AVAILABLE_PUBLICATION_RING_ALIGNMENT 2U

struct virtio_available_ring {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile uint16_t ring[VIRTIO_QUEUE_SIZE_MAX];
    volatile uint16_t used_event;
};

struct virtio_available_barrier {
    void (*fence)(void *context);
    void *context;
};

struct virtio_available_token {
    uint32_t sequence;
    uint32_t generation;
};

struct virtio_available_publication {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t queue_size;
    bool event_index;
    uint32_t expected_idx;
    uint32_t published_count;
    uint32_t next_sequence;
    bool pending;
    uint32_t pending_sequence;
    uint32_t pending_length;
    uint16_t pending_chain[VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX];
    struct virtio_available_ring *ring;
    struct virtio_available_barrier barrier;
};

enum virtio_available_publication_status {
    VIRTIO_AVAILABLE_PUBLICATION_OK = 0,
    VIRTIO_AVAILABLE_PUBLICATION_NULL = 1,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION = 2,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION = 3,
    VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED = 4,
    VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED = 5,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT = 6,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER = 7,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE = 8,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN = 9,
    VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR = 10,
    VIRTIO_AVAILABLE_PUBLICATION_RING_FULL = 11,
    VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL = 12,
    VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING = 13,
    VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN = 14,
    VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED = 15,
    VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE = 16,
};

enum virtio_available_publication_status
virtio_available_publication_init(
    struct virtio_available_publication *controller,
    const struct virtio_queue_layout_result *layout,
    struct virtio_available_ring *ring,
    const struct virtio_available_barrier *barrier,
    uint32_t generation);
enum virtio_available_publication_status
virtio_available_publication_prepare(
    struct virtio_available_publication *controller,
    const uint16_t *chain,
    uint32_t chain_length,
    struct virtio_available_token *token);
enum virtio_available_publication_status
virtio_available_publication_publish(
    struct virtio_available_publication *controller,
    const struct virtio_available_token *token);
enum virtio_available_publication_status
virtio_available_publication_cancel(
    struct virtio_available_publication *controller,
    const struct virtio_available_token *token);
bool virtio_available_publication_published(
    const struct virtio_available_publication *controller);
uint32_t virtio_available_publication_visible_index(
    const struct virtio_available_publication *controller);
uint32_t virtio_available_publication_published_count(
    const struct virtio_available_publication *controller);
uint32_t virtio_available_publication_pending_length(
    const struct virtio_available_publication *controller);
const char *virtio_available_publication_status_string(
    enum virtio_available_publication_status status);

#endif
