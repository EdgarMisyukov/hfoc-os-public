#ifndef HFOC_LWIP_TCP_POOL_H
#define HFOC_LWIP_TCP_POOL_H

#include "hfoc_lwip_tcp_conn.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_POOL_VERSION 1U
#define HFOC_LWIP_TCP_POOL_SLOT_COUNT 4U
#define HFOC_LWIP_TCP_POOL_GENERATION_MIN 1U
#define HFOC_LWIP_TCP_POOL_GENERATION_MAX UINT32_C(0xFFFFFFFE)

enum hfoc_lwip_tcp_pool_state {
    HFOC_LWIP_TCP_POOL_STATE_FREE = 0,
    HFOC_LWIP_TCP_POOL_STATE_RESERVED = 1,
    HFOC_LWIP_TCP_POOL_STATE_PUBLISHED = 2,
};

enum hfoc_lwip_tcp_pool_status {
    HFOC_LWIP_TCP_POOL_OK = 0,
    HFOC_LWIP_TCP_POOL_NULL = 1,
    HFOC_LWIP_TCP_POOL_BAD_STATE = 2,
    HFOC_LWIP_TCP_POOL_EXHAUSTED = 3,
    HFOC_LWIP_TCP_POOL_BAD_INDEX = 4,
    HFOC_LWIP_TCP_POOL_STALE_TOKEN = 5,
    HFOC_LWIP_TCP_POOL_WRONG_STATE = 6,
    HFOC_LWIP_TCP_POOL_DOUBLE_FREE = 7,
    HFOC_LWIP_TCP_POOL_CONN_FAILED = 8,
    HFOC_LWIP_TCP_POOL_CLOSE_FAILED = 9,
    HFOC_LWIP_TCP_POOL_ALREADY_CLOSED = 10,
    HFOC_LWIP_TCP_POOL_LIVE_CONNECTION = 11,
};

struct hfoc_lwip_tcp_pool_token {
    uint32_t index;
    uint32_t generation;
};

struct hfoc_lwip_tcp_pool_slot {
    enum hfoc_lwip_tcp_pool_state state;
    uint32_t generation;
    struct hfoc_lwip_tcp_connection connection;
};

struct hfoc_lwip_tcp_pool {
    bool published;
    uint32_t version;
    uint32_t free_count;
    uint32_t reserved_count;
    uint32_t published_count;
    struct hfoc_lwip_tcp_pool_slot
        slots[HFOC_LWIP_TCP_POOL_SLOT_COUNT];
};

enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_init(
    struct hfoc_lwip_tcp_pool *pool);
enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_reserve(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_pool_token *token);
enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_publish(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_conn_callbacks *callbacks);
enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_close(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token);
enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_abort(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token);
enum hfoc_lwip_tcp_pool_status hfoc_lwip_tcp_pool_release(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_pool_published(
    const struct hfoc_lwip_tcp_pool *pool);
uint32_t hfoc_lwip_tcp_pool_free_count(
    const struct hfoc_lwip_tcp_pool *pool);
uint32_t hfoc_lwip_tcp_pool_reserved_count(
    const struct hfoc_lwip_tcp_pool *pool);
uint32_t hfoc_lwip_tcp_pool_published_count(
    const struct hfoc_lwip_tcp_pool *pool);
uint32_t hfoc_lwip_tcp_pool_generation(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index);
enum hfoc_lwip_tcp_pool_state hfoc_lwip_tcp_pool_state(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index);
const struct hfoc_lwip_tcp_connection *hfoc_lwip_tcp_pool_connection(
    const struct hfoc_lwip_tcp_pool *pool,
    uint32_t index);
const char *hfoc_lwip_tcp_pool_state_string(
    enum hfoc_lwip_tcp_pool_state state);
const char *hfoc_lwip_tcp_pool_status_string(
    enum hfoc_lwip_tcp_pool_status status);

#endif
