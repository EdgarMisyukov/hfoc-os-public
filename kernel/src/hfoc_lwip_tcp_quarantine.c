#include "hfoc_lwip_tcp_quarantine.h"

#include "hfoc_lwip_tcp_conn.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_QUARANTINE_HOST_TEST) || \
    defined(HFOC_NETWORK_RESOURCE_QEMU_TEST)

static bool hfoc_lwip_tcp_quarantine_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_quarantine_kind_valid(
    enum hfoc_lwip_tcp_quarantine_kind kind)
{
    return kind >= HFOC_LWIP_TCP_QUARANTINE_KIND_NONE &&
           kind <= HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT;
}

static bool hfoc_lwip_tcp_quarantine_state_valid(
    enum hfoc_lwip_tcp_conn_state state)
{
    return state >= HFOC_LWIP_TCP_CONN_STATE_INACTIVE &&
           state <= HFOC_LWIP_TCP_CONN_STATE_ABORTED;
}

static bool hfoc_lwip_tcp_quarantine_kind_allowed(
    enum hfoc_lwip_tcp_conn_state state,
    enum hfoc_lwip_tcp_quarantine_kind kind)
{
    if (kind == HFOC_LWIP_TCP_QUARANTINE_KIND_NONE) {
        return false;
    }
    if (state == HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED) {
        return true;
    }
    if (state == HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        return kind == HFOC_LWIP_TCP_QUARANTINE_KIND_SENT ||
               kind == HFOC_LWIP_TCP_QUARANTINE_KIND_POLL ||
               kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR ||
               kind == HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE ||
               kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT;
    }
    return false;
}

static bool hfoc_lwip_tcp_quarantine_entry_valid(
    const struct hfoc_lwip_tcp_quarantine_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U &&
               entry->connection == NULL &&
               entry->pcb == NULL &&
               entry->accepted_count == 0U &&
               entry->quarantined_count == 0U &&
               entry->stale_token_count == 0U &&
               entry->invalid_connection_count == 0U &&
               entry->invalid_pcb_count == 0U &&
               entry->invalid_state_count == 0U &&
               entry->invalid_kind_count == 0U &&
               entry->last_reason ==
                   HFOC_LWIP_TCP_QUARANTINE_REASON_NONE &&
               entry->last_kind ==
                   HFOC_LWIP_TCP_QUARANTINE_KIND_NONE &&
               entry->last_expected_state ==
                   HFOC_LWIP_TCP_CONN_STATE_INACTIVE &&
               entry->last_index == 0U &&
               entry->last_generation == 0U;
    }
    return hfoc_lwip_tcp_quarantine_generation_valid(
               entry->generation) &&
           entry->connection != NULL &&
           entry->pcb != NULL;
}

static bool hfoc_lwip_tcp_quarantine_valid(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    uint32_t active_count;
    uint32_t index;

    if (gate == NULL ||
        !gate->published ||
        gate->version != HFOC_LWIP_TCP_QUARANTINE_VERSION ||
        gate->active_count >
            HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT) {
        return false;
    }

    active_count = 0U;
    for (index = 0U;
         index < HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT;
         ++index) {
        if (!hfoc_lwip_tcp_quarantine_entry_valid(
                &gate->entries[index])) {
            return false;
        }
        if (gate->entries[index].active) {
            ++active_count;
        }
    }
    return active_count == gate->active_count;
}

static bool hfoc_lwip_tcp_quarantine_lookup(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_quarantine_entry **entry_out)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    if (!hfoc_lwip_tcp_quarantine_valid(gate) ||
        token == NULL ||
        token->index >= HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            token->generation)) {
        return false;
    }
    entry = &gate->entries[token->index];
    if (!entry->active ||
        entry->generation != token->generation) {
        return false;
    }
    *entry_out = entry;
    return true;
}

static void hfoc_lwip_tcp_quarantine_record_evidence(
    struct hfoc_lwip_tcp_quarantine *gate,
    struct hfoc_lwip_tcp_quarantine_entry *entry,
    enum hfoc_lwip_tcp_quarantine_reason reason,
    enum hfoc_lwip_tcp_quarantine_kind kind,
    enum hfoc_lwip_tcp_conn_state expected_state,
    uint32_t index,
    uint32_t generation)
{
    ++gate->quarantined_count;
    gate->last_reason = reason;
    gate->last_kind = kind;
    gate->last_expected_state = expected_state;
    gate->last_index = index;
    gate->last_generation = generation;

    if (entry == NULL) {
        return;
    }
    ++entry->quarantined_count;
    entry->last_reason = reason;
    entry->last_kind = kind;
    entry->last_expected_state = expected_state;
    entry->last_index = index;
    entry->last_generation = generation;
    switch (reason) {
    case HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN:
        ++entry->stale_token_count;
        break;
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION:
        ++entry->invalid_connection_count;
        break;
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB:
        ++entry->invalid_pcb_count;
        break;
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE:
        ++entry->invalid_state_count;
        break;
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND:
        ++entry->invalid_kind_count;
        break;
    default:
        break;
    }
}

enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_init(
    struct hfoc_lwip_tcp_quarantine *gate)
{
    if (gate == NULL) {
        return HFOC_LWIP_TCP_QUARANTINE_NULL;
    }
    if (gate->published) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_STATE;
    }

    memset(gate, 0, sizeof(*gate));
    gate->version = HFOC_LWIP_TCP_QUARANTINE_VERSION;
    gate->last_expected_state =
        HFOC_LWIP_TCP_CONN_STATE_INACTIVE;
    gate->published = true;
    return HFOC_LWIP_TCP_QUARANTINE_OK;
}

enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_attach(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb)
{
    struct hfoc_lwip_tcp_quarantine_entry *entry;
    uint32_t index;

    if (gate == NULL || token == NULL || connection == NULL ||
        pcb == NULL) {
        return HFOC_LWIP_TCP_QUARANTINE_NULL;
    }
    if (!hfoc_lwip_tcp_quarantine_valid(gate)) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN;
    }
    if (!connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            connection->generation) ||
        connection->generation != token->generation) {
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION;
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
        connection->state != HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE;
    }
    if (connection->pcb != pcb) {
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB;
    }

    entry = &gate->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN;
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT;
         ++index) {
        if (!gate->entries[index].active) {
            continue;
        }
        if (gate->entries[index].pcb == pcb) {
            return HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB;
        }
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT;
         ++index) {
        if (gate->entries[index].active &&
            gate->entries[index].connection == connection) {
            return HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->connection = connection;
    entry->pcb = pcb;
    entry->last_expected_state =
        HFOC_LWIP_TCP_CONN_STATE_INACTIVE;
    ++gate->active_count;
    return HFOC_LWIP_TCP_QUARANTINE_OK;
}

enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_check(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    enum hfoc_lwip_tcp_conn_state expected_state,
    enum hfoc_lwip_tcp_quarantine_kind kind)
{
    struct hfoc_lwip_tcp_quarantine_entry *entry;

    if (gate == NULL || token == NULL || connection == NULL) {
        return HFOC_LWIP_TCP_QUARANTINE_NULL;
    }
    if (!hfoc_lwip_tcp_quarantine_valid(gate)) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            token->generation)) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, NULL,
            HFOC_LWIP_TCP_QUARANTINE_REASON_BAD_TOKEN,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN;
    }

    entry = &gate->entries[token->index];
    if (!entry->active) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, NULL,
            HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, NULL,
            HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN;
    }
    if (connection != entry->connection ||
        !connection->published ||
        connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            connection->generation) ||
        connection->generation != token->generation) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, entry,
            HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION;
    }
    if (!hfoc_lwip_tcp_quarantine_state_valid(expected_state) ||
        connection->state != expected_state) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, entry,
            HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE;
    }

    if (kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR ||
        kind == HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT) {
        if (pcb != NULL) {
            hfoc_lwip_tcp_quarantine_record_evidence(
                gate, entry,
                HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB,
                kind, expected_state, token->index,
                token->generation);
            return HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB;
        }
    } else {
        if (pcb == NULL ||
            pcb != entry->pcb ||
            connection->pcb != pcb) {
            hfoc_lwip_tcp_quarantine_record_evidence(
                gate, entry,
                HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB,
                kind, expected_state, token->index,
                token->generation);
            return HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB;
        }
    }
    if (!hfoc_lwip_tcp_quarantine_kind_valid(kind) ||
        !hfoc_lwip_tcp_quarantine_kind_allowed(
            expected_state, kind)) {
        hfoc_lwip_tcp_quarantine_record_evidence(
            gate, entry,
            HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND,
            kind, expected_state, token->index,
            token->generation);
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND;
    }

    ++entry->accepted_count;
    ++entry->kind_counts[(unsigned int)kind];
    return HFOC_LWIP_TCP_QUARANTINE_OK;
}

enum hfoc_lwip_tcp_quarantine_status
hfoc_lwip_tcp_quarantine_detach(
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_quarantine_entry *entry;

    if (gate == NULL || token == NULL) {
        return HFOC_LWIP_TCP_QUARANTINE_NULL;
    }
    if (!hfoc_lwip_tcp_quarantine_valid(gate)) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN;
    }
    entry = &gate->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN;
    }
    if (entry->connection == NULL ||
        !entry->connection->published ||
        entry->connection->version != HFOC_LWIP_TCP_CONN_VERSION ||
        !hfoc_lwip_tcp_quarantine_generation_valid(
            entry->connection->generation) ||
        entry->connection->generation != token->generation) {
        return HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION;
    }
    if (entry->connection->state !=
            HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
        entry->connection->state !=
            HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        return HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION;
    }

    memset(entry, 0, sizeof(*entry));
    entry->last_expected_state =
        HFOC_LWIP_TCP_CONN_STATE_INACTIVE;
    --gate->active_count;
    return HFOC_LWIP_TCP_QUARANTINE_OK;
}

bool hfoc_lwip_tcp_quarantine_published(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate);
}

uint32_t hfoc_lwip_tcp_quarantine_active_count(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->active_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_quarantined_count(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->quarantined_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_accepted_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->accepted_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_entry_quarantined_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->quarantined_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_stale_token_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->stale_token_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_invalid_connection_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->invalid_connection_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_invalid_pcb_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->invalid_pcb_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_invalid_state_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->invalid_state_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_invalid_kind_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    return hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)
        ? entry->invalid_kind_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_kind_count(
    const struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    enum hfoc_lwip_tcp_quarantine_kind kind)
{
    const struct hfoc_lwip_tcp_quarantine_entry *entry;

    if (!hfoc_lwip_tcp_quarantine_kind_valid(kind) ||
        !hfoc_lwip_tcp_quarantine_lookup(gate, token, &entry)) {
        return 0U;
    }
    return entry->kind_counts[(unsigned int)kind];
}

enum hfoc_lwip_tcp_quarantine_reason
hfoc_lwip_tcp_quarantine_last_reason(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->last_reason
        : HFOC_LWIP_TCP_QUARANTINE_REASON_NONE;
}

enum hfoc_lwip_tcp_quarantine_kind
hfoc_lwip_tcp_quarantine_last_kind(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->last_kind
        : HFOC_LWIP_TCP_QUARANTINE_KIND_NONE;
}

enum hfoc_lwip_tcp_conn_state
hfoc_lwip_tcp_quarantine_last_expected_state(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->last_expected_state
        : HFOC_LWIP_TCP_CONN_STATE_INACTIVE;
}

uint32_t hfoc_lwip_tcp_quarantine_last_index(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->last_index
        : 0U;
}

uint32_t hfoc_lwip_tcp_quarantine_last_generation(
    const struct hfoc_lwip_tcp_quarantine *gate)
{
    return hfoc_lwip_tcp_quarantine_valid(gate)
        ? gate->last_generation
        : 0U;
}

const char *hfoc_lwip_tcp_quarantine_kind_string(
    enum hfoc_lwip_tcp_quarantine_kind kind)
{
    switch (kind) {
    case HFOC_LWIP_TCP_QUARANTINE_KIND_NONE:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_NONE";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_ACCEPT:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_ACCEPT";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_SENT:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_SENT";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_POLL:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_POLL";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE";
    case HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT:
        return "HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_quarantine_reason_string(
    enum hfoc_lwip_tcp_quarantine_reason reason)
{
    switch (reason) {
    case HFOC_LWIP_TCP_QUARANTINE_REASON_NONE:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_NONE";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_BAD_TOKEN:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_BAD_TOKEN";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE";
    case HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND:
        return "HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_quarantine_status_string(
    enum hfoc_lwip_tcp_quarantine_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_QUARANTINE_OK:
        return "HFOC_LWIP_TCP_QUARANTINE_OK";
    case HFOC_LWIP_TCP_QUARANTINE_NULL:
        return "HFOC_LWIP_TCP_QUARANTINE_NULL";
    case HFOC_LWIP_TCP_QUARANTINE_BAD_STATE:
        return "HFOC_LWIP_TCP_QUARANTINE_BAD_STATE";
    case HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN:
        return "HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN";
    case HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED";
    case HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN:
        return "HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN";
    case HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED";
    case HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION:
        return "HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION";
    case HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB:
        return "HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB";
    case HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE:
        return "HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE";
    case HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND:
        return "HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND";
    case HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION:
        return "HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION";
    case HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB:
        return "HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB";
    case HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION:
        return "HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION";
    default:
        return NULL;
    }
}

#endif
