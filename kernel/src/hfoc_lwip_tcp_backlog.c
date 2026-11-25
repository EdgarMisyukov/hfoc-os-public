#include "hfoc_lwip_tcp_backlog.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_BACKLOG_HOST_TEST) || \
    defined(HFOC_TCP_LISTENER_QEMU_TEST)

static bool hfoc_lwip_tcp_backlog_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_BACKLOG_GENERATION_MAX;
}

static uint32_t hfoc_lwip_tcp_backlog_next_generation(
    uint32_t generation)
{
    return generation == HFOC_LWIP_TCP_BACKLOG_GENERATION_MAX
        ? 0U
        : generation + 1U;
}

static bool hfoc_lwip_tcp_backlog_valid(
    const struct hfoc_lwip_tcp_backlog *backlog)
{
    uint32_t pending_state_count;
    uint32_t index;

    if (backlog == NULL ||
        !backlog->published ||
        backlog->version != HFOC_LWIP_TCP_BACKLOG_VERSION ||
        backlog->capacity != HFOC_LWIP_TCP_BACKLOG_CAPACITY ||
        backlog->pending_count > HFOC_LWIP_TCP_BACKLOG_CAPACITY ||
        backlog->head >= HFOC_LWIP_TCP_BACKLOG_CAPACITY ||
        backlog->tail >= HFOC_LWIP_TCP_BACKLOG_CAPACITY) {
        return false;
    }

    pending_state_count = 0U;
    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        const struct hfoc_lwip_tcp_backlog_entry *entry =
            &backlog->entries[index];

        if (!hfoc_lwip_tcp_backlog_generation_valid(
                entry->generation)) {
            return false;
        }
        if (entry->state == HFOC_LWIP_TCP_BACKLOG_STATE_FREE) {
            if (entry->pcb != NULL) {
                return false;
            }
            continue;
        }
        if (entry->state != HFOC_LWIP_TCP_BACKLOG_STATE_PENDING ||
            entry->pcb == NULL ||
            entry->pcb->state == LISTEN) {
            return false;
        }
        ++pending_state_count;
    }

    return pending_state_count == backlog->pending_count;
}

enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_init(
    struct hfoc_lwip_tcp_backlog *backlog)
{
    uint32_t index;

    if (backlog == NULL) {
        return HFOC_LWIP_TCP_BACKLOG_NULL;
    }
    if (backlog->published) {
        return HFOC_LWIP_TCP_BACKLOG_BAD_STATE;
    }

    memset(backlog, 0, sizeof(*backlog));
    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        backlog->entries[index].state =
            HFOC_LWIP_TCP_BACKLOG_STATE_FREE;
        backlog->entries[index].generation =
            HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN;
    }
    backlog->version = HFOC_LWIP_TCP_BACKLOG_VERSION;
    backlog->capacity = HFOC_LWIP_TCP_BACKLOG_CAPACITY;
    backlog->published = true;
    return HFOC_LWIP_TCP_BACKLOG_OK;
}

enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_admit(
    struct hfoc_lwip_tcp_backlog *backlog,
    struct tcp_pcb *pcb,
    struct hfoc_lwip_tcp_backlog_token *token)
{
    struct hfoc_lwip_tcp_backlog_entry *entry;
    uint32_t index;

    if (backlog == NULL || pcb == NULL || token == NULL) {
        return HFOC_LWIP_TCP_BACKLOG_NULL;
    }
    if (!hfoc_lwip_tcp_backlog_valid(backlog)) {
        return HFOC_LWIP_TCP_BACKLOG_BAD_STATE;
    }
    if (pcb->state == LISTEN) {
        return HFOC_LWIP_TCP_BACKLOG_INVALID_PCB;
    }
    if (backlog->pending_count ==
        HFOC_LWIP_TCP_BACKLOG_CAPACITY) {
        return HFOC_LWIP_TCP_BACKLOG_FULL;
    }

    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        const struct hfoc_lwip_tcp_backlog_entry *candidate =
            &backlog->entries[index];

        if (candidate->state ==
                HFOC_LWIP_TCP_BACKLOG_STATE_PENDING &&
            candidate->pcb == pcb) {
            return HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB;
        }
    }

    entry = &backlog->entries[backlog->tail];
    if (entry->state != HFOC_LWIP_TCP_BACKLOG_STATE_FREE) {
        return HFOC_LWIP_TCP_BACKLOG_BAD_STATE;
    }

    entry->state = HFOC_LWIP_TCP_BACKLOG_STATE_PENDING;
    entry->pcb = pcb;
    token->index = backlog->tail;
    token->generation = entry->generation;
    tcp_backlog_delayed(pcb);
    ++backlog->pending_count;
    backlog->tail = (backlog->tail + 1U) %
        HFOC_LWIP_TCP_BACKLOG_CAPACITY;
    return HFOC_LWIP_TCP_BACKLOG_OK;
}

enum hfoc_lwip_tcp_backlog_status hfoc_lwip_tcp_backlog_drain(
    struct hfoc_lwip_tcp_backlog *backlog,
    struct hfoc_lwip_tcp_backlog_token *token,
    struct tcp_pcb **pcb)
{
    struct hfoc_lwip_tcp_backlog_entry *entry;
    uint32_t next_generation;

    if (backlog == NULL || token == NULL || pcb == NULL) {
        return HFOC_LWIP_TCP_BACKLOG_NULL;
    }
    if (!hfoc_lwip_tcp_backlog_valid(backlog)) {
        return HFOC_LWIP_TCP_BACKLOG_BAD_STATE;
    }
    if (backlog->pending_count == 0U) {
        return HFOC_LWIP_TCP_BACKLOG_EMPTY;
    }

    entry = &backlog->entries[backlog->head];
    if (entry->state != HFOC_LWIP_TCP_BACKLOG_STATE_PENDING ||
        entry->pcb == NULL) {
        return HFOC_LWIP_TCP_BACKLOG_BAD_STATE;
    }

    next_generation =
        hfoc_lwip_tcp_backlog_next_generation(entry->generation);
    if (next_generation == 0U) {
        return HFOC_LWIP_TCP_BACKLOG_GENERATION_EXHAUSTED;
    }

    token->index = backlog->head;
    token->generation = entry->generation;
    *pcb = entry->pcb;
    tcp_backlog_accepted(entry->pcb);
    entry->state = HFOC_LWIP_TCP_BACKLOG_STATE_FREE;
    entry->generation = next_generation;
    entry->pcb = NULL;
    --backlog->pending_count;
    backlog->head = (backlog->head + 1U) %
        HFOC_LWIP_TCP_BACKLOG_CAPACITY;
    return HFOC_LWIP_TCP_BACKLOG_OK;
}

bool hfoc_lwip_tcp_backlog_published(
    const struct hfoc_lwip_tcp_backlog *backlog)
{
    return hfoc_lwip_tcp_backlog_valid(backlog);
}

uint32_t hfoc_lwip_tcp_backlog_capacity(
    const struct hfoc_lwip_tcp_backlog *backlog)
{
    return hfoc_lwip_tcp_backlog_valid(backlog)
        ? backlog->capacity
        : 0U;
}

uint32_t hfoc_lwip_tcp_backlog_pending_count(
    const struct hfoc_lwip_tcp_backlog *backlog)
{
    return hfoc_lwip_tcp_backlog_valid(backlog)
        ? backlog->pending_count
        : 0U;
}

const char *hfoc_lwip_tcp_backlog_state_string(
    enum hfoc_lwip_tcp_backlog_state state)
{
    switch (state) {
    case HFOC_LWIP_TCP_BACKLOG_STATE_FREE:
        return "HFOC_LWIP_TCP_BACKLOG_STATE_FREE";
    case HFOC_LWIP_TCP_BACKLOG_STATE_PENDING:
        return "HFOC_LWIP_TCP_BACKLOG_STATE_PENDING";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_backlog_status_string(
    enum hfoc_lwip_tcp_backlog_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_BACKLOG_OK:
        return "HFOC_LWIP_TCP_BACKLOG_OK";
    case HFOC_LWIP_TCP_BACKLOG_NULL:
        return "HFOC_LWIP_TCP_BACKLOG_NULL";
    case HFOC_LWIP_TCP_BACKLOG_BAD_STATE:
        return "HFOC_LWIP_TCP_BACKLOG_BAD_STATE";
    case HFOC_LWIP_TCP_BACKLOG_FULL:
        return "HFOC_LWIP_TCP_BACKLOG_FULL";
    case HFOC_LWIP_TCP_BACKLOG_EMPTY:
        return "HFOC_LWIP_TCP_BACKLOG_EMPTY";
    case HFOC_LWIP_TCP_BACKLOG_INVALID_PCB:
        return "HFOC_LWIP_TCP_BACKLOG_INVALID_PCB";
    case HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB:
        return "HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB";
    case HFOC_LWIP_TCP_BACKLOG_GENERATION_EXHAUSTED:
        return "HFOC_LWIP_TCP_BACKLOG_GENERATION_EXHAUSTED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_backlog_unused;

#endif
