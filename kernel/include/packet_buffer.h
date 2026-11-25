#ifndef HFOC_PACKET_BUFFER_H
#define HFOC_PACKET_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKET_BUFFER_POOL_VERSION 1U
#define PACKET_BUFFER_CAPACITY_MIN 1U
#define PACKET_BUFFER_CAPACITY_MAX 64U
#define PACKET_BUFFER_PAYLOAD_MIN 1U
#define PACKET_BUFFER_PAYLOAD_MAX 2048U

enum packet_buffer_owner {
    PACKET_BUFFER_OWNER_FREE = 0,
    PACKET_BUFFER_OWNER_RX = 1,
    PACKET_BUFFER_OWNER_TX = 2,
};

enum packet_buffer_status {
    PACKET_BUFFER_OK = 0,
    PACKET_BUFFER_NULL = 1,
    PACKET_BUFFER_BAD_STATE = 2,
    PACKET_BUFFER_BAD_CAPACITY = 3,
    PACKET_BUFFER_BAD_PAYLOAD = 4,
    PACKET_BUFFER_BAD_STORAGE = 5,
    PACKET_BUFFER_EXHAUSTED = 6,
    PACKET_BUFFER_BAD_DESCRIPTOR = 7,
    PACKET_BUFFER_STALE_DESCRIPTOR = 8,
    PACKET_BUFFER_WRONG_OWNER = 9,
    PACKET_BUFFER_DOUBLE_FREE = 10,
};

struct packet_buffer_descriptor {
    uint32_t slot;
    uint32_t generation;
};

struct packet_buffer_pool {
    uint32_t version;
    uint32_t capacity;
    uint32_t payload_size;
    uint32_t free_count;
    uint8_t *storage;
    uint32_t generations[PACKET_BUFFER_CAPACITY_MAX];
    uint8_t owners[PACKET_BUFFER_CAPACITY_MAX];
    bool published;
};

enum packet_buffer_status packet_buffer_pool_init(
    struct packet_buffer_pool *pool,
    void *storage,
    size_t storage_size,
    uint32_t capacity,
    uint32_t payload_size);
enum packet_buffer_status packet_buffer_acquire_rx(
    struct packet_buffer_pool *pool,
    struct packet_buffer_descriptor *descriptor);
enum packet_buffer_status packet_buffer_acquire_tx(
    struct packet_buffer_pool *pool,
    struct packet_buffer_descriptor *descriptor);
enum packet_buffer_status packet_buffer_handoff_tx(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor);
enum packet_buffer_status packet_buffer_release(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor);
void *packet_buffer_data(
    struct packet_buffer_pool *pool,
    const struct packet_buffer_descriptor *descriptor);
uint32_t packet_buffer_free_count(
    const struct packet_buffer_pool *pool);
uint32_t packet_buffer_capacity(
    const struct packet_buffer_pool *pool);
const char *packet_buffer_status_string(
    enum packet_buffer_status status);

#endif
