#include "hfoc_lwip_tcp_isolation.h"

#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_ISOLATION_HOST_TEST)

static bool hfoc_lwip_tcp_isolation_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_isolation_event_valid(
    enum hfoc_lwip_tcp_isolation_event event)
{
    return event >= HFOC_LWIP_TCP_ISOLATION_EVENT_NONE &&
           event <= HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT;
}

static bool hfoc_lwip_tcp_isolation_entry_valid(
    const struct hfoc_lwip_tcp_isolation_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U &&
               entry->pool == NULL &&
               entry->quarantine == NULL &&
               entry->connection == NULL &&
               entry->pcb == NULL &&
               entry->event_count == 0U &&
               entry->close_count == 0U &&
               entry->rejected_count == 0U &&
               entry->last_event ==
                   HFOC_LWIP_TCP_ISOLATION_EVENT_NONE &&
               entry->last_status ==
                   HFOC_LWIP_TCP_ISOLATION_OK &&
               entry->last_index == 0U &&
               entry->last_generation == 0U;
    }
    return hfoc_lwip_tcp_isolation_generation_valid(
               entry->generation) &&
           entry->pool != NULL &&
           entry->quarantine != NULL &&
           entry->connection != NULL &&
           entry->pcb != NULL;
}

static bool hfoc_lwip_tcp_isolation_valid(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    uint32_t active_count;
    uint32_t index;

    if (matrix == NULL ||
        !matrix->published ||
        matrix->version != HFOC_LWIP_TCP_ISOLATION_VERSION ||
        matrix->active_count >
            HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT) {
        return false;
    }

    active_count = 0U;
    for (index = 0U;
         index < HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT;
         ++index) {
        if (!hfoc_lwip_tcp_isolation_entry_valid(
                &matrix->entries[index])) {
            return false;
        }
        if (matrix->entries[index].active) {
            ++active_count;
        }
    }
    return active_count == matrix->active_count;
}

static bool hfoc_lwip_tcp_isolation_lookup(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_isolation_entry **entry_out)
{
    const struct hfoc_lwip_tcp_isolation_entry *entry;

    if (!hfoc_lwip_tcp_isolation_valid(matrix) ||
        token == NULL ||
        token->index >= HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT ||
        !hfoc_lwip_tcp_isolation_generation_valid(
            token->generation)) {
        return false;
    }
    entry = &matrix->entries[token->index];
    if (!entry->active ||
        entry->generation != token->generation) {
        return false;
    }
    *entry_out = entry;
    return true;
}

static void hfoc_lwip_tcp_isolation_record(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_isolation_entry *entry,
    enum hfoc_lwip_tcp_isolation_event event,
    enum hfoc_lwip_tcp_isolation_status status,
    uint32_t index,
    uint32_t generation)
{
    matrix->last_event = event;
    matrix->last_status = status;
    matrix->last_index = index;
    matrix->last_generation = generation;

    if (entry == NULL) {
        return;
    }
    entry->last_event = event;
    entry->last_status = status;
    entry->last_index = index;
    entry->last_generation = generation;
}

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_init(
    struct hfoc_lwip_tcp_isolation *matrix)
{
    if (matrix == NULL) {
        return HFOC_LWIP_TCP_ISOLATION_NULL;
    }
    if (matrix->published) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_STATE;
    }

    memset(matrix, 0, sizeof(*matrix));
    matrix->version = HFOC_LWIP_TCP_ISOLATION_VERSION;
    matrix->published = true;
    return HFOC_LWIP_TCP_ISOLATION_OK;
}

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_attach(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_isolation_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;
    uint32_t index;

    if (matrix == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_ISOLATION_NULL;
    }
    if (!hfoc_lwip_tcp_isolation_valid(matrix)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT ||
        !hfoc_lwip_tcp_isolation_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_pool_published(pool)) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_POOL;
    }
    if (pool->slots[token->index].state !=
            HFOC_LWIP_TCP_POOL_STATE_PUBLISHED ||
        pool->slots[token->index].generation != token->generation) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
    }
    if (!hfoc_lwip_tcp_quarantine_published(quarantine)) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE;
    }
    if (!quarantine->entries[token->index].active ||
        quarantine->entries[token->index].generation !=
            token->generation) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        !hfoc_lwip_tcp_isolation_generation_valid(
            connection->generation) ||
        connection->generation != token->generation) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
    }
    if (connection->pcb == NULL) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_PCB;
    }

    entry = &matrix->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN;
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT;
         ++index) {
        if (matrix->entries[index].active &&
            matrix->entries[index].connection == connection) {
            return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->pool = pool;
    entry->quarantine = quarantine;
    entry->connection = connection;
    entry->pcb = connection->pcb;
    entry->last_status = HFOC_LWIP_TCP_ISOLATION_OK;
    ++matrix->active_count;
    return HFOC_LWIP_TCP_ISOLATION_OK;
}

static enum hfoc_lwip_tcp_quarantine_kind
hfoc_lwip_tcp_isolation_quarantine_kind(
    enum hfoc_lwip_tcp_isolation_event event)
{
    switch (event) {
    case HFOC_LWIP_TCP_ISOLATION_EVENT_RESET:
    case HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT:
        return HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT;
    case HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION:
    case HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE:
        return HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE;
    case HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER:
        return HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE;
    default:
        return HFOC_LWIP_TCP_QUARANTINE_KIND_NONE;
    }
}

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_inject(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_isolation_event event)
{
    struct hfoc_lwip_tcp_isolation_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_quarantine_kind kind;
    enum hfoc_lwip_tcp_quarantine_status quarantine_status;
    enum hfoc_lwip_tcp_pool_status pool_status;

    if (matrix == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_ISOLATION_NULL;
    }
    if (!hfoc_lwip_tcp_isolation_valid(matrix)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT ||
        !hfoc_lwip_tcp_isolation_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_isolation_event_valid(event) ||
        event == HFOC_LWIP_TCP_ISOLATION_EVENT_NONE) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT;
    }

    entry = &matrix->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN;
    }
    if (pool != entry->pool) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_POOL;
    }
    if (quarantine != entry->quarantine) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        connection->generation != token->generation ||
        connection != entry->connection) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION,
            token->index, token->generation);
        ++matrix->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
    }
    if (connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
        connection->state == HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED,
            token->index, token->generation);
        return HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION,
            token->index, token->generation);
        ++matrix->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
    }

    pcb = connection->pcb;
    if (pcb == NULL || pcb != entry->pcb) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_INVALID_PCB,
            token->index, token->generation);
        ++matrix->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_ISOLATION_INVALID_PCB;
    }

    ++entry->event_count;
    ++entry->event_counts[(unsigned int)event];
    kind = hfoc_lwip_tcp_isolation_quarantine_kind(event);
    quarantine_status = hfoc_lwip_tcp_quarantine_check(
        entry->quarantine, token, connection,
        kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT ? NULL : pcb,
        connection->state, kind);
    if (quarantine_status != HFOC_LWIP_TCP_QUARANTINE_OK) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE,
            token->index, token->generation);
        ++matrix->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE;
    }

    pool_status = hfoc_lwip_tcp_pool_abort(pool, token);
    if (pool_status != HFOC_LWIP_TCP_POOL_OK) {
        hfoc_lwip_tcp_isolation_record(
            matrix, entry, event,
            HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED,
            token->index, token->generation);
        ++matrix->rejected_count;
        ++entry->rejected_count;
        return HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED;
    }

    ++entry->close_count;
    ++matrix->closed_count;
    hfoc_lwip_tcp_isolation_record(
        matrix, entry, event,
        HFOC_LWIP_TCP_ISOLATION_ABORTED,
        token->index, token->generation);
    return HFOC_LWIP_TCP_ISOLATION_ABORTED;
}

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_detach(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *quarantine,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_isolation_entry *entry;
    struct hfoc_lwip_tcp_connection *connection;

    if (matrix == NULL || pool == NULL || quarantine == NULL ||
        token == NULL) {
        return HFOC_LWIP_TCP_ISOLATION_NULL;
    }
    if (!hfoc_lwip_tcp_isolation_valid(matrix)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT ||
        !hfoc_lwip_tcp_isolation_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN;
    }
    entry = &matrix->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN;
    }
    if (pool != entry->pool) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_POOL;
    }
    if (quarantine != entry->quarantine) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE;
    }

    connection = &pool->slots[token->index].connection;
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        connection->generation != token->generation ||
        connection != entry->connection) {
        return HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        return HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION;
    }

    memset(entry, 0, sizeof(*entry));
    entry->last_status = HFOC_LWIP_TCP_ISOLATION_OK;
    --matrix->active_count;
    return HFOC_LWIP_TCP_ISOLATION_OK;
}

bool hfoc_lwip_tcp_isolation_published(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix);
}

uint32_t hfoc_lwip_tcp_isolation_active_count(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->active_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_closed_count(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->closed_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_rejected_count(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_entry_event_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_isolation_entry *entry;

    return hfoc_lwip_tcp_isolation_lookup(matrix, token, &entry)
        ? entry->event_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_entry_close_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_isolation_entry *entry;

    return hfoc_lwip_tcp_isolation_lookup(matrix, token, &entry)
        ? entry->close_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_entry_rejected_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_isolation_entry *entry;

    return hfoc_lwip_tcp_isolation_lookup(matrix, token, &entry)
        ? entry->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_event_count(
    const struct hfoc_lwip_tcp_isolation *matrix,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_isolation_event event)
{
    const struct hfoc_lwip_tcp_isolation_entry *entry;

    if (!hfoc_lwip_tcp_isolation_event_valid(event)) {
        return 0U;
    }
    return hfoc_lwip_tcp_isolation_lookup(matrix, token, &entry)
        ? entry->event_counts[(unsigned int)event]
        : 0U;
}

enum hfoc_lwip_tcp_isolation_event
hfoc_lwip_tcp_isolation_last_event(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->last_event
        : HFOC_LWIP_TCP_ISOLATION_EVENT_NONE;
}

enum hfoc_lwip_tcp_isolation_status
hfoc_lwip_tcp_isolation_last_status(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->last_status
        : HFOC_LWIP_TCP_ISOLATION_OK;
}

uint32_t hfoc_lwip_tcp_isolation_last_index(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->last_index
        : 0U;
}

uint32_t hfoc_lwip_tcp_isolation_last_generation(
    const struct hfoc_lwip_tcp_isolation *matrix)
{
    return hfoc_lwip_tcp_isolation_valid(matrix)
        ? matrix->last_generation
        : 0U;
}

const char *hfoc_lwip_tcp_isolation_event_string(
    enum hfoc_lwip_tcp_isolation_event event)
{
    switch (event) {
    case HFOC_LWIP_TCP_ISOLATION_EVENT_NONE:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_NONE";
    case HFOC_LWIP_TCP_ISOLATION_EVENT_RESET:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_RESET";
    case HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION";
    case HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER";
    case HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE";
    case HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT:
        return "HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_isolation_status_string(
    enum hfoc_lwip_tcp_isolation_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_ISOLATION_OK:
        return "HFOC_LWIP_TCP_ISOLATION_OK";
    case HFOC_LWIP_TCP_ISOLATION_NULL:
        return "HFOC_LWIP_TCP_ISOLATION_NULL";
    case HFOC_LWIP_TCP_ISOLATION_BAD_STATE:
        return "HFOC_LWIP_TCP_ISOLATION_BAD_STATE";
    case HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN:
        return "HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN";
    case HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED";
    case HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN:
        return "HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN";
    case HFOC_LWIP_TCP_ISOLATION_INVALID_POOL:
        return "HFOC_LWIP_TCP_ISOLATION_INVALID_POOL";
    case HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE:
        return "HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE";
    case HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION:
        return "HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION";
    case HFOC_LWIP_TCP_ISOLATION_INVALID_PCB:
        return "HFOC_LWIP_TCP_ISOLATION_INVALID_PCB";
    case HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT:
        return "HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT";
    case HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION:
        return "HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION";
    case HFOC_LWIP_TCP_ISOLATION_ABORTED:
        return "HFOC_LWIP_TCP_ISOLATION_ABORTED";
    case HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED:
        return "HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED";
    case HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED:
        return "HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED";
    case HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED";
    default:
        return NULL;
    }
}

#endif
