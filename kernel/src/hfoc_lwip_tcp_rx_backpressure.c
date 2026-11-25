#include "hfoc_lwip_tcp_rx_backpressure.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_RX_BACKPRESSURE_HOST_TEST)

static bool hfoc_lwip_tcp_rx_backpressure_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_rx_backpressure_policy_valid(
    const struct hfoc_lwip_tcp_rx_backpressure_policy *policy)
{
    return policy != NULL &&
           policy->version ==
               HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION &&
           (policy->mode ==
                HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE ||
            policy->mode ==
                HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT) &&
           policy->packet_capacity >=
               HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MIN &&
           policy->packet_capacity <=
               HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX &&
           policy->application_capacity >=
               HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_MIN &&
           policy->application_capacity <=
               HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_MAX;
}

static bool hfoc_lwip_tcp_rx_backpressure_queue_valid(
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry)
{
    uint32_t capacity;
    uint32_t queued;

    if (!entry->active) {
        return true;
    }
    capacity = entry->policy.packet_capacity;
    if (entry->packet_head >= capacity ||
        entry->packet_tail >= capacity ||
        entry->outstanding_packets > capacity) {
        return false;
    }
    if (entry->outstanding_packets == 0U) {
        return entry->packet_head == entry->packet_tail &&
               entry->outstanding_bytes == 0U;
    }

    queued = (entry->packet_tail + capacity - entry->packet_head) %
        capacity;
    if (queued == 0U &&
        entry->outstanding_packets == capacity) {
        queued = capacity;
    }
    return queued == entry->outstanding_packets &&
           entry->outstanding_bytes > 0U;
}

static bool hfoc_lwip_tcp_rx_backpressure_entry_valid(
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return !entry->paused &&
               entry->generation == 0U &&
               entry->pcb == NULL &&
               entry->packet_head == 0U &&
               entry->packet_tail == 0U &&
               entry->outstanding_packets == 0U &&
               entry->outstanding_bytes == 0U &&
               entry->accepted_count == 0U &&
               entry->paused_count == 0U &&
               entry->rejected_count == 0U &&
               entry->released_count == 0U &&
               entry->policy.version == 0U;
    }
    if (!hfoc_lwip_tcp_rx_backpressure_generation_valid(
            entry->generation) ||
        entry->pcb == NULL ||
        !hfoc_lwip_tcp_rx_backpressure_policy_valid(
            &entry->policy) ||
        entry->outstanding_bytes >
            entry->policy.application_capacity ||
        entry->released_count > entry->accepted_count ||
        !hfoc_lwip_tcp_rx_backpressure_queue_valid(entry)) {
        return false;
    }

    if (entry->policy.mode ==
        HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT) {
        return !entry->paused;
    }
    return !(entry->paused &&
             entry->outstanding_packets == 0U &&
             entry->outstanding_bytes == 0U);
}

static bool hfoc_lwip_tcp_rx_backpressure_valid(
    const struct hfoc_lwip_tcp_rx_backpressure *gate)
{
    uint32_t active_count;
    uint32_t index;

    if (gate == NULL ||
        !gate->published ||
        gate->version != HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION ||
        gate->active_count >
            HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT) {
        return false;
    }

    active_count = 0U;
    for (index = 0U;
         index < HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_rx_backpressure_entry *entry =
            &gate->entries[index];

        if (!hfoc_lwip_tcp_rx_backpressure_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == gate->active_count;
}

static enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_validate_token(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_rx_backpressure_entry **entry_out)
{
    struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    if (gate == NULL || token == NULL || entry_out == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    if (!hfoc_lwip_tcp_rx_backpressure_valid(gate)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE;
    }
    if (token->index >=
            HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_rx_backpressure_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN;
    }

    entry = &gate->entries[token->index];
    if (!entry->active) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN;
    }

    *entry_out = entry;
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

static bool hfoc_lwip_tcp_rx_backpressure_lookup(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_rx_backpressure_entry **entry_out)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    if (!hfoc_lwip_tcp_rx_backpressure_valid(gate) ||
        token == NULL ||
        token->index >=
            HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_rx_backpressure_generation_valid(
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

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_init(
    struct hfoc_lwip_tcp_rx_backpressure *gate)
{
    if (gate == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    if (gate->published) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE;
    }

    memset(gate, 0, sizeof(*gate));
    gate->version = HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION;
    gate->published = true;
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_attach(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    const struct hfoc_lwip_tcp_rx_backpressure_policy *policy)
{
    struct hfoc_lwip_tcp_rx_backpressure_entry *entry;
    uint32_t index;

    if (gate == NULL || token == NULL || pcb == NULL ||
        policy == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    if (!hfoc_lwip_tcp_rx_backpressure_valid(gate)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE;
    }
    if (token->index >=
            HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT ||
        !hfoc_lwip_tcp_rx_backpressure_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_rx_backpressure_policy_valid(policy)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY;
    }

    entry = &gate->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN;
    }
    for (index = 0U;
         index < HFOC_LWIP_TCP_RX_BACKPRESSURE_SLOT_COUNT;
         ++index) {
        if (gate->entries[index].active &&
            gate->entries[index].pcb == pcb) {
            return HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB;
        }
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->pcb = pcb;
    entry->policy = *policy;
    ++gate->active_count;
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_receive(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length,
    enum hfoc_lwip_tcp_rx_backpressure_decision *decision)
{
    struct hfoc_lwip_tcp_rx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_rx_backpressure_status token_status;

    if (gate == NULL || token == NULL || pcb == NULL ||
        decision == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_rx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_RX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->pcb != pcb) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB;
    }
    if (length == 0U ||
        length > entry->policy.application_capacity) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH;
    }

    if (entry->paused ||
        entry->outstanding_packets >=
            entry->policy.packet_capacity ||
        length >
            entry->policy.application_capacity -
                entry->outstanding_bytes) {
        if (entry->policy.mode ==
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE) {
            entry->paused = true;
            ++entry->paused_count;
            *decision =
                HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE;
            return HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED;
        }
        ++entry->rejected_count;
        *decision =
            HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT;
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED;
    }

    entry->accepted_packets[entry->packet_tail] = length;
    entry->packet_tail =
        (entry->packet_tail + 1U) %
        entry->policy.packet_capacity;
    ++entry->outstanding_packets;
    entry->outstanding_bytes += length;
    ++entry->accepted_count;
    if (entry->policy.mode ==
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE &&
        (entry->outstanding_packets >=
             entry->policy.packet_capacity ||
         entry->outstanding_bytes >=
             entry->policy.application_capacity)) {
        entry->paused = true;
    }
    *decision =
        HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT;
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_release(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb *pcb,
    uint32_t length)
{
    struct hfoc_lwip_tcp_rx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_rx_backpressure_status token_status;

    if (gate == NULL || token == NULL || pcb == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_rx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_RX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->pcb != pcb) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB;
    }
    if (length == 0U || entry->outstanding_packets == 0U ||
        entry->accepted_packets[entry->packet_head] != length) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE;
    }

    entry->packet_head =
        (entry->packet_head + 1U) %
        entry->policy.packet_capacity;
    --entry->outstanding_packets;
    entry->outstanding_bytes -= length;
    ++entry->released_count;
    if (entry->outstanding_packets <
            entry->policy.packet_capacity &&
        entry->outstanding_bytes <
            entry->policy.application_capacity) {
        entry->paused = false;
    }

    tcp_recved(pcb, (u16_t)length);
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

enum hfoc_lwip_tcp_rx_backpressure_status
hfoc_lwip_tcp_rx_backpressure_detach(
    struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_rx_backpressure_entry *entry;
    enum hfoc_lwip_tcp_rx_backpressure_status token_status;

    if (gate == NULL || token == NULL) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL;
    }
    token_status = hfoc_lwip_tcp_rx_backpressure_validate_token(
        gate, token, &entry);
    if (token_status != HFOC_LWIP_TCP_RX_BACKPRESSURE_OK) {
        return token_status;
    }
    if (entry->outstanding_packets != 0U ||
        entry->outstanding_bytes != 0U) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING;
    }

    memset(entry, 0, sizeof(*entry));
    --gate->active_count;
    return HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
}

bool hfoc_lwip_tcp_rx_backpressure_published(
    const struct hfoc_lwip_tcp_rx_backpressure *gate)
{
    return hfoc_lwip_tcp_rx_backpressure_valid(gate);
}

uint32_t hfoc_lwip_tcp_rx_backpressure_active_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate)
{
    return hfoc_lwip_tcp_rx_backpressure_valid(gate)
        ? gate->active_count
        : 0U;
}

enum hfoc_lwip_tcp_rx_backpressure_state
hfoc_lwip_tcp_rx_backpressure_state(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    if (!hfoc_lwip_tcp_rx_backpressure_lookup(
            gate, token, &entry)) {
        return HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE;
    }
    return entry->paused
        ? HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED
        : HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE;
}

bool hfoc_lwip_tcp_rx_backpressure_paused(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry) &&
           entry->paused;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->outstanding_packets
        : 0U;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->outstanding_bytes
        : 0U;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_accepted_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->accepted_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_paused_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->paused_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_rejected_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->rejected_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_rx_backpressure_released_count(
    const struct hfoc_lwip_tcp_rx_backpressure *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_rx_backpressure_entry *entry;

    return hfoc_lwip_tcp_rx_backpressure_lookup(
               gate, token, &entry)
        ? entry->released_count
        : 0U;
}

const char *hfoc_lwip_tcp_rx_backpressure_mode_string(
    enum hfoc_lwip_tcp_rx_backpressure_mode mode)
{
    switch (mode) {
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_rx_backpressure_state_string(
    enum hfoc_lwip_tcp_rx_backpressure_state state)
{
    switch (state) {
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_rx_backpressure_decision_string(
    enum hfoc_lwip_tcp_rx_backpressure_decision decision)
{
    switch (decision) {
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_rx_backpressure_status_string(
    enum hfoc_lwip_tcp_rx_backpressure_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_OK:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_OK";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_NOT_ATTACHED";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE";
    case HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING:
        return "HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_rx_backpressure_not_compiled;

#endif
