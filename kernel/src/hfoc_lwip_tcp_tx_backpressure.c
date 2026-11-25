#include "hfoc_lwip_tcp_tx_backpressure.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_TX_BACKPRESSURE_HOST_TEST)

static bool hfoc_lwip_tcp_tx_backpressure_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_tx_backpressure_policy_valid(
    const struct hfoc_lwip_tcp_tx_backpressure_policy *policy)
{
    return policy != NULL &&
           policy->version ==
               HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION &&
           policy->response_capacity >=
               HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MIN &&
           policy->response_capacity <=
               HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX &&
           policy->chunk_capacity >=
               HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_MIN &&
           policy->chunk_capacity <=
               HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_MAX;
}

static bool hfoc_lwip_tcp_tx_backpressure_entry_valid(
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return !entry->blocked &&
               !entry->response_pending &&
               entry->generation == 0U &&
               entry->pcb == NULL &&
               entry->response_length == 0U &&
               entry->acked_offset == 0U &&
               entry->published_offset == 0U &&
               entry->submit_count == 0U &&
               entry->publish_count == 0U &&
               entry->sent_count == 0U &&
               entry->retained_count == 0U &&
               entry->rejected_count == 0U &&
               entry->complete_count == 0U &&
               entry->policy.version == 0U;
    }
    if (!hfoc_lwip_tcp_tx_backpressure_generation_valid(
            entry->generation) ||
        entry->pcb == NULL ||
        !hfoc_lwip_tcp_tx_backpressure_policy_valid(
            &entry->policy) ||
        entry->acked_offset > entry->published_offset ||
        entry->published_offset > entry->response_length ||
        entry->response_length >
            entry->policy.response_capacity) {
        return false;
    }
    if (!entry->response_pending) {
        return !entry->blocked &&
               entry->response_length == 0U &&
               entry->acked_offset == 0U &&
               entry->published_offset == 0U;
    }
    return entry->response_length > 0U;
}

static bool hfoc_lwip_tcp_tx_backpressure_valid(
    const struct hfoc_lwip_tcp_tx_backpressure *gate)
{
    uint32_t active_count;
    uint32_t index;

    if (gate == NULL ||
        !gate->published ||
        gate->version != HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION ||
        gate->active_count >
            HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT) {
        return false;
    }

    active_count = 0U;
    for (index = 0U;
         index < HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_tx_backpressure_entry *entry =
            &gate->entries[index];

        if (!hfoc_lwip_tcp_tx_backpressure_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == gate->active_count;
}

static enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_validate_token(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_tx_backpressure_entry **entry_out)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    if (gate == NULL || token == NULL || entry_out == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    if (!hfoc_lwip_tcp_tx_backpressure_valid(gate)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE;
    }
    if (token->index >=
            HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_tx_backpressure_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN;
    }

    entry = &gate->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN;
    }

    *entry_out = entry;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

static bool hfoc_lwip_tcp_tx_backpressure_lookup(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_tx_backpressure_entry **entry_out)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    if (!hfoc_lwip_tcp_tx_backpressure_valid(gate) ||
        token == NULL ||
        token->index >=
            HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_tx_backpressure_generation_valid(
            token->generation)) {
        return false;
    }
    entry = &gate->entries[token->index];
    if (!entry->active || entry->generation != token->generation) {
        return false;
    }
    *entry_out = entry;
    return true;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_init(
    struct hfoc_lwip_tcp_tx_backpressure *gate)
{
    if (gate == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    if (gate->published) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE;
    }

    memset(gate, 0, sizeof(*gate));
    gate->version = HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION;
    gate->published = true;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_attach(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_tx_backpressure_policy *policy)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;
    uint32_t index;

    if (gate == NULL || token == NULL || pcb == NULL ||
        policy == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    if (!hfoc_lwip_tcp_tx_backpressure_valid(gate)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE;
    }
    if (token->index >=
            HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_tx_backpressure_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_tx_backpressure_policy_valid(policy)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY;
    }

    entry = &gate->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN;
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_TX_BACKPRESSURE_SLOT_COUNT;
         ++index) {
        if (gate->entries[index].active &&
            gate->entries[index].pcb == pcb) {
            return HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->pcb = pcb;
    entry->policy = *policy;
    ++gate->active_count;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_submit(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const uint8_t *response,
    uint32_t length,
    enum hfoc_lwip_tcp_tx_backpressure_decision *decision)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_tx_backpressure_status token_status;

    if (gate == NULL || token == NULL || pcb == NULL ||
        response == NULL || decision == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_tx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_TX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->pcb != pcb) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB;
    }
    if (length == 0U) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH;
    }
    if (length > entry->policy.response_capacity) {
        ++entry->rejected_count;
        *decision =
            HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_REJECTED;
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED;
    }
    if (entry->response_pending) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING;
    }

    memcpy(entry->response, response, length);
    entry->response_length = length;
    entry->acked_offset = 0U;
    entry->published_offset = 0U;
    entry->response_pending = true;
    entry->blocked = false;
    ++entry->submit_count;
    *decision =
        HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_publish(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t *published_length,
    enum hfoc_lwip_tcp_tx_backpressure_decision *decision)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_tx_backpressure_status token_status;
    uint32_t remaining;
    uint32_t available;
    uint32_t chunk;
    err_t write_err;

    if (gate == NULL || token == NULL || pcb == NULL ||
        published_length == NULL || decision == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    *published_length = 0U;
    token_status = hfoc_lwip_tcp_tx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_TX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->pcb != pcb) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB;
    }
    if (!entry->response_pending) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE;
    }

    remaining =
        entry->response_length - entry->published_offset;
    if (remaining == 0U) {
        entry->blocked = true;
        ++entry->retained_count;
        *decision =
            HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED;
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY;
    }

    available = (uint32_t)tcp_sndbuf(pcb);
    chunk = remaining;
    if (chunk > available) {
        chunk = available;
    }
    if (chunk > entry->policy.chunk_capacity) {
        chunk = entry->policy.chunk_capacity;
    }
    if (chunk == 0U) {
        entry->blocked = true;
        ++entry->retained_count;
        *decision =
            HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED;
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY;
    }

    write_err = tcp_write(
        pcb,
        entry->response + entry->published_offset,
        (u16_t)chunk,
        TCP_WRITE_FLAG_COPY);
    if (write_err != ERR_OK) {
        entry->blocked = true;
        ++entry->retained_count;
        *decision =
            HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED;
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY;
    }

    entry->published_offset += chunk;
    entry->blocked = false;
    ++entry->publish_count;
    *published_length = chunk;
    *decision =
        HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_sent(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_tx_backpressure_status token_status;

    if (gate == NULL || token == NULL || pcb == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_tx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_TX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->pcb != pcb) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB;
    }
    if (!entry->response_pending) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE;
    }
    if (length == 0U ||
        length >
            entry->published_offset - entry->acked_offset) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK;
    }

    entry->acked_offset += length;
    ++entry->sent_count;
    if (entry->acked_offset == entry->published_offset &&
        entry->published_offset == entry->response_length) {
        entry->response_pending = false;
        entry->blocked = false;
        entry->response_length = 0U;
        entry->acked_offset = 0U;
        entry->published_offset = 0U;
        memset(entry->response, 0, sizeof(entry->response));
        ++entry->complete_count;
    }
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_tx_backpressure_status
hfoc_lwip_tcp_tx_backpressure_detach(
    struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_tx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_tx_backpressure_status token_status;

    if (gate == NULL || token == NULL) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_tx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_TX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->response_pending ||
        entry->published_offset != entry->acked_offset) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING;
    }

    memset(entry, 0, sizeof(*entry));
    --gate->active_count;
    return HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

bool hfoc_lwip_tcp_tx_backpressure_published(
    const struct hfoc_lwip_tcp_tx_backpressure *gate)
{
    return hfoc_lwip_tcp_tx_backpressure_valid(gate);
}

uint32_t hfoc_lwip_tcp_tx_backpressure_active_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate)
{
    return hfoc_lwip_tcp_tx_backpressure_valid(gate)
        ? gate->active_count
        : 0U;
}

enum hfoc_lwip_tcp_tx_backpressure_state
hfoc_lwip_tcp_tx_backpressure_state(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    if (!hfoc_lwip_tcp_tx_backpressure_lookup(
            gate, token, &entry)) {
        return HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE;
    }
    return entry->blocked
        ? HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_BLOCKED
        : HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE;
}

bool hfoc_lwip_tcp_tx_backpressure_response_pending(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry) &&
           entry->response_pending;
}

bool hfoc_lwip_tcp_tx_backpressure_blocked(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry) &&
           entry->blocked;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_response_length(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->response_length
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_acked_offset(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->acked_offset
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_published_offset(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->published_offset
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_outstanding_bytes(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->published_offset - entry->acked_offset
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_submit_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->submit_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_publish_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->publish_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_sent_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->sent_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_retained_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->retained_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_rejected_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_tx_backpressure_complete_count(
    const struct hfoc_lwip_tcp_tx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_tx_backpressure_entry *entry;

    return hfoc_lwip_tcp_tx_backpressure_lookup(
               gate, token, &entry)
        ? entry->complete_count
        : 0U;
}

const char *hfoc_lwip_tcp_tx_backpressure_state_string(
    enum hfoc_lwip_tcp_tx_backpressure_state state)
{
    switch (state) {
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_BLOCKED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_BLOCKED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_tx_backpressure_decision_string(
    enum hfoc_lwip_tcp_tx_backpressure_decision decision)
{
    switch (decision) {
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_REJECTED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_REJECTED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_COMPLETE:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_COMPLETE";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_tx_backpressure_status_string(
    enum hfoc_lwip_tcp_tx_backpressure_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_OK:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_OK";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_NOT_ATTACHED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED";
    case HFOC_LWIP_TCP_TX_BACKPRESSURE_EMPTY:
        return "HFOC_LWIP_TCP_TX_BACKPRESSURE_EMPTY";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_tx_backpressure_not_compiled;

#endif
