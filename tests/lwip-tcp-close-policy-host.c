#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_CLOSE_POLICY_HOST_TEST

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

static err_t close_policy_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err)
{
    (void)context;
    (void)connection;
    (void)event;
    (void)p;
    (void)len;
    (void)err;
    return ERR_OK;
}

static struct hfoc_lwip_tcp_conn_callbacks make_callbacks(void)
{
    struct hfoc_lwip_tcp_conn_callbacks callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = close_policy_event;
    callbacks.context = NULL;
    return callbacks;
}

static struct tcp_pcb *make_pcb(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb != NULL) {
        pcb->state = ESTABLISHED;
    }
    return pcb;
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

static bool publish_token(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct tcp_pcb *pcb = make_pcb();
    struct hfoc_lwip_tcp_conn_callbacks callbacks =
        make_callbacks();

    if (pcb == NULL) {
        return false;
    }
    if (hfoc_lwip_tcp_pool_publish(
            pool, token, pcb, &callbacks) !=
        HFOC_LWIP_TCP_POOL_OK) {
        tcp_abort(pcb);
        return false;
    }
    return true;
}

static bool init_quarantine(
    struct hfoc_lwip_tcp_quarantine *gate)
{
    memset(gate, 0, sizeof(*gate));
    return hfoc_lwip_tcp_quarantine_init(gate) ==
        HFOC_LWIP_TCP_QUARANTINE_OK;
}

static bool attach_quarantine(
    struct hfoc_lwip_tcp_quarantine *gate,
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_connection *connection =
        &pool->slots[token->index].connection;

    return hfoc_lwip_tcp_quarantine_attach(
        gate, token, connection, connection->pcb) ==
        HFOC_LWIP_TCP_QUARANTINE_OK;
}

static bool init_policy(
    struct hfoc_lwip_tcp_close_policy *policy)
{
    memset(policy, 0, sizeof(*policy));
    return hfoc_lwip_tcp_close_policy_init(policy) ==
        HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

static bool attach_policy(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    return hfoc_lwip_tcp_close_policy_attach(
        policy, pool, gate, token) ==
        HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

static void release_closed_slot(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    const struct hfoc_lwip_tcp_connection *connection =
        hfoc_lwip_tcp_pool_connection(pool, token->index);

    if (connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSING) {
        CHECK(hfoc_lwip_tcp_pool_abort(pool, token) ==
                  HFOC_LWIP_TCP_POOL_OK,
              "closing slot cleanup aborts residual TCP close");
    }
    connection = hfoc_lwip_tcp_pool_connection(pool, token->index);
    CHECK(connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
              connection->state == HFOC_LWIP_TCP_CONN_STATE_ABORTED,
          "closed slot cleanup leaves terminal connection state");
    CHECK(hfoc_lwip_tcp_close_policy_detach(
              policy, pool, gate, token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  gate, token) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(pool, token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "closed slot detaches policy, quarantine, and pool exactly once");
}

static void test_strings_and_mapping(void)
{
    CHECK(hfoc_lwip_tcp_close_policy_reason_string(
              HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN) != NULL &&
              hfoc_lwip_tcp_close_policy_reason_string(
                  (enum hfoc_lwip_tcp_close_policy_reason)99U) == NULL,
          "close policy reason strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_close_policy_action_string(
              HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE) != NULL &&
              hfoc_lwip_tcp_close_policy_action_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE) != NULL &&
              hfoc_lwip_tcp_close_policy_action_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT) != NULL &&
              hfoc_lwip_tcp_close_policy_action_string(
                  (enum hfoc_lwip_tcp_close_policy_action)99U) == NULL,
          "close policy action strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_close_policy_status_string(
              HFOC_LWIP_TCP_CLOSE_POLICY_OK) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_NULL) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_CLOSED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_CLOSE_FAILED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  HFOC_LWIP_TCP_CLOSE_POLICY_ABORT_FAILED) != NULL &&
              hfoc_lwip_tcp_close_policy_status_string(
                  (enum hfoc_lwip_tcp_close_policy_status)99U) == NULL,
          "close policy status strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_close_policy_action_for_reason(
              HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_ABORT &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE &&
              hfoc_lwip_tcp_close_policy_action_for_reason(
                  (enum hfoc_lwip_tcp_close_policy_reason)99U) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE,
          "close policy maps five reasons to exact close or abort action");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_close_policy policy;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool other_pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_quarantine other_gate;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token invalid;

    memset(&policy, 0, sizeof(policy));
    memset(&pool, 0, sizeof(pool));
    memset(&other_pool, 0, sizeof(other_pool));
    memset(&gate, 0, sizeof(gate));
    memset(&other_gate, 0, sizeof(other_gate));
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;

    CHECK(hfoc_lwip_tcp_close_policy_init(NULL) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_NULL,
          "close policy init rejects null policy");
    CHECK(!hfoc_lwip_tcp_close_policy_published(&policy) &&
              hfoc_lwip_tcp_close_policy_active_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_handled_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_close_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_abort_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_rejected_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_entry_handled_count(
                  &policy, &token) == 0U &&
              hfoc_lwip_tcp_close_policy_entry_close_count(
                  &policy, &token) == 0U &&
              hfoc_lwip_tcp_close_policy_entry_abort_count(
                  &policy, &token) == 0U &&
              hfoc_lwip_tcp_close_policy_entry_rejected_count(
                  &policy, &token) == 0U &&
              hfoc_lwip_tcp_close_policy_reason_count(
                  &policy, &token,
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN) == 0U &&
              hfoc_lwip_tcp_close_policy_last_reason(&policy) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE &&
              hfoc_lwip_tcp_close_policy_last_action(&policy) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_NONE &&
              hfoc_lwip_tcp_close_policy_last_status(&policy) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_OK,
          "unpublished close policy accessors fail closed");

    CHECK(init_policy(&policy),
          "valid close policy config publishes fixed four-slot state");
    CHECK(hfoc_lwip_tcp_close_policy_init(&policy) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_BAD_STATE,
          "published close policy rejects double init");
    CHECK(hfoc_lwip_tcp_close_policy_published(&policy) &&
              policy.version == HFOC_LWIP_TCP_CLOSE_POLICY_VERSION &&
              policy.active_count == 0U,
          "published close policy exposes exact empty state");

    CHECK(hfoc_lwip_tcp_close_policy_attach(
              NULL, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_NULL,
          "close policy attach rejects null policy");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, NULL, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_NULL,
          "close policy attach rejects null pool");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, NULL, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_NULL,
          "close policy attach rejects null quarantine");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &gate, NULL) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_NULL,
          "close policy attach rejects null token");

    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 99U;
    invalid.generation = 1U;
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &gate, &invalid) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN,
          "close policy attach rejects invalid token index");
    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 0U;
    invalid.generation = 0U;
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &gate, &invalid) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_BAD_TOKEN,
          "close policy attach rejects invalid token generation");

    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &other_pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL,
          "close policy attach rejects unpublished pool");
    CHECK(init_pool(&pool) && init_pool(&other_pool),
          "validation fixture initializes pools");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &other_pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_CONNECTION,
          "close policy attach rejects wrong pool token");

    CHECK(reserve_token(&pool, &token) &&
              publish_token(&pool, &token),
          "validation fixture publishes pool connection");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &other_gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE,
          "close policy attach rejects unpublished quarantine");
    CHECK(init_quarantine(&gate),
          "validation fixture initializes quarantine");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_QUARANTINE,
          "close policy attach requires matching quarantine attachment");
    CHECK(attach_quarantine(&gate, &pool, &token),
          "validation fixture attaches quarantine");
    CHECK(attach_policy(&policy, &pool, &gate, &token),
          "close policy attach publishes one active slot");
    CHECK(hfoc_lwip_tcp_close_policy_attach(
              &policy, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_ALREADY_ATTACHED,
          "close policy attach rejects double attachment");

    CHECK(hfoc_lwip_tcp_close_policy_detach(
              &policy, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_LIVE_CONNECTION,
          "close policy detach rejects live connection");
    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK,
          "validation fixture aborts live connection");
    CHECK(hfoc_lwip_tcp_close_policy_detach(
              &policy, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "aborted close policy slot detaches and releases exactly once");
}

static void run_one_reason(
    enum hfoc_lwip_tcp_close_policy_reason reason)
{
    struct hfoc_lwip_tcp_close_policy policy;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token second;
    enum hfoc_lwip_tcp_close_policy_action action;
    enum hfoc_lwip_tcp_close_policy_status status;
    const struct hfoc_lwip_tcp_connection *first_conn;

    CHECK(init_pool(&pool) &&
              init_quarantine(&gate) &&
              init_policy(&policy),
          "close reason fixture initializes pool, gate, and policy");
    CHECK(reserve_token(&pool, &first) &&
              reserve_token(&pool, &second),
          "close reason fixture reserves two tokens");
    CHECK(publish_token(&pool, &first) &&
              publish_token(&pool, &second),
          "close reason fixture publishes two connections");
    CHECK(attach_quarantine(&gate, &pool, &first) &&
              attach_quarantine(&gate, &pool, &second) &&
              attach_policy(&policy, &pool, &gate, &first) &&
              attach_policy(&policy, &pool, &gate, &second),
          "close reason fixture attaches both slots");

    if (reason ==
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN) {
        CHECK(hfoc_lwip_tcp_conn_recv(
                  &pool.slots[first.index].connection,
                  pool.slots[first.index].connection.pcb,
                  NULL, ERR_OK) == HFOC_LWIP_TCP_CONN_OK &&
                  pool.slots[first.index].connection.state ==
                      HFOC_LWIP_TCP_CONN_STATE_CLOSING,
              "graceful FIN fixture records peer close state");
    }

    action = hfoc_lwip_tcp_close_policy_action_for_reason(reason);
    status = hfoc_lwip_tcp_close_policy_handle(
        &policy, &pool, &gate, &first, reason);
    first_conn = hfoc_lwip_tcp_pool_connection(&pool, first.index);
    if (action == HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE) {
        CHECK(status == HFOC_LWIP_TCP_CLOSE_POLICY_CLOSED &&
                  (first_conn->state ==
                       HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
                   first_conn->state ==
                       HFOC_LWIP_TCP_CONN_STATE_CLOSING),
              "close reason publishes exact close cleanup state");
    } else {
        CHECK(status == HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED &&
                  first_conn->state ==
                      HFOC_LWIP_TCP_CONN_STATE_ABORTED,
              "abort reason publishes exact abort cleanup state");
    }
    CHECK(hfoc_lwip_tcp_pool_connection(&pool, second.index) != NULL &&
              hfoc_lwip_tcp_pool_connection(
                  &pool, second.index)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
          "close reason leaves unrelated connection established");
    CHECK(hfoc_lwip_tcp_close_policy_handled_count(&policy) == 1U &&
              hfoc_lwip_tcp_close_policy_entry_handled_count(
                  &policy, &first) == 1U &&
              hfoc_lwip_tcp_close_policy_reason_count(
                  &policy, &first, reason) == 1U &&
              hfoc_lwip_tcp_close_policy_last_reason(&policy) == reason &&
              hfoc_lwip_tcp_close_policy_last_action(&policy) == action &&
              hfoc_lwip_tcp_close_policy_last_status(&policy) == status &&
              hfoc_lwip_tcp_close_policy_last_index(&policy) ==
                  first.index &&
              hfoc_lwip_tcp_close_policy_last_generation(&policy) ==
                  first.generation,
          "close reason records exact handled and reason evidence");
    if (action == HFOC_LWIP_TCP_CLOSE_POLICY_ACTION_CLOSE) {
        CHECK(hfoc_lwip_tcp_close_policy_close_count(&policy) == 1U &&
                  hfoc_lwip_tcp_close_policy_abort_count(&policy) == 0U &&
                  hfoc_lwip_tcp_close_policy_entry_close_count(
                      &policy, &first) == 1U,
              "close reason records exact close counter");
    } else {
        CHECK(hfoc_lwip_tcp_close_policy_abort_count(&policy) == 1U &&
                  hfoc_lwip_tcp_close_policy_close_count(&policy) == 0U &&
                  hfoc_lwip_tcp_close_policy_entry_abort_count(
                      &policy, &first) == 1U,
              "abort reason records exact abort counter");
    }
    CHECK(hfoc_lwip_tcp_quarantine_accepted_count(
              &gate, &first) == 1U,
          "close reason passes exactly one quarantine check");

    release_closed_slot(&policy, &pool, &gate, &first);
    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &second) ==
              HFOC_LWIP_TCP_POOL_OK,
          "unaffected slot aborts for cleanup");
    release_closed_slot(&policy, &pool, &gate, &second);
}

static void test_five_reason_policy(void)
{
    run_one_reason(
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_GRACEFUL_FIN);
    run_one_reason(
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE);
    run_one_reason(
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT);
    run_one_reason(
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_LINK_RESET);
    run_one_reason(
        HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN);
}

static void test_invalid_reason_and_stale_reuse(void)
{
    struct hfoc_lwip_tcp_close_policy policy;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool wrong_pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token stale;
    struct hfoc_lwip_tcp_pool_token reused;

    CHECK(init_pool(&pool) &&
              init_pool(&wrong_pool) &&
              init_quarantine(&gate) &&
              init_policy(&policy),
          "invalid/stale close fixture initializes boundaries");
    CHECK(reserve_token(&pool, &first) &&
              publish_token(&pool, &first),
          "invalid/stale close fixture publishes first connection");
    CHECK(attach_quarantine(&gate, &pool, &first) &&
              attach_policy(&policy, &pool, &gate, &first),
          "invalid/stale close fixture attaches first connection");

    CHECK(hfoc_lwip_tcp_close_policy_handle(
              &policy, &pool, &gate, &first,
              HFOC_LWIP_TCP_CLOSE_POLICY_REASON_NONE) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_REASON &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_close_policy_close_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_abort_count(&policy) == 0U,
          "invalid close reason is rejected without cleanup");
    CHECK(hfoc_lwip_tcp_close_policy_handle(
              &policy, &wrong_pool, &gate, &first,
              HFOC_LWIP_TCP_CLOSE_POLICY_REASON_PROTOCOL_CLOSE) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_INVALID_POOL &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
          "wrong pool close handle is rejected without mutation");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &first) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_close_policy_detach(
                  &policy, &pool, &gate, &first) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &first) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &first) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "invalid/stale close fixture releases first generation");

    stale = first;
    CHECK(reserve_token(&pool, &reused) &&
              publish_token(&pool, &reused) &&
              attach_quarantine(&gate, &pool, &reused) &&
              attach_policy(&policy, &pool, &gate, &reused),
          "invalid/stale close fixture reuses advanced generation");
    CHECK(hfoc_lwip_tcp_close_policy_handle(
              &policy, &pool, &gate, &stale,
              HFOC_LWIP_TCP_CLOSE_POLICY_REASON_SERVICE_SHUTDOWN) ==
              HFOC_LWIP_TCP_CLOSE_POLICY_STALE_TOKEN &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_close_policy_close_count(&policy) == 0U &&
              hfoc_lwip_tcp_close_policy_abort_count(&policy) == 0U,
          "stale close token cannot mutate a reused connection");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &reused) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_close_policy_detach(
                  &policy, &pool, &gate, &reused) ==
                  HFOC_LWIP_TCP_CLOSE_POLICY_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &reused) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &reused) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reused close policy generation is released cleanly");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "close policy fixture starts fixed lwIP allocator");

    test_strings_and_mapping();
    test_init_and_attach_validation();
    test_five_reason_policy();
    test_invalid_reason_and_stale_reuse();

    if (failures != 0) {
        fprintf(stderr, "%d close policy fixture failure(s)\n",
                failures);
        return 1;
    }
    printf("lwIP peer close and local abort policy host fixture: OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
