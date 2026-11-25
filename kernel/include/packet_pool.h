#ifndef HFOC_PACKET_POOL_H
#define HFOC_PACKET_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKET_POOL_VERSION 1U
#define PACKET_POOL_COUNT_MIN 1U
#define PACKET_POOL_COUNT_MAX 64U
#define PACKET_POOL_CAPACITY_MIN 1U
#define PACKET_POOL_CAPACITY_MAX 2048U
#define PACKET_POOL_HEADROOM_MAX 255U
#define PACKET_POOL_LENGTH_MIN 1U
#define PACKET_POOL_LENGTH_MAX 2048U
#define PACKET_POOL_STORAGE_SIZE_MAX 65536U

enum packet_pool_owner {
    PACKET_POOL_OWNER_FREE = 0,
    PACKET_POOL_OWNER_RESERVED = 1,
    PACKET_POOL_OWNER_ACTIVE = 2,
};

enum packet_pool_direction {
    PACKET_POOL_DIRECTION_NONE = 0,
    PACKET_POOL_DIRECTION_RX = 1,
    PACKET_POOL_DIRECTION_TX = 2,
};

enum packet_pool_status {
    PACKET_POOL_OK = 0,
    PACKET_POOL_NULL = 1,
    PACKET_POOL_BAD_STATE = 2,
    PACKET_POOL_BAD_COUNT = 3,
    PACKET_POOL_BAD_CAPACITY = 4,
    PACKET_POOL_BAD_HEADROOM = 5,
    PACKET_POOL_BAD_LENGTH = 6,
    PACKET_POOL_BAD_STORAGE = 7,
    PACKET_POOL_OVERFLOW = 8,
    PACKET_POOL_BAD_SLOT = 9,
    PACKET_POOL_EXHAUSTED = 10,
    PACKET_POOL_BAD_DIRECTION = 11,
    PACKET_POOL_STALE_DESCRIPTOR = 12,
    PACKET_POOL_WRONG_OWNER = 13,
    PACKET_POOL_DOUBLE_FREE = 14,
};

struct packet_pool_slot_descriptor {
    uint32_t slot;
    uint32_t generation;
    enum packet_pool_owner owner;
    enum packet_pool_direction direction;
};

struct packet_pool_descriptor {
    uint32_t version;
    uint32_t count;
    uint32_t capacity;
    uint32_t headroom;
    uint32_t length;
    size_t storage_size;
    uint8_t *storage;
    uint32_t generations[PACKET_POOL_COUNT_MAX];
    uint8_t owners[PACKET_POOL_COUNT_MAX];
    uint8_t directions[PACKET_POOL_COUNT_MAX];
    uint32_t free_count;
    uint32_t reserved_count;
    uint32_t active_count;
    bool published;
};

enum packet_pool_status packet_pool_init(
    struct packet_pool_descriptor *pool,
    void *storage,
    size_t storage_size,
    uint32_t count,
    uint32_t capacity,
    uint32_t headroom,
    uint32_t length);
bool packet_pool_is_published(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_count(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_capacity(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_headroom(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_length(
    const struct packet_pool_descriptor *pool);
size_t packet_pool_storage_size(
    const struct packet_pool_descriptor *pool);
enum packet_pool_status packet_pool_slot(
    const struct packet_pool_descriptor *pool,
    uint32_t slot,
    struct packet_pool_slot_descriptor *descriptor);
enum packet_pool_status packet_pool_reserve(
    struct packet_pool_descriptor *pool,
    enum packet_pool_direction direction,
    struct packet_pool_slot_descriptor *descriptor);
enum packet_pool_status packet_pool_commit(
    struct packet_pool_descriptor *pool,
    struct packet_pool_slot_descriptor *descriptor);
enum packet_pool_status packet_pool_transfer(
    struct packet_pool_descriptor *pool,
    struct packet_pool_slot_descriptor *descriptor,
    enum packet_pool_direction direction);
enum packet_pool_status packet_pool_release(
    struct packet_pool_descriptor *pool,
    const struct packet_pool_slot_descriptor *descriptor);
uint32_t packet_pool_free_count(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_reserved_count(
    const struct packet_pool_descriptor *pool);
uint32_t packet_pool_active_count(
    const struct packet_pool_descriptor *pool);
const char *packet_pool_status_string(
    enum packet_pool_status status);

#endif
