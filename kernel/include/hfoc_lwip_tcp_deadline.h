#ifndef HFOC_LWIP_TCP_DEADLINE_H
#define HFOC_LWIP_TCP_DEADLINE_H

#include "hfoc_lwip_tcp_pool.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_DEADLINE_VERSION 1U
#define HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT \
    HFOC_LWIP_TCP_POOL_SLOT_COUNT
#define HFOC_LWIP_TCP_DEADLINE_DISABLED UINT32_MAX
#define HFOC_LWIP_TCP_DEADLINE_NEXT_NONE UINT32_MAX
#define HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX (UINT32_MAX / 4U)

enum hfoc_lwip_tcp_deadline_kind {
    HFOC_LWIP_TCP_DEADLINE_KIND_NONE = 0,
    HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT = 1,
    HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS = 2,
    HFOC_LWIP_TCP_DEADLINE_KIND_IDLE = 3,
    HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE = 4,
};

enum hfoc_lwip_tcp_deadline_status {
    HFOC_LWIP_TCP_DEADLINE_OK = 0,
    HFOC_LWIP_TCP_DEADLINE_NULL = 1,
    HFOC_LWIP_TCP_DEADLINE_BAD_STATE = 2,
    HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN = 3,
    HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN = 4,
    HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED = 5,
    HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED = 6,
    HFOC_LWIP_TCP_DEADLINE_CLOSED = 7,
    HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY = 8,
    HFOC_LWIP_TCP_DEADLINE_EXPIRED = 9,
};

struct hfoc_lwip_tcp_deadline_policy {
    uint32_t version;
    uint32_t accept_timeout_ms;
    uint32_t progress_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t close_timeout_ms;
};

struct hfoc_lwip_tcp_deadline_entry {
    bool active;
    bool closing;
    uint32_t generation;
    uint32_t accept_deadline_ms;
    uint32_t progress_deadline_ms;
    uint32_t idle_deadline_ms;
    uint32_t close_deadline_ms;
    struct hfoc_lwip_tcp_deadline_policy policy;
};

struct hfoc_lwip_tcp_deadline_scheduler {
    bool published;
    uint32_t version;
    uint32_t active_count;
    struct hfoc_lwip_tcp_deadline_entry
        entries[HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT];
};

struct hfoc_lwip_tcp_deadline_snapshot {
    uint32_t version;
    uint32_t active_count;
    uint32_t next_deadline_ms;
};

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_init(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler);
enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_attach(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_deadline_policy *policy,
    uint32_t now_ms);
enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_touch(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms);
enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_begin_close(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms);
enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_check(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms,
    enum hfoc_lwip_tcp_deadline_kind *expired_kind);
enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_cancel(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token);
bool hfoc_lwip_tcp_deadline_published(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler);
uint32_t hfoc_lwip_tcp_deadline_active_count(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler);
uint32_t hfoc_lwip_tcp_deadline_next_deadline_ms(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    uint32_t now_ms);
struct hfoc_lwip_tcp_deadline_snapshot
hfoc_lwip_tcp_deadline_snapshot(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    uint32_t now_ms);
const char *hfoc_lwip_tcp_deadline_kind_string(
    enum hfoc_lwip_tcp_deadline_kind kind);
const char *hfoc_lwip_tcp_deadline_status_string(
    enum hfoc_lwip_tcp_deadline_status status);

#endif
