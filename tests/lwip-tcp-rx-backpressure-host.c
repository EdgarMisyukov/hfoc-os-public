#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_rx_backpressure.h"

#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_RX_BACKPRESSURE_HOST_TEST

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
    struct hfoc_lwip_tcp_rx_backpressure *gate)
{
    memset(gate, 0, sizeof(*gate));
    return hfoc_lwip_tcp_rx_backpressure_init(gate) ==
        HFOC_LWIP_TCP_RX_BACKPRESSURE_OK;
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

static struct hfoc_lwip_tcp_rx_backpressure_policy make_policy(
    enum hfoc_lwip_tcp_rx_backpressure_mode mode,
    uint32_t packet_capacity,
    uint32_t application_capacity)
{
    struct hfoc_lwip_tcp_rx_backpressure_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION;
    policy.mode = mode;
    policy.packet_capacity = packet_capacity;
    policy.application_capacity = application_capacity;
    return policy;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_rx_backpressure_mode_string(
              HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_mode_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_mode_string(
                  (enum hfoc_lwip_tcp_rx_backpressure_mode)99U) == NULL,
          "RX mode strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_rx_backpressure_state_string(
              HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_state_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_state_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_state_string(
                  (enum hfoc_lwip_tcp_rx_backpressure_state)99U) == NULL,
          "RX state strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_rx_backpressure_decision_string(
              HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_decision_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_decision_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_decision_string(
                  (enum hfoc_lwip_tcp_rx_backpressure_decision)99U) == NULL,
          "RX decision strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_rx_backpressure_status_string(
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING) != NULL &&
              hfoc_lwip_tcp_rx_backpressure_status_string(
                  (enum hfoc_lwip_tcp_rx_backpressure_status)99U) == NULL,
          "RX status strings cover known and unknown values");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_rx_backpressure gate;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy =
        make_policy(
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE, 4U, 2048U);
    struct hfoc_lwip_tcp_rx_backpressure_policy bad_policy;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token second_token;
    struct hfoc_lwip_tcp_pool_token invalid_token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    struct tcp_pcb *other_pcb;
    enum hfoc_lwip_tcp_rx_backpressure_decision decision;

    memset(&gate, 0, sizeof(gate));
    CHECK(hfoc_lwip_tcp_rx_backpressure_init(NULL) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX gate init rejects null gate");
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    CHECK(!hfoc_lwip_tcp_rx_backpressure_published(&gate) &&
              hfoc_lwip_tcp_rx_backpressure_active_count(&gate) == 0U &&
              hfoc_lwip_tcp_rx_backpressure_state(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_FREE &&
              !hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token) &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 0U,
          "unpublished RX gate accessors fail closed");

    CHECK(init_gate(&gate),
          "valid RX gate publishes fixed empty state");
    CHECK(hfoc_lwip_tcp_rx_backpressure_init(&gate) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_STATE,
          "published RX gate rejects double init");
    CHECK(gate.active_count == 0U &&
              gate.version == HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION &&
              hfoc_lwip_tcp_rx_backpressure_published(&gate),
          "published RX gate exposes exact empty state");

    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              NULL, &token, NULL, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX attach rejects null gate");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, NULL, NULL, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX attach rejects null token");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, NULL, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX attach rejects null PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, (struct tcp_pcb *)1U, NULL) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX attach rejects null policy");

    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 99U;
    invalid_token.generation = 1U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &invalid_token, (struct tcp_pcb *)1U,
              &policy) == HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN,
          "RX attach rejects invalid token index");
    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 0U;
    invalid_token.generation = 0U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &invalid_token, (struct tcp_pcb *)1U,
              &policy) == HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_TOKEN,
          "RX attach rejects invalid token generation");

    bad_policy = policy;
    bad_policy.version = 2U;
    pcb = make_pcb();
    CHECK(pcb != NULL, "RX validation fixture allocates PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY &&
              gate.active_count == 0U &&
              !gate.entries[token.index].active,
          "RX attach rejects invalid policy version without mutation");
    bad_policy = policy;
    bad_policy.mode =
        (enum hfoc_lwip_tcp_rx_backpressure_mode)99U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY,
          "RX attach rejects invalid mode");
    bad_policy = policy;
    bad_policy.packet_capacity = 0U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY,
          "RX attach rejects zero packet capacity");
    bad_policy = policy;
    bad_policy.packet_capacity =
        HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX + 1U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY,
          "RX attach rejects over-limit packet capacity");
    bad_policy = policy;
    bad_policy.application_capacity = 0U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY,
          "RX attach rejects zero application capacity");
    bad_policy = policy;
    bad_policy.application_capacity =
        HFOC_LWIP_TCP_RX_BACKPRESSURE_APPLICATION_CAPACITY_MAX + 1U;
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &bad_policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_POLICY,
          "RX attach rejects over-limit application capacity");

    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              gate.active_count == 1U &&
              gate.entries[token.index].active &&
              gate.entries[token.index].pcb == pcb,
          "RX attach publishes one active PCB slot");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_ALREADY_ATTACHED &&
              gate.active_count == 1U,
          "RX attach rejects double attachment");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, NULL) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_NULL,
          "RX receive rejects null decision");
    other_pcb = make_pcb();
    CHECK(other_pcb != NULL,
          "RX validation fixture allocates second PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, other_pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 0U,
          "RX receive rejects wrong PCB without mutation");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, other_pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_PCB,
          "RX release rejects wrong PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 0U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_INVALID_LENGTH,
          "RX receive rejects zero length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 0U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE,
          "RX release rejects zero length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE,
          "RX release rejects empty outstanding queue");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT,
          "RX receive accepts valid first length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_LIVE_OUTSTANDING &&
              gate.active_count == 1U,
          "RX detach rejects live outstanding bytes");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "RX release drains validation outstanding length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              gate.active_count == 0U,
          "RX detach succeeds after outstanding drain");
    discard_pcb(pcb);
    discard_pcb(other_pcb);

    CHECK(init_pool(&pool),
          "duplicate-PCB fixture initializes pool");
    CHECK(reserve_token(&pool, &token),
          "duplicate-PCB fixture reserves first token");
    CHECK(reserve_token(&pool, &second_token),
          "duplicate-PCB fixture reserves second token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "duplicate-PCB fixture allocates first PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "duplicate-PCB fixture attaches first PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &second_token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_DUPLICATE_PCB &&
              gate.active_count == 1U,
          "RX attach rejects duplicate live PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &second_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "duplicate-PCB fixture releases gate and pool tokens");
    discard_pcb(pcb);
}

static void test_connection_capacity(void)
{
    struct hfoc_lwip_tcp_rx_backpressure gate;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy =
        make_policy(
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE, 4U, 2048U);
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
        CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
                  &gate, &tokens[index], pcbs[index], &policy) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
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
        CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
                  &gate, &tokens[index]) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
                  hfoc_lwip_tcp_pool_release(
                      &pool, &tokens[index]) ==
                      HFOC_LWIP_TCP_POOL_OK,
              "connection-capacity fixture releases slot");
        discard_pcb(pcbs[index]);
    }
    CHECK(gate.active_count == 0U,
          "connection-capacity fixture returns to empty gate");
}

static void test_pause_mode_window_resume(void)
{
    struct hfoc_lwip_tcp_rx_backpressure gate;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy =
        make_policy(
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE, 2U, 100U);
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_rx_backpressure_decision decision;
    uint32_t max_window;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "pause-mode fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &token),
          "pause-mode fixture reserves token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "pause-mode fixture allocates PCB");
    max_window = (uint32_t)TCP_WND_MAX(pcb);
    pcb->rcv_wnd = (tcpwnd_size_t)(max_window - 20U);
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "pause-mode fixture attaches connection");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 10U &&
              (uint32_t)pcb->rcv_wnd == max_window - 20U,
          "first accepted receive records credit without window ack");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_ACCEPT &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 2U,
          "second accepted receive fills packet capacity");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 2U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 20U &&
              hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token) &&
              hfoc_lwip_tcp_rx_backpressure_state(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_PAUSED &&
              hfoc_lwip_tcp_rx_backpressure_paused_count(
                  &gate, &token) == 1U &&
              (uint32_t)pcb->rcv_wnd == max_window - 20U,
          "packet exhaustion pauses without recording or acking bytes");

    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 10U &&
              !hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token) &&
              hfoc_lwip_tcp_rx_backpressure_state(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STATE_ACTIVE &&
              (uint32_t)pcb->rcv_wnd == max_window - 10U,
          "exact release resumes and publishes window once");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 100U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_PAUSED &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_PAUSE &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 10U &&
              hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token),
          "application exhaustion pauses without recording new bytes");

    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 0U &&
              !hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token) &&
              (uint32_t)pcb->rcv_wnd == max_window,
          "final release drains credits and resumes at full window");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE &&
              (uint32_t)pcb->rcv_wnd == max_window,
          "duplicate release after drain does not ack again");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 20U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 20U,
          "connection accepts new receive after explicit release");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 20U,
          "FIFO release rejects length mismatch without mutation");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 20U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "FIFO release accepts exact oldest length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "pause-mode fixture releases gate and pool");
    discard_pcb(pcb);
}

static void test_reject_mode_no_ack(void)
{
    struct hfoc_lwip_tcp_rx_backpressure gate;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy =
        make_policy(
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_REJECT, 2U, 100U);
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *pcb;
    enum hfoc_lwip_tcp_rx_backpressure_decision decision;
    uint32_t max_window;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "reject-mode fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &token),
          "reject-mode fixture reserves token");
    pcb = make_pcb();
    CHECK(pcb != NULL, "reject-mode fixture allocates PCB");
    max_window = (uint32_t)TCP_WND_MAX(pcb);
    pcb->rcv_wnd = (tcpwnd_size_t)(max_window - 20U);
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &token, pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "reject-mode fixture attaches connection");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 1U,
          "reject-mode fixture accepts first packet");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 2U,
          "reject-mode fixture fills packet capacity");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_REJECTED &&
              decision ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_DECISION_REJECT &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_packets(
                  &gate, &token) == 2U &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &token) == 20U &&
              hfoc_lwip_tcp_rx_backpressure_rejected_count(
                  &gate, &token) == 1U &&
              !hfoc_lwip_tcp_rx_backpressure_paused(&gate, &token) &&
              (uint32_t)pcb->rcv_wnd == max_window - 20U,
          "packet exhaustion rejects without recording or acking bytes");

    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              (uint32_t)pcb->rcv_wnd == max_window - 10U,
          "reject-mode release publishes first window increment");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              (uint32_t)pcb->rcv_wnd == max_window,
          "reject-mode release publishes second window increment");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &token, pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "reject-mode connection resumes after capacity release");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &token, pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_detach(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reject-mode fixture releases gate and pool");
    discard_pcb(pcb);
}

static void test_fifo_reuse_and_stale_tokens(void)
{
    struct hfoc_lwip_tcp_rx_backpressure gate;
    struct hfoc_lwip_tcp_rx_backpressure_policy policy =
        make_policy(
            HFOC_LWIP_TCP_RX_BACKPRESSURE_MODE_PAUSE, 4U, 2048U);
    struct hfoc_lwip_tcp_pool_token old_token;
    struct hfoc_lwip_tcp_pool_token new_token;
    struct hfoc_lwip_tcp_pool pool;
    struct tcp_pcb *old_pcb;
    struct tcp_pcb *new_pcb;
    enum hfoc_lwip_tcp_rx_backpressure_decision decision;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "FIFO/reuse fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &old_token),
          "FIFO/reuse fixture reserves old token");
    old_pcb = make_pcb();
    CHECK(old_pcb != NULL, "FIFO/reuse fixture allocates old PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &old_token, old_pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "FIFO/reuse fixture attaches old generation");

    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &old_token, old_pcb, 10U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_accepted_count(
                  &gate, &old_token) == 1U,
          "FIFO/reuse fixture records first accepted length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &old_token, old_pcb, 20U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_accepted_count(
                  &gate, &old_token) == 2U,
          "FIFO/reuse fixture records second accepted length");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &old_token, old_pcb, 20U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_BAD_RELEASE &&
              hfoc_lwip_tcp_rx_backpressure_outstanding_bytes(
                  &gate, &old_token) == 30U,
          "FIFO release rejects out-of-order length without mutation");
    CHECK(hfoc_lwip_tcp_rx_backpressure_release(
              &gate, &old_token, old_pcb, 10U) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_release(
                  &gate, &old_token, old_pcb, 20U) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_released_count(
                  &gate, &old_token) == 2U,
          "FIFO release drains accepted lengths in exact order");
    CHECK(hfoc_lwip_tcp_rx_backpressure_detach(
              &gate, &old_token) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &old_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "FIFO/reuse fixture releases old gate and pool slot");
    discard_pcb(old_pcb);

    CHECK(reserve_token(&pool, &new_token),
          "FIFO/reuse fixture reserves advanced token");
    CHECK(new_token.index == old_token.index &&
              new_token.generation == old_token.generation + 1U,
          "FIFO/reuse fixture advances slot generation");
    new_pcb = make_pcb();
    CHECK(new_pcb != NULL, "FIFO/reuse fixture allocates new PCB");
    CHECK(hfoc_lwip_tcp_rx_backpressure_attach(
              &gate, &new_token, new_pcb, &policy) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK,
          "FIFO/reuse fixture reuses slot with new generation");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &old_token, old_pcb, 1U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN &&
              hfoc_lwip_tcp_rx_backpressure_release(
                  &gate, &old_token, old_pcb, 1U) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN &&
              hfoc_lwip_tcp_rx_backpressure_detach(
                  &gate, &old_token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_STALE_TOKEN,
          "stale old token is rejected without mutation");
    CHECK(hfoc_lwip_tcp_rx_backpressure_receive(
              &gate, &new_token, new_pcb, 5U, &decision) ==
              HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_release(
                  &gate, &new_token, new_pcb, 5U) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_rx_backpressure_detach(
                  &gate, &new_token) ==
                  HFOC_LWIP_TCP_RX_BACKPRESSURE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &new_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "new generation operates and releases cleanly");
    discard_pcb(new_pcb);
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "RX backpressure fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_attach_validation();
    test_connection_capacity();
    test_pause_mode_window_resume();
    test_reject_mode_no_ack();
    test_fifo_reuse_and_stale_tokens();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP RX backpressure fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP RX backpressure host tests: ALL OK\n");
    return 0;
}

#endif
