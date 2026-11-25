#ifndef HFOC_LWIP_TCP_BACKLOG_H
#define HFOC_LWIP_TCP_BACKLOG_H

#include "hfoc_lwip_tcp.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_BACKLOG_VERSION 1U
#define HFOC_LWIP_TCP_BACKLOG_CAPACITY HFOC_LWIP_TCP_BACKLOG
#define HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN 1U
#define HFOC_LWIP_TCP_BACKLOG_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HFOC_LWIP_TCP_BACKLOG_NONE UINT32_MAX

enum hfoc_lwip_tcp_backlog_state {
    HFOC_LWIP_TCP_BACKLOG_STATE_FREE = 0,
    HFOC_LWIP_TCP_BACKLOG_STATE_PENDING = 1,
};

enum hfoc_lwip_tcp_backlog_status {
    HFOC_LWIP_TCP_BACKLOG_OK = 0,
    HFOC_LWIP_TCP_BACKLOG_NULL = 1,
    HFOC_LWIP_TCP_BACKLOG_BAD_STATE = 2,
    HFOC_LWIP_TCP_BACKLOG_FULL = 3,
    HFOC_LWIP_TCP_BACKLOG_EMPTY = 4,
    HFOC_LWIP_TCP_BACKLOG_INVALID_PCB = 5,
    HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB = 6,
    HFOC_LWIP_TCP_BACKLOG_GENERATION_EXHAUSTED = 7,
};

struct hfoc_lwip_tcp_backlog_token {
    uint32_t index;
    uint32_t generation;
};

struct hfoc_lwip_tcp_backlog_entry {
    enum hfoc_lwip_tcp_backlog_state state;
    uint32_t generation;
    struct tcp_pcb *pcb;
};

struct hfoc_lwip_tcp_backlog {
    bool published;
    uint32_t version;
    uint32_t capacity;
    uint32_t pending_count;
    uint32_t head;
    uint32_t tail;
    struct hfoc_lwip_tcp_backlog_entry
        entries[HFOC_LWIP_TCP_BACKLOG_CAPACITY];
};

enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_init(
    struct hfoc_lwip_tcp_backlog *backlog);
enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_admit(
    struct hfoc_lwip_tcp_backlog *backlog,
    struct tcp_pcb *pcb,
    struct hfoc_lwip_tcp_backlog_token *token);
enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_drain(
    struct hfoc_lwip_tcp_backlog *backlog,
    struct hfoc_lwip_tcp_backlog_token *token,
    struct tcp_pcb **pcb);
bool hfoc_lwip_tcp_backlog_published(
    const struct hfoc_lwip_tcp_backlog *backlog);
uint32_t hfoc_lwip_tcp_backlog_capacity(
    const struct hfoc_lwip_tcp_backlog *backlog);
uint32_t hfoc_lwip_tcp_backlog_pending_count(
    const struct hfoc_lwip_tcp_backlog *backlog);
const char *hfoc_lwip_tcp_backlog_state_string(
    enum hfoc_lwip_tcp_backlog_state state);
const char *hfoc_lwip_tcp_backlog_status_string(
    enum hfoc_lwip_tcp_backlog_status status);

#endif
