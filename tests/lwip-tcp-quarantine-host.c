#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_QUARANTINE_HOST_TEST

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

static bool init_gate(
    struct hfoc_lwip_tcp_quarantine *gate)
{
    memset(gate, 0, sizeof(*gate));
    return hfoc_lwip_tcp_quarantine_init(gate) ==
        HFOC_LWIP_TCP_QUARANTINE_OK;
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

static void init_connection(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    uint32_t generation,
    enum hfoc_lwip_tcp_conn_state state)
{
    memset(connection, 0, sizeof(*connection));
    connection->version = HFOC_LWIP_TCP_CONN_VERSION;
    connection->published = true;
    connection->generation = generation;
    connection->state = state;
    connection->pcb = pcb;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_quarantine_kind_string(
              HFOC_LWIP_TCP_QUARANTINE_KIND_NONE) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_ACCEPT) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_SENT) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_POLL) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT) != NULL &&
              hfoc_lwip_tcp_quarantine_kind_string(
                  (enum hfoc_lwip_tcp_quarantine_kind)99U) == NULL,
          "quarantine kind strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_quarantine_reason_string(
              HFOC_LWIP_TCP_QUARANTINE_REASON_NONE) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_CONNECTION) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_STATE) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  HFOC_LWIP_TCP_QUARANTINE_REASON_INVALID_KIND) != NULL &&
              hfoc_lwip_tcp_quarantine_reason_string(
                  (enum hfoc_lwip_tcp_quarantine_reason)99U) == NULL,
          "quarantine reason strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_quarantine_status_string(
              HFOC_LWIP_TCP_QUARANTINE_OK) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_NULL) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION) != NULL &&
              hfoc_lwip_tcp_quarantine_status_string(
                  (enum hfoc_lwip_tcp_quarantine_status)99U) == NULL,
          "quarantine status strings cover known and unknown values");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token invalid;
    struct hfoc_lwip_tcp_connection connection;
    struct tcp_pcb *pcb;
    struct tcp_pcb *other_pcb;

    memset(&gate, 0, sizeof(gate));
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    CHECK(hfoc_lwip_tcp_quarantine_init(NULL) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine init rejects null gate");
    CHECK(!hfoc_lwip_tcp_quarantine_published(&gate) &&
              hfoc_lwip_tcp_quarantine_active_count(&gate) == 0U &&
              hfoc_lwip_tcp_quarantine_quarantined_count(&gate) == 0U &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_stale_token_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_invalid_connection_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_invalid_pcb_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_invalid_state_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_invalid_kind_count(
                  &gate, &token) == 0U &&
              hfoc_lwip_tcp_quarantine_kind_count(
                  &gate, &token,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) == 0U &&
              hfoc_lwip_tcp_quarantine_last_reason(&gate) ==
                  HFOC_LWIP_TCP_QUARANTINE_REASON_NONE &&
              hfoc_lwip_tcp_quarantine_last_kind(&gate) ==
                  HFOC_LWIP_TCP_QUARANTINE_KIND_NONE,
          "unpublished quarantine gate accessors fail closed");

    CHECK(init_gate(&gate),
          "valid quarantine gate publishes fixed empty state");
    CHECK(hfoc_lwip_tcp_quarantine_init(&gate) ==
              HFOC_LWIP_TCP_QUARANTINE_BAD_STATE,
          "published quarantine gate rejects double init");
    CHECK(gate.active_count == 0U &&
              gate.version == HFOC_LWIP_TCP_QUARANTINE_VERSION &&
              hfoc_lwip_tcp_quarantine_published(&gate),
          "published quarantine gate exposes exact empty state");

    CHECK(hfoc_lwip_tcp_quarantine_attach(
              NULL, &token, NULL, NULL) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine attach rejects null gate");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, NULL, NULL, NULL) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine attach rejects null token");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, NULL, NULL) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine attach rejects null connection");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, (struct hfoc_lwip_tcp_connection *)1U,
              NULL) == HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine attach rejects null PCB");

    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 99U;
    invalid.generation = 1U;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &invalid, (struct hfoc_lwip_tcp_connection *)1U,
              (struct tcp_pcb *)1U) ==
              HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN,
          "quarantine attach rejects invalid token index");
    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 0U;
    invalid.generation = 0U;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &invalid, (struct hfoc_lwip_tcp_connection *)1U,
              (struct tcp_pcb *)1U) ==
              HFOC_LWIP_TCP_QUARANTINE_BAD_TOKEN,
          "quarantine attach rejects invalid token generation");

    pcb = make_pcb();
    CHECK(pcb != NULL, "quarantine validation fixture allocates PCB");
    init_connection(&connection, pcb, 1U,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    connection.published = false;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION &&
              gate.active_count == 0U,
          "quarantine attach rejects unpublished connection");
    init_connection(&connection, pcb, 1U,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    connection.version = 0U;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION,
          "quarantine attach rejects invalid connection version");
    init_connection(&connection, pcb, 1U,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    connection.generation = 0U;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION,
          "quarantine attach rejects connection generation mismatch");
    init_connection(&connection, pcb, 1U,
                    HFOC_LWIP_TCP_CONN_STATE_INACTIVE);
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE,
          "quarantine attach rejects inactive connection state");

    init_connection(&connection, pcb, 1U,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    other_pcb = make_pcb();
    CHECK(other_pcb != NULL,
          "quarantine validation fixture allocates second PCB");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, other_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB &&
              gate.active_count == 0U,
          "quarantine attach rejects PCB not owned by connection");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_OK &&
              gate.active_count == 1U &&
              gate.entries[token.index].active &&
              gate.entries[token.index].connection == &connection &&
              gate.entries[token.index].pcb == pcb,
          "quarantine attach publishes one active slot");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_ALREADY_ATTACHED &&
              gate.active_count == 1U,
          "quarantine attach rejects double attachment");

    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_QUARANTINE_LIVE_CONNECTION,
          "quarantine detach rejects live connection");
    connection.state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
    connection.pcb = NULL;
    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &token) ==
              HFOC_LWIP_TCP_QUARANTINE_OK &&
              gate.active_count == 0U,
          "quarantine detach succeeds after close");
    discard_pcb(other_pcb);
    discard_pcb(pcb);
}

static void test_duplicate_and_capacity(void)
{
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token second;
    struct hfoc_lwip_tcp_pool_token tokens[
        HFOC_LWIP_TCP_POOL_SLOT_COUNT];
    struct hfoc_lwip_tcp_pool_token fifth;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_connection first_connection;
    struct hfoc_lwip_tcp_connection second_connection;
    struct hfoc_lwip_tcp_connection connections[
        HFOC_LWIP_TCP_POOL_SLOT_COUNT];
    struct tcp_pcb *first_pcb;
    struct tcp_pcb *second_pcb;
    struct tcp_pcb *pcbs[HFOC_LWIP_TCP_POOL_SLOT_COUNT];
    uint32_t index;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "duplicate/capacity fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &first),
          "duplicate fixture reserves first token");
    CHECK(reserve_token(&pool, &second),
          "duplicate fixture reserves second token");
    first_pcb = make_pcb();
    second_pcb = make_pcb();
    CHECK(first_pcb != NULL && second_pcb != NULL,
          "duplicate fixture allocates PCBs");
    init_connection(&first_connection, first_pcb, first.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    init_connection(&second_connection, second_pcb, second.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &first, &first_connection, first_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "duplicate fixture attaches first connection");
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &second, &first_connection, first_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB &&
              gate.active_count == 1U,
          "quarantine attach rejects duplicate PCB");
    second_connection.pcb = first_pcb;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &second, &second_connection, first_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_PCB,
          "quarantine attach rejects second connection on duplicate PCB");
    first_connection.pcb = second_pcb;
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &second, &first_connection, second_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_DUPLICATE_CONNECTION,
          "quarantine attach rejects duplicate connection pointer");
    first_connection.state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
    first_connection.pcb = NULL;
    second_connection.state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
    second_connection.pcb = NULL;
    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &first) == HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &first) ==
                  HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &second) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "duplicate fixture releases gate and pool state");
    discard_pcb(first_pcb);
    discard_pcb(second_pcb);

    CHECK(init_gate(&gate) && init_pool(&pool),
          "capacity fixture initializes gate and pool");
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(reserve_token(&pool, &tokens[index]),
              "capacity fixture reserves slot");
        pcbs[index] = make_pcb();
        CHECK(pcbs[index] != NULL,
              "capacity fixture allocates PCB");
        init_connection(&connections[index], pcbs[index],
                        tokens[index].generation,
                        HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
        CHECK(hfoc_lwip_tcp_quarantine_attach(
                  &gate, &tokens[index], &connections[index],
                  pcbs[index]) == HFOC_LWIP_TCP_QUARANTINE_OK,
              "capacity fixture attaches slot");
    }
    CHECK(gate.active_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "capacity fixture reaches four active slots");
    memset(&fifth, 0, sizeof(fifth));
    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &fifth) ==
              HFOC_LWIP_TCP_POOL_EXHAUSTED &&
              gate.active_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "fifth connection capacity returns pool exhaustion");

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        connections[index].state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
        connections[index].pcb = NULL;
        CHECK(hfoc_lwip_tcp_quarantine_detach(
                  &gate, &tokens[index]) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
                  hfoc_lwip_tcp_pool_release(
                      &pool, &tokens[index]) ==
                      HFOC_LWIP_TCP_POOL_OK,
              "capacity fixture releases slot");
        discard_pcb(pcbs[index]);
    }
    CHECK(gate.active_count == 0U,
          "capacity fixture returns to empty gate");
}

static void test_valid_and_invalid_check(void)
{
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_connection connection;
    struct hfoc_lwip_tcp_connection wrong_connection;
    struct tcp_pcb *pcb;
    struct tcp_pcb *other_pcb;
    uint32_t expected_accepted;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "check fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &token),
          "check fixture reserves token");
    pcb = make_pcb();
    other_pcb = make_pcb();
    CHECK(pcb != NULL && other_pcb != NULL,
          "check fixture allocates PCBs");
    init_connection(&connection, pcb, token.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    init_connection(&wrong_connection, pcb, token.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &token, &connection, pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "check fixture attaches connection");

    CHECK(hfoc_lwip_tcp_quarantine_check(
              NULL, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine check rejects null gate");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, NULL, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine check rejects null token");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, NULL, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_NULL,
          "quarantine check rejects null connection");

    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_quarantine_kind_count(
                  &gate, &token,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) == 1U,
          "valid receive callback is accepted with exact evidence");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_SENT) ==
              HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_check(
                  &gate, &token, &connection, pcb,
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_POLL) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_check(
                  &gate, &token, &connection, NULL,
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_ERROR) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_check(
                  &gate, &token, &connection, NULL,
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_check(
                  &gate, &token, &connection, pcb,
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
                  HFOC_LWIP_TCP_QUARANTINE_KIND_CLOSE) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &token) == 6U,
          "established callbacks cover all allowed kinds");

    expected_accepted = hfoc_lwip_tcp_quarantine_accepted_count(
        &gate, &token);
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &wrong_connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_CONNECTION &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &token) == expected_accepted &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_quarantine_invalid_connection_count(
                  &gate, &token) == 1U &&
              gate.entries[token.index].connection == &connection &&
              gate.entries[token.index].active,
          "wrong connection pointer is evidence-only and does not mutate entry");

    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, other_pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_PCB &&
              hfoc_lwip_tcp_quarantine_invalid_pcb_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &token) == 2U,
          "wrong PCB is evidence-only");

    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_CLOSING,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE &&
              hfoc_lwip_tcp_quarantine_invalid_state_count(
                  &gate, &token) == 1U,
          "wrong expected state is evidence-only");

    connection.state = HFOC_LWIP_TCP_CONN_STATE_CLOSING;
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_CLOSING,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND &&
              hfoc_lwip_tcp_quarantine_invalid_kind_count(
                  &gate, &token) == 1U &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &token) == 4U,
          "disallowed callback kind is evidence-only");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_CLOSING,
              HFOC_LWIP_TCP_QUARANTINE_KIND_SENT) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "closing connection accepts sent callback");

    connection.state = HFOC_LWIP_TCP_CONN_STATE_ABORTED;
    connection.pcb = NULL;
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE &&
              connection.state == HFOC_LWIP_TCP_CONN_STATE_ABORTED &&
              connection.pcb == NULL &&
              gate.entries[token.index].active &&
              gate.entries[token.index].connection == &connection &&
              gate.entries[token.index].pcb == pcb,
          "delayed callback after abort is evidence-only");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &token, &connection, NULL,
              HFOC_LWIP_TCP_CONN_STATE_ABORTED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_KIND,
          "aborted connection rejects abort callback kind");

    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &token) == HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "check fixture releases gate and pool after abort");
    discard_pcb(other_pcb);
    discard_pcb(pcb);
}

static void test_not_attached_and_reuse_evidence(void)
{
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token old_token;
    struct hfoc_lwip_tcp_pool_token new_token;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_connection old_connection;
    struct hfoc_lwip_tcp_connection new_connection;
    struct tcp_pcb *old_pcb;
    struct tcp_pcb *new_pcb;
    uint32_t quarantine_before;

    CHECK(init_gate(&gate) && init_pool(&pool),
          "reuse fixture initializes gate and pool");
    CHECK(reserve_token(&pool, &old_token),
          "reuse fixture reserves old token");
    old_pcb = make_pcb();
    CHECK(old_pcb != NULL, "reuse fixture allocates old PCB");
    init_connection(&old_connection, old_pcb, old_token.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &old_token, &old_connection, old_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "reuse fixture attaches old generation");

    old_connection.state = HFOC_LWIP_TCP_CONN_STATE_ABORTED;
    old_connection.pcb = NULL;
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &old_token, &old_connection, old_pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_INVALID_STATE &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &old_token) == 1U,
          "reuse fixture records abort-delayed callback evidence");
    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &old_token) == HFOC_LWIP_TCP_QUARANTINE_OK,
          "reuse fixture detaches old generation");
    quarantine_before = hfoc_lwip_tcp_quarantine_quarantined_count(
        &gate);
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &old_token, &old_connection, old_pcb,
              HFOC_LWIP_TCP_CONN_STATE_ABORTED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_ABORT) ==
              HFOC_LWIP_TCP_QUARANTINE_NOT_ATTACHED &&
              hfoc_lwip_tcp_quarantine_quarantined_count(
                  &gate) == quarantine_before + 1U &&
              hfoc_lwip_tcp_quarantine_last_reason(&gate) ==
                  HFOC_LWIP_TCP_QUARANTINE_REASON_NOT_ATTACHED,
          "callback after detach is evidence-only");
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &old_token) ==
              HFOC_LWIP_TCP_POOL_OK,
          "reuse fixture releases old pool token");
    discard_pcb(old_pcb);

    CHECK(reserve_token(&pool, &new_token),
          "reuse fixture reserves advanced token");
    CHECK(new_token.index == old_token.index &&
              new_token.generation == old_token.generation + 1U,
          "reuse fixture advances slot generation");
    new_pcb = make_pcb();
    CHECK(new_pcb != NULL, "reuse fixture allocates new PCB");
    init_connection(&new_connection, new_pcb, new_token.generation,
                    HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED);
    CHECK(hfoc_lwip_tcp_quarantine_attach(
              &gate, &new_token, &new_connection, new_pcb) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "reuse fixture reuses slot with new generation");
    quarantine_before = hfoc_lwip_tcp_quarantine_quarantined_count(
        &gate);
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &old_token, &old_connection, old_pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_STALE_TOKEN &&
              hfoc_lwip_tcp_quarantine_quarantined_count(
                  &gate) == quarantine_before + 1U &&
              hfoc_lwip_tcp_quarantine_last_reason(&gate) ==
                  HFOC_LWIP_TCP_QUARANTINE_REASON_STALE_TOKEN &&
              gate.entries[new_token.index].active &&
              gate.entries[new_token.index].connection ==
                  &new_connection &&
              gate.entries[new_token.index].pcb == new_pcb &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &new_token) == 0U &&
              hfoc_lwip_tcp_quarantine_entry_quarantined_count(
                  &gate, &new_token) == 0U &&
              hfoc_lwip_tcp_quarantine_stale_token_count(
                  &gate, &new_token) == 0U,
          "stale token callback is evidence-only and does not mutate new entry");
    CHECK(hfoc_lwip_tcp_quarantine_check(
              &gate, &new_token, &new_connection, new_pcb,
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
              HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
              HFOC_LWIP_TCP_QUARANTINE_OK,
          "new generation accepts its own callback");

    new_connection.state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
    new_connection.pcb = NULL;
    CHECK(hfoc_lwip_tcp_quarantine_detach(
              &gate, &new_token) == HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &new_token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reuse fixture releases new gate and pool state");
    discard_pcb(new_pcb);
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "quarantine fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_attach_validation();
    test_duplicate_and_capacity();
    test_valid_and_invalid_check();
    test_not_attached_and_reuse_evidence();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP stale callback quarantine fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP stale callback quarantine host tests: ALL OK\n");
    return 0;
}

#endif
