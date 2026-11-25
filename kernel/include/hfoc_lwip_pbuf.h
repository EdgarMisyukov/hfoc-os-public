#ifndef HFOC_LWIP_PBUF_H
#define HFOC_LWIP_PBUF_H

#include "lwip/pbuf.h"
#include "packet_pool.h"
#include "packet_slice.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_PBUF_VERSION 1U
#define HFOC_LWIP_PBUF_SLOT_COUNT 8U
#define HFOC_LWIP_PBUF_CHAIN_MAX 4U
#define HFOC_LWIP_PBUF_TOT_LEN_MAX 2048U

enum hfoc_lwip_pbuf_result {
    HFOC_LWIP_PBUF_ACCEPTED = 0,
    HFOC_LWIP_PBUF_BACKPRESSURE = 1,
    HFOC_LWIP_PBUF_REJECTED = 2,
};

enum hfoc_lwip_pbuf_status {
    HFOC_LWIP_PBUF_OK = 0,
    HFOC_LWIP_PBUF_NULL = 1,
    HFOC_LWIP_PBUF_BAD_STATE = 2,
    HFOC_LWIP_PBUF_INVALID_VERSION = 3,
    HFOC_LWIP_PBUF_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_PBUF_UNPUBLISHED = 5,
    HFOC_LWIP_PBUF_BAD_POOL = 6,
    HFOC_LWIP_PBUF_BAD_SLICE = 7,
    HFOC_LWIP_PBUF_WRONG_DIRECTION = 8,
    HFOC_LWIP_PBUF_STALE_DESCRIPTOR = 9,
    HFOC_LWIP_PBUF_INVALID_LENGTH = 10,
    HFOC_LWIP_PBUF_INVALID_CHAIN = 11,
    HFOC_LWIP_PBUF_EXHAUSTED = 12,
    HFOC_LWIP_PBUF_LIVE_CHAIN = 13,
    HFOC_LWIP_PBUF_STALE_CHAIN = 14,
    HFOC_LWIP_PBUF_COPY_FAILED = 15,
};

struct hfoc_lwip_pbuf_packet_import {
    bool imported;
    uint32_t generation;
};

struct hfoc_lwip_pbuf_bridge {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t used_count;
    struct packet_pool_descriptor *pool;
    struct hfoc_lwip_pbuf_packet_import
        packet_imports[PACKET_POOL_COUNT_MAX];
    uint32_t slot_generations[HFOC_LWIP_PBUF_SLOT_COUNT];
    bool used[HFOC_LWIP_PBUF_SLOT_COUNT];
    struct packet_pool_slot_descriptor
        packet_slots[HFOC_LWIP_PBUF_SLOT_COUNT];
    struct pbuf_custom pbufs[HFOC_LWIP_PBUF_SLOT_COUNT];
};

struct hfoc_lwip_pbuf_chain {
    uint32_t version;
    uint32_t bridge_generation;
    uint32_t slot;
    uint32_t slot_generation;
    struct packet_pool_slot_descriptor packet;
    struct pbuf *head;
    bool published;
};

enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_bridge_init(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct packet_pool_descriptor *pool);
enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_bridge_shutdown(
    struct hfoc_lwip_pbuf_bridge *bridge);
bool hfoc_lwip_pbuf_bridge_published(
    const struct hfoc_lwip_pbuf_bridge *bridge);
uint32_t hfoc_lwip_pbuf_bridge_used_count(
    const struct hfoc_lwip_pbuf_bridge *bridge);
uint32_t hfoc_lwip_pbuf_bridge_free_count(
    const struct hfoc_lwip_pbuf_bridge *bridge);
enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_import_rx(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct packet_slice *slice,
    struct hfoc_lwip_pbuf_chain *chain,
    enum hfoc_lwip_pbuf_result *result);
enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_export_tx(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct pbuf *chain,
    struct packet_slice *slice,
    enum hfoc_lwip_pbuf_result *result);
enum hfoc_lwip_pbuf_status hfoc_lwip_pbuf_release_chain(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct hfoc_lwip_pbuf_chain *chain);
const char *hfoc_lwip_pbuf_result_string(
    enum hfoc_lwip_pbuf_result result);
const char *hfoc_lwip_pbuf_status_string(
    enum hfoc_lwip_pbuf_status status);

#endif
