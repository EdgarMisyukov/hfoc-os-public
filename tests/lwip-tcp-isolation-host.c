#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_isolation.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_ISOLATION_HOST_TEST

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

static err_t isolation_pool_event(
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
    callbacks.event = isolation_pool_event;
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

static bool init_matrix(
    struct hfoc_lwip_tcp_isolation *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    return hfoc_lwip_tcp_isolation_init(matrix) ==
        HFOC_LWIP_TCP_ISOLATION_OK;
}

static bool attach_matrix(
    struct hfoc_lwip_tcp_isolation *matrix,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    return hfoc_lwip_tcp_isolation_attach(
        matrix, pool, gate, token) ==
        HFOC_LWIP_TCP_ISOLATION_OK;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_isolation_event_string(
              HFOC_LWIP_TCP_ISOLATION_EVENT_NONE) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  HFOC_LWIP_TCP_ISOLATION_EVENT_RESET) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT) != NULL &&
              hfoc_lwip_tcp_isolation_event_string(
                  (enum hfoc_lwip_tcp_isolation_event)99U) == NULL,
          "isolation event strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_isolation_status_string(
              HFOC_LWIP_TCP_ISOLATION_OK) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_NULL) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_INVALID_POOL) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_ABORTED) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_ALREADY_CLOSED) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_CLOSE_FAILED) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_isolation_status_string(
                  (enum hfoc_lwip_tcp_isolation_status)99U) == NULL,
          "isolation status strings cover known and unknown values");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_isolation matrix;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool other_pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_quarantine other_gate;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token invalid;

    memset(&matrix, 0, sizeof(matrix));
    memset(&pool, 0, sizeof(pool));
    memset(&other_pool, 0, sizeof(other_pool));
    memset(&gate, 0, sizeof(gate));
    memset(&other_gate, 0, sizeof(other_gate));
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;

    CHECK(hfoc_lwip_tcp_isolation_init(NULL) ==
              HFOC_LWIP_TCP_ISOLATION_NULL,
          "isolation init rejects null matrix");
    CHECK(!hfoc_lwip_tcp_isolation_published(&matrix) &&
              hfoc_lwip_tcp_isolation_active_count(&matrix) == 0U &&
              hfoc_lwip_tcp_isolation_closed_count(&matrix) == 0U &&
              hfoc_lwip_tcp_isolation_rejected_count(&matrix) == 0U &&
              hfoc_lwip_tcp_isolation_entry_event_count(
                  &matrix, &token) == 0U &&
              hfoc_lwip_tcp_isolation_entry_close_count(
                  &matrix, &token) == 0U &&
              hfoc_lwip_tcp_isolation_entry_rejected_count(
                  &matrix, &token) == 0U &&
              hfoc_lwip_tcp_isolation_event_count(
                  &matrix, &token,
                  HFOC_LWIP_TCP_ISOLATION_EVENT_RESET) == 0U &&
              hfoc_lwip_tcp_isolation_last_event(&matrix) ==
                  HFOC_LWIP_TCP_ISOLATION_EVENT_NONE &&
              hfoc_lwip_tcp_isolation_last_status(&matrix) ==
                  HFOC_LWIP_TCP_ISOLATION_OK,
          "unpublished isolation matrix accessors fail closed");

    CHECK(init_matrix(&matrix),
          "valid matrix config publishes fixed four-slot state");
    CHECK(hfoc_lwip_tcp_isolation_init(&matrix) ==
              HFOC_LWIP_TCP_ISOLATION_BAD_STATE,
          "published matrix rejects double init");
    CHECK(hfoc_lwip_tcp_isolation_published(&matrix) &&
              matrix.version == HFOC_LWIP_TCP_ISOLATION_VERSION &&
              matrix.active_count == 0U,
          "published matrix exposes exact empty state");

    CHECK(hfoc_lwip_tcp_isolation_attach(
              NULL, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_NULL,
          "isolation attach rejects null matrix");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, NULL, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_NULL,
          "isolation attach rejects null pool");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, NULL, &token) ==
              HFOC_LWIP_TCP_ISOLATION_NULL,
          "isolation attach rejects null quarantine");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &gate, NULL) ==
              HFOC_LWIP_TCP_ISOLATION_NULL,
          "isolation attach rejects null token");

    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 99U;
    invalid.generation = 1U;
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &gate, &invalid) ==
              HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN,
          "isolation attach rejects invalid token index");
    memset(&invalid, 0, sizeof(invalid));
    invalid.index = 0U;
    invalid.generation = 0U;
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &gate, &invalid) ==
              HFOC_LWIP_TCP_ISOLATION_BAD_TOKEN,
          "isolation attach rejects invalid token generation");

    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &other_pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_POOL,
          "isolation attach rejects unpublished pool");
    CHECK(init_pool(&pool) && init_pool(&other_pool),
          "validation fixture initializes pools");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &other_pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_CONNECTION,
          "isolation attach rejects wrong pool token");

    CHECK(reserve_token(&pool, &token) &&
              publish_token(&pool, &token),
          "validation fixture publishes pool connection");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &other_gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE,
          "isolation attach rejects unpublished quarantine");
    CHECK(init_quarantine(&gate),
          "validation fixture initializes quarantine");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_QUARANTINE,
          "isolation attach requires matching quarantine attachment");
    CHECK(attach_quarantine(&gate, &pool, &token),
          "validation fixture attaches quarantine");
    CHECK(attach_matrix(&matrix, &pool, &gate, &token),
          "isolation attach publishes one active slot");
    CHECK(hfoc_lwip_tcp_isolation_attach(
              &matrix, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_ALREADY_ATTACHED,
          "isolation attach rejects double attachment");

    CHECK(hfoc_lwip_tcp_isolation_detach(
              &matrix, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_LIVE_CONNECTION,
          "isolation detach rejects live connection");
    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK,
          "validation fixture aborts live connection");
    CHECK(hfoc_lwip_tcp_isolation_detach(
              &matrix, &pool, &gate, &token) ==
              HFOC_LWIP_TCP_ISOLATION_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &token) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "aborted matrix slot detaches and releases exactly once");
}

static void run_one_event(
    enum hfoc_lwip_tcp_isolation_event event)
{
    struct hfoc_lwip_tcp_isolation matrix;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token second;

    CHECK(init_pool(&pool) &&
              init_quarantine(&gate) &&
              init_matrix(&matrix),
          "isolation event fixture initializes pool, gate, and matrix");
    CHECK(reserve_token(&pool, &first) &&
              reserve_token(&pool, &second),
          "isolation event fixture reserves two tokens");
    CHECK(publish_token(&pool, &first) &&
              publish_token(&pool, &second),
          "isolation event fixture publishes two connections");
    CHECK(attach_quarantine(&gate, &pool, &first) &&
              attach_quarantine(&gate, &pool, &second) &&
              attach_matrix(&matrix, &pool, &gate, &first) &&
              attach_matrix(&matrix, &pool, &gate, &second),
          "isolation event fixture attaches both slots");

    CHECK(hfoc_lwip_tcp_isolation_inject(
              &matrix, &pool, &gate, &first, event) ==
              HFOC_LWIP_TCP_ISOLATION_ABORTED,
          "malformed event aborts affected connection");
    CHECK(hfoc_lwip_tcp_pool_connection(&pool, 0U) != NULL &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ABORTED &&
              hfoc_lwip_tcp_pool_connection(&pool, 1U) != NULL &&
              hfoc_lwip_tcp_pool_connection(&pool, 1U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
          "malformed event closes only affected connection");
    CHECK(hfoc_lwip_tcp_isolation_active_count(&matrix) == 2U &&
              hfoc_lwip_tcp_isolation_closed_count(&matrix) == 1U &&
              hfoc_lwip_tcp_isolation_entry_event_count(
                  &matrix, &first) == 1U &&
              hfoc_lwip_tcp_isolation_entry_close_count(
                  &matrix, &first) == 1U &&
              hfoc_lwip_tcp_isolation_event_count(
                  &matrix, &first, event) == 1U &&
              hfoc_lwip_tcp_isolation_last_event(&matrix) == event &&
              hfoc_lwip_tcp_isolation_last_status(&matrix) ==
                  HFOC_LWIP_TCP_ISOLATION_ABORTED &&
              hfoc_lwip_tcp_quarantine_accepted_count(
                  &gate, &first) == 1U,
          "malformed event records exact isolation evidence");

    CHECK(hfoc_lwip_tcp_isolation_detach(
              &matrix, &pool, &gate, &first) ==
              HFOC_LWIP_TCP_ISOLATION_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &first) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &first) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "affected slot detaches and releases after abort");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &second) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_isolation_detach(
                  &matrix, &pool, &gate, &second) ==
                  HFOC_LWIP_TCP_ISOLATION_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &second) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &second) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "unaffected slot is released cleanly after isolation");
}

static void test_five_event_isolation(void)
{
    run_one_event(HFOC_LWIP_TCP_ISOLATION_EVENT_RESET);
    run_one_event(
        HFOC_LWIP_TCP_ISOLATION_EVENT_DUPLICATE_COMPLETION);
    run_one_event(
        HFOC_LWIP_TCP_ISOLATION_EVENT_INVALID_CALLBACK_ORDER);
    run_one_event(HFOC_LWIP_TCP_ISOLATION_EVENT_SHORT_RECEIVE);
    run_one_event(HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT);
}

static void test_invalid_event_and_stale_reuse(void)
{
    struct hfoc_lwip_tcp_isolation matrix;
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool wrong_pool;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token stale;
    struct hfoc_lwip_tcp_pool_token reused;

    CHECK(init_pool(&pool) &&
              init_pool(&wrong_pool) &&
              init_quarantine(&gate) &&
              init_matrix(&matrix),
          "invalid/stale fixture initializes boundaries");
    CHECK(reserve_token(&pool, &first) &&
              publish_token(&pool, &first),
          "invalid/stale fixture publishes first connection");
    CHECK(attach_quarantine(&gate, &pool, &first) &&
              attach_matrix(&matrix, &pool, &gate, &first),
          "invalid/stale fixture attaches first connection");

    CHECK(hfoc_lwip_tcp_isolation_inject(
              &matrix, &pool, &gate, &first,
              HFOC_LWIP_TCP_ISOLATION_EVENT_NONE) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_EVENT &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_isolation_closed_count(&matrix) == 0U,
          "invalid event is rejected without closing connection");
    CHECK(hfoc_lwip_tcp_isolation_inject(
              &matrix, &wrong_pool, &gate, &first,
              HFOC_LWIP_TCP_ISOLATION_EVENT_RESET) ==
              HFOC_LWIP_TCP_ISOLATION_INVALID_POOL &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
          "wrong pool injection is rejected without mutation");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &first) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_isolation_detach(
                  &matrix, &pool, &gate, &first) ==
                  HFOC_LWIP_TCP_ISOLATION_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &first) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &first) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "invalid/stale fixture releases first generation");

    stale = first;
    CHECK(reserve_token(&pool, &reused) &&
              publish_token(&pool, &reused) &&
              attach_quarantine(&gate, &pool, &reused) &&
              attach_matrix(&matrix, &pool, &gate, &reused),
          "invalid/stale fixture reuses advanced generation");
    CHECK(hfoc_lwip_tcp_isolation_inject(
              &matrix, &pool, &gate, &stale,
              HFOC_LWIP_TCP_ISOLATION_EVENT_RESET) ==
              HFOC_LWIP_TCP_ISOLATION_STALE_TOKEN &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_isolation_closed_count(&matrix) == 0U,
          "stale token cannot close a reused connection");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &reused) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_isolation_detach(
                  &matrix, &pool, &gate, &reused) ==
                  HFOC_LWIP_TCP_ISOLATION_OK &&
              hfoc_lwip_tcp_quarantine_detach(
                  &gate, &reused) ==
                  HFOC_LWIP_TCP_QUARANTINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &reused) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reused generation is released cleanly");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "isolation fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_attach_validation();
    test_five_event_isolation();
    test_invalid_event_and_stale_reuse();

    if (failures != 0) {
        fprintf(stderr, "%d isolation fixture failure(s)\n",
                failures);
        return 1;
    }
    printf("lwIP malformed TCP isolation matrix host fixture: OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
