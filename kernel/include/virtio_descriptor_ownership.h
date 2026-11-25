#ifndef HFOC_VIRTIO_DESCRIPTOR_OWNERSHIP_H
#define HFOC_VIRTIO_DESCRIPTOR_OWNERSHIP_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_DESCRIPTOR_OWNERSHIP_VERSION 1U
#define VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MIN 1U
#define VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX 64U

enum virtio_descriptor_state {
    VIRTIO_DESCRIPTOR_STATE_FREE = 0,
    VIRTIO_DESCRIPTOR_STATE_RESERVED = 1,
    VIRTIO_DESCRIPTOR_STATE_PUBLISHED = 2,
};

enum virtio_descriptor_ownership_status {
    VIRTIO_DESCRIPTOR_OWNERSHIP_OK = 0,
    VIRTIO_DESCRIPTOR_OWNERSHIP_NULL = 1,
    VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE = 2,
    VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY = 3,
    VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED = 4,
    VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR = 5,
    VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR = 6,
    VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE = 7,
    VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE = 8,
};

struct virtio_descriptor_token {
    uint32_t index;
    uint32_t generation;
};

struct virtio_descriptor_ownership {
    bool published;
    uint32_t version;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t reserved_count;
    uint32_t published_count;
    uint32_t freelist[VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX];
    uint32_t freelist_top;
    uint8_t states[VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX];
    uint32_t generations[VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX];
};

enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_init(
    struct virtio_descriptor_ownership *pool,
    uint32_t capacity);
enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_reserve(
    struct virtio_descriptor_ownership *pool,
    struct virtio_descriptor_token *token);
enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_publish(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token);
enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_rollback(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token);
enum virtio_descriptor_ownership_status
virtio_descriptor_ownership_consume(
    struct virtio_descriptor_ownership *pool,
    const struct virtio_descriptor_token *token);
bool virtio_descriptor_ownership_published(
    const struct virtio_descriptor_ownership *pool);
uint32_t virtio_descriptor_ownership_free_count(
    const struct virtio_descriptor_ownership *pool);
uint32_t virtio_descriptor_ownership_reserved_count(
    const struct virtio_descriptor_ownership *pool);
uint32_t virtio_descriptor_ownership_published_count(
    const struct virtio_descriptor_ownership *pool);
const char *virtio_descriptor_ownership_status_string(
    enum virtio_descriptor_ownership_status status);

#endif
