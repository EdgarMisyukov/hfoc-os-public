#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_tx_backpressure.h"

#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_TX_BACKPRESSURE_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

u32_t sys_now(void)
{
    return 1000U;
}

static void fill_pattern(uint8_t *buffer, uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        buffer[index] = (uint8_t)(0xA0U + (index * 7U));
    }
}

static struct tcp_pcb *make_pcb(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb != NULL) {
        pcb->state = ESTABLISHED;
        pcb->listener = NULL;
    }
    return pcb;
}

static void discard_pcb(struct tcp_pcb *pcb)
{
    if (pcb != NULL) {
        tcp_abort(pcb);
    }
}

static bool init_gate(
    struct hfoc_lwip_tcp_tx_backpressure *gate)
{
    memset(gate, 0, sizeof(*gate));
    return hfoc_lwip_tcp_tx_backpressure_init(gate) ==
        HFOC_LWIP_TCP_TX_BACKPRESSURE_OK;
}

static bool init_pool(struct hfoc_lwip_tcp_pool *pool)
{
    memset(pool, 0, sizeof(*pool));
    return hfoc_lwip_tcp_pool_init(pool) ==
        HFOC_LWIP_TCP_POOL_OK;
}

static bool reserve_token(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_pool_token *token)
{
    memset(token, 0, sizeof(*token));
    return hfoc_lwip_tcp_pool_reserve(pool, token) ==
        HFOC_LWIP_TCP_POOL_OK;
}

static struct hfoc_lwip_tcp_tx_backpressure_policy make_policy(
    uint32_t response_capacity,
    uint32_t chunk_capacity)
{
    struct hfoc_lwip_tcp_tx_backpressure_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION;
    policy.response_capacity = response_capacity;
    policy.chunk_capacity = chunk_capacity;
    return policy;
}

static void add_snd_buf(struct tcp_pcb *pcb, u16_t length)
{
    u32_t next = (u32_t)pcb->snd_buf + length;

    pcb->snd_buf = (tcpwnd_size_t)(
        next > TCP_SND_BUF ? TCP_SND_BUF : next);
}

static uint32_t unsent_copy(
    struct tcp_pcb *pcb,
    uint8_t *output,
    uint32_t output_capacity)
{
    struct tcp_seg *seg = pcb->unsent;
    uint32_t total = 0U;

    while (seg != NULL) {
        u16_t segment_length = seg->len;
        u16_t data_offset =
            (u16_t)(seg->p->tot_len - seg->len);

        if (segment_length > output_capacity - total ||
            pbuf_copy_partial(
                seg->p,
                output + total,
                segment_length,
                data_offset) != segment_length) {
            return output_capacity + 1U;
        }
        total += segment_length;
        seg = seg->next;
    }
    return total;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_tx_backpressure_state_string(
              HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_state_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_state_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_BLOCKED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_state_string(
                  (enum hfoc_lwip_tcp_tx_backpressure_state)99U) == NULL,
          "TX state strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_tx_backpressure_decision_string(
              HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_decision_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_decision_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_REJECTED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_decision_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_COMPLETE) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_decision_string(
                  (enum hfoc_lwip_tcp_tx_backpressure_decision)99U) == NULL,
          "TX decision strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_tx_backpressure_status_string(
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_EMPTY) != NULL &&
              hfoc_lwip_tcp_tx_backpressure_status_string(
                  (enum hfoc_lwip_tcp_tx_backpressure_status)99U) == NULL,
          "TX status strings cover known and unknown values");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_tx_backpressure gate;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy =
        make_policy(256U, 64U);
    struct hfoc_lwip_tcp_tx_backpressure_policy bad_policy;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token second_token;
    struct hfoc_lwip_tcp_pool_token invalid_token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    struct tcp_pcb *other_pcb;
    enum hfoc_lwip_tcp_tx_backpressure_decision decision;
    uint32_t published_length;
    uint8_t response[64];
    uint8_t other_response[64];

    memset(&gate, 0, sizeof(gate));
    CHECK(hfoc_lwip_tcp_tx_backpressure_init(NULL) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX gate init rejects null gate");
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    CHECK(!hfoc_lwip_tcp_tx_backpressure_published(&gate) &&
              hfoc_lwip_tcp_tx_backpressure_active_count(&gate) == 0U &&
              hfoc_lwip_tcp_tx_backpressure_state(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_FREE &&
              !hfoc_lwip_tcp_tx_backpressure_response_pending(
                  &gate, &token) &&
              !hfoc_lwip_tcp_tx_backpressure_blocked(&gate, &token) &&
              hfoc_lwip_tcp_tx_backpressure_response_length(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_tx_backpressure_outstanding_bytes(
                  &gate, &token) == 0U,
          "unpublished TX gate accessors fail closed");

    CHECK(init_gate(&gate),
          "valid TX gate publishes fixed empty state");
    CHECK(hfoc_lwip_tcp_tx_backpressure_init(&gate) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_STATE,
          "published TX gate rejects double init");
    CHECK(gate.active_count == 0U &&
              gate.version == HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION &&
              hfoc_lwip_tcp_tx_backpressure_published(&gate),
          "published TX gate exposes exact empty state");

    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              NULL, &token, NULL, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX attach rejects null gate");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, NULL, NULL, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX attach rejects null token");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, NULL, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX attach rejects null PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, (struct tcp_pcb *)1U, NULL) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX attach rejects null policy");

    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 99U;
    invalid_token.generation = 1U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &invalid_token, (struct tcp_pcb *)1U,
              &policy) == HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN,
          "TX attach rejects invalid token index");
    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 0U;
    invalid_token.generation = 0U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &invalid_token, (struct tcp_pcb *)1U,
              &policy) == HFOC_LWIP_TCP_TX_BACKPRESSURE_BAD_TOKEN,
          "TX attach rejects invalid token generation");

    bad_policy = policy;
    bad_policy.version = 2U;
    pcb = make_pcb();
    CHECK(pcb != NULL, "TX validation fixture allocates PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY &&
              gate.active_count == 0U &&
              !gate.entries[token.index].active,
          "TX attach rejects invalid policy version without mutation");
    bad_policy = policy;
    bad_policy.response_capacity = 0U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY,
          "TX attach rejects zero response capacity");
    bad_policy = policy;
    bad_policy.response_capacity =
        HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX + 1U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY,
          "TX attach rejects over-limit response capacity");
    bad_policy = policy;
    bad_policy.chunk_capacity = 0U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY,
          "TX attach rejects zero chunk capacity");
    bad_policy = policy;
    bad_policy.chunk_capacity =
        HFOC_LWIP_TCP_TX_BACKPRESSURE_CHUNK_CAPACITY_MAX + 1U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_POLICY,
          "TX attach rejects over-limit chunk capacity");

    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              gate.active_count == 1U &&
              gate.entries[token.index].active &&
              gate.entries[token.index].pcb == pcb,
          "TX attach publishes one active PCB slot");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_ALREADY_ATTACHED &&
              gate.active_count == 1U,
          "TX attach rejects double attachment");

    fill_pattern(response, sizeof(response));
    fill_pattern(other_response, sizeof(other_response));
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              NULL, &token, pcb, response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX submit rejects null gate");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, NULL, pcb, response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX submit rejects null token");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, NULL, response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX submit rejects null PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, NULL, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX submit rejects null response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, response, 8U, NULL) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX submit rejects null decision");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              NULL, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX publish rejects null gate");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, NULL, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX publish rejects null published length");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, NULL) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX publish rejects null decision");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              NULL, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NULL,
          "TX sent rejects null gate");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE,
          "TX sent rejects absent response");

    other_pcb = make_pcb();
    CHECK(other_pcb != NULL,
          "TX validation fixture allocates second PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, other_pcb, response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB &&
              !gate.entries[token.index].response_pending,
          "TX submit rejects wrong PCB without mutation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, other_pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB,
          "TX publish rejects wrong PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, other_pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_PCB,
          "TX sent rejects wrong PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, response, 0U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_LENGTH,
          "TX submit rejects zero length");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, response, 257U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_REJECTED &&
              !gate.entries[token.index].response_pending,
          "TX submit rejects over-capacity response without mutation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE &&
              published_length == 0U,
          "TX publish rejects absent response");

    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED &&
              gate.entries[token.index].response_pending,
          "TX submit retains first bounded response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, other_response, 8U, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_PENDING &&
              gate.entries[token.index].response_length == 8U,
          "TX submit rejects pending response without mutation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_LIVE_OUTSTANDING &&
              gate.active_count == 1U,
          "TX detach rejects pending response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_sent(
                  &gate, &token, pcb, 8U) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_detach(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              gate.active_count == 0U,
          "TX validation fixture drains and detaches pending response");
    discard_pcb(pcb);

    CHECK(init_pool(&pool),
          "duplicate-PCB fixture initializes pool");
    CHECK(reserve_token(&pool, &token),
          "duplicate-PCB fixture reserves first token");
    CHECK(reserve_token(&pool, &second_token),
          "duplicate-PCB fixture reserves second token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "duplicate-PCB fixture allocates first PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
          "duplicate-PCB fixture attaches first PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &second_token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_DUPLICATE_PCB &&
              gate.active_count == 1U,
          "TX attach rejects duplicate live PCB");
    CHECK(hfoc_lwip_tcp_tx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &second_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "duplicate-PCB fixture releases gate and pool tokens");
    discard_pcb(pcb);
    discard_pcb(other_pcb);
}

static void test_connection_capacity(void)
{
    struct hfoc_lwip_tcp_tx_backpressure gate;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy =
        make_policy(256U, 64U);
    struct hfoc_lwip_tcp_pool_token tokens[
        HFOC_LWIP_TCP_POOL_SLOT_COUNT];
    struct hfoc_lwip_tcp_pool_token fifth;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcbs[HFOC_LWIP_TCP_POOL_SLOT_COUNT];
    uint32_t index;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "connection-capacity fixture initializes gate and pool");
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(reserve_token(&pool, &tokens[index]),
              "connection-capacity fixture reserves slot");
        pcbs[index] = make_pcb();
        CHECK(pcbs[index] != NULL,
              "connection-capacity fixture allocates PCB");
        CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
                  &gate, &tokens[index], pcbs[index], &policy) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
              "connection-capacity fixture attaches slot");
    }
    CHECK(gate.active_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "connection-capacity fixture reaches four active slots");
    memset(&fifth, 0, sizeof(fifth));
    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &fifth) ==
              HFOC_LWIP_TCP_POOL_EXHAUSTED &&
              gate.active_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "fifth connection capacity returns pool exhaustion");

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_tx_backpressure_detach(
                  &gate, &tokens[index]) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
                  hfoc_lwip_tcp_pool_release(
                      &pool, &tokens[index]) ==
                      HFOC_LWIP_TCP_POOL_OK,
              "connection-capacity fixture releases slot");
        discard_pcb(pcbs[index]);
    }
    CHECK(gate.active_count == 0U,
          "connection-capacity fixture returns to empty gate");
}

static void test_short_capacity_retry_exact_queue(void)
{
    struct hfoc_lwip_tcp_tx_backpressure gate;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy =
        make_policy(256U, 8U);
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_tx_backpressure_decision decision;
    uint32_t published_length;
    uint8_t response[50];
    uint8_t queued[50];
    uint8_t expected[50];
    uint32_t queued_length;

    fill_pattern(response, sizeof(response));
    memcpy(expected, response, sizeof(expected));
    CHECK(init_gate(&gate) && init_pool(&pool),
          "short-capacity fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &token),
          "short-capacity fixture reserves token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "short-capacity fixture allocates PCB");
    pcb->snd_buf = 20U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
          "short-capacity fixture attaches connection");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &token, pcb, response, sizeof(response),
              &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED &&
              gate.entries[token.index].response_length ==
                  sizeof(response),
          "short-capacity fixture retains submitted response");

    memset(response, 0xEE, sizeof(response));
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_SUBMITTED &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 8U &&
              pcb->snd_buf == 12U,
          "first publish sends exact bounded chunk");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 16U &&
              pcb->snd_buf == 4U,
          "second publish fills remaining short capacity");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 4U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 20U &&
              pcb->snd_buf == 0U,
          "third publish uses last short capacity byte");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY &&
              decision ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED &&
              published_length == 0U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 20U &&
              hfoc_lwip_tcp_tx_backpressure_retained_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_tx_backpressure_blocked(
                  &gate, &token),
          "publish retains response when send capacity is exhausted");

    queued_length = unsent_copy(pcb, queued, sizeof(queued));
    CHECK(queued_length == 20U &&
              memcmp(queued, expected, 20U) == 0U,
          "retained retry queue contains exact first bytes once");

    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 8U,
          "sent callback consumes first contiguous ACK");
    add_snd_buf(pcb, 8U);
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 28U,
          "retry after sent callback sends next exact chunk");

    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 16U,
          "second sent callback consumes next ACK");
    add_snd_buf(pcb, 8U);
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 36U,
          "second retry advances to next response slice");

    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 24U,
          "third sent callback consumes next ACK");
    add_snd_buf(pcb, 8U);
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 8U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 44U,
          "third retry sends next exact chunk");

    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 8U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 32U,
          "fourth sent callback consumes next ACK");
    add_snd_buf(pcb, 8U);
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == 6U &&
              hfoc_lwip_tcp_tx_backpressure_published_offset(
                  &gate, &token) == 50U,
          "final publish sends exact remaining bytes");

    queued_length = unsent_copy(pcb, queued, sizeof(queued));
    CHECK(queued_length == sizeof(expected) &&
              memcmp(queued, expected, sizeof(expected)) == 0U,
          "full retry queue contains every response byte exactly once");

    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_RETRY &&
              decision ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_DECISION_RETAINED &&
              published_length == 0U,
          "publish retains when response waits for final ACK");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 18U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              !hfoc_lwip_tcp_tx_backpressure_response_pending(
                  &gate, &token) &&
              hfoc_lwip_tcp_tx_backpressure_complete_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_tx_backpressure_state(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STATE_ACTIVE,
          "final sent callback completes retained response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "short-capacity fixture releases gate and pool");
    discard_pcb(pcb);
}

static void test_sent_validation_and_completion(void)
{
    struct hfoc_lwip_tcp_tx_backpressure gate;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy =
        make_policy(256U, 64U);
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_tx_backpressure_decision decision;
    uint32_t published_length;
    uint8_t response[30];

    fill_pattern(response, sizeof(response));
    CHECK(init_gate(&gate) && init_pool(&pool),
          "sent-validation fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &token),
          "sent-validation fixture reserves token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "sent-validation fixture allocates PCB");
    pcb->snd_buf = 64U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_submit(
                  &gate, &token, pcb, response, sizeof(response),
                  &decision) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
          "sent-validation fixture attaches and submits response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              published_length == sizeof(response),
          "sent-validation fixture publishes full response");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 0U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK,
          "sent rejects zero ACK length");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 31U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 0U,
          "sent rejects ACK beyond published window");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 10U,
          "sent consumes valid first ACK");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 21U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_INVALID_ACK &&
              hfoc_lwip_tcp_tx_backpressure_acked_offset(
                  &gate, &token) == 10U,
          "sent rejects out-of-order oversized ACK without mutation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 20U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_response_length(
                  &gate, &token) == 0U &&
              !hfoc_lwip_tcp_tx_backpressure_response_pending(
                  &gate, &token),
          "sent completes response with exact remaining ACK");
    CHECK(hfoc_lwip_tcp_tx_backpressure_sent(
              &gate, &token, pcb, 1U) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE,
          "sent rejects ACK after completion");
    CHECK(hfoc_lwip_tcp_tx_backpressure_publish(
              &gate, &token, pcb, &published_length, &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_NO_RESPONSE,
          "publish reports no response after completion");
    CHECK(hfoc_lwip_tcp_tx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "sent-validation fixture releases gate and pool");
    discard_pcb(pcb);
}

static void test_fifo_reuse_and_stale_tokens(void)
{
    struct hfoc_lwip_tcp_tx_backpressure gate;
    struct hfoc_lwip_tcp_tx_backpressure_policy policy =
        make_policy(256U, 64U);
    struct hfoc_lwip_tcp_pool_token old_token;
    struct hfoc_lwip_tcp_pool_token new_token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *old_pcb;
    struct tcp_pcb *new_pcb;
    enum hfoc_lwip_tcp_tx_backpressure_decision decision;
    uint32_t published_length;
    uint8_t response[8];
    uint8_t new_response[8];

    fill_pattern(response, sizeof(response));
    fill_pattern(new_response, sizeof(new_response));
    new_response[0] = 0x11U;
    CHECK(init_gate(&gate) && init_pool(&pool),
          "FIFO/reuse fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &old_token),
          "FIFO/reuse fixture reserves old token");
    old_pcb = make_pcb();
    CHECK(old_pcb != NULL, "FIFO/reuse fixture allocates old PCB");
    old_pcb->snd_buf = 64U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &old_token, old_pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
          "FIFO/reuse fixture attaches old generation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &old_token, old_pcb, response, sizeof(response),
              &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_publish(
                  &gate, &old_token, old_pcb, &published_length,
                  &decision) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_sent(
                  &gate, &old_token, old_pcb, 8U) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_detach(
                  &gate, &old_token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &old_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "FIFO/reuse fixture completes and releases old generation");
    discard_pcb(old_pcb);

    CHECK(reserve_token(&pool, &new_token),
          "FIFO/reuse fixture reserves advanced token");
    CHECK(new_token.index == old_token.index &&
              new_token.generation == old_token.generation + 1U,
          "FIFO/reuse fixture advances slot generation");
    new_pcb = make_pcb();
    CHECK(new_pcb != NULL, "FIFO/reuse fixture allocates new PCB");
    new_pcb->snd_buf = 64U;
    CHECK(hfoc_lwip_tcp_tx_backpressure_attach(
              &gate, &new_token, new_pcb, &policy) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK,
          "FIFO/reuse fixture reuses slot with new generation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &old_token, old_pcb, response, sizeof(response),
              &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN &&
              hfoc_lwip_tcp_tx_backpressure_publish(
                  &gate, &old_token, old_pcb, &published_length,
                  &decision) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN &&
              hfoc_lwip_tcp_tx_backpressure_sent(
                  &gate, &old_token, old_pcb, 1U) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN &&
              hfoc_lwip_tcp_tx_backpressure_detach(
                  &gate, &old_token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_STALE_TOKEN,
          "stale old token is rejected without mutation");
    CHECK(hfoc_lwip_tcp_tx_backpressure_submit(
              &gate, &new_token, new_pcb, new_response,
              sizeof(new_response), &decision) ==
              HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_publish(
                  &gate, &new_token, new_pcb, &published_length,
                  &decision) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_sent(
                  &gate, &new_token, new_pcb, 8U) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_tx_backpressure_detach(
                  &gate, &new_token) ==
                  HFOC_LWIP_TCP_TX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &new_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "new generation operates and releases cleanly");
    discard_pcb(new_pcb);
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "TX backpressure fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_attach_validation();
    test_connection_capacity();
    test_short_capacity_retry_exact_queue();
    test_sent_validation_and_completion();
    test_fifo_reuse_and_stale_tokens();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP TX backpressure fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP TX backpressure host tests: ALL OK\n");
    return 0;
}

#endif
