#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_POOL_HOST_TEST

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

static err_t pool_event(
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
    callbacks.event = pool_event;
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

static bool publish_token(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct tcp_pcb **pcb_out)
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
    *pcb_out = pcb;
    return true;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_pool_state_string(
              HFOC_LWIP_TCP_POOL_STATE_FREE) != NULL &&
              hfoc_lwip_tcp_pool_state_string(
                  HFOC_LWIP_TCP_POOL_STATE_RESERVED) != NULL &&
              hfoc_lwip_tcp_pool_state_string(
                  HFOC_LWIP_TCP_POOL_STATE_PUBLISHED) != NULL &&
              hfoc_lwip_tcp_pool_state_string(
                  (enum hfoc_lwip_tcp_pool_state)99U) == NULL,
          "pool state strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_pool_status_string(
              HFOC_LWIP_TCP_POOL_OK) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_NULL) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_EXHAUSTED) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_BAD_INDEX) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_WRONG_STATE) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_DOUBLE_FREE) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_CONN_FAILED) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_CLOSE_FAILED) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_ALREADY_CLOSED) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  HFOC_LWIP_TCP_POOL_LIVE_CONNECTION) != NULL &&
              hfoc_lwip_tcp_pool_status_string(
                  (enum hfoc_lwip_tcp_pool_status)99U) == NULL,
          "pool status strings cover known and unknown values");
}

static void test_init_and_accessors(void)
{
    struct hfoc_lwip_tcp_pool pool;
    uint32_t index;

    memset(&pool, 0, sizeof(pool));
    CHECK(hfoc_lwip_tcp_pool_init(NULL) ==
              HFOC_LWIP_TCP_POOL_NULL,
          "pool init rejects null pool");
    CHECK(!hfoc_lwip_tcp_pool_published(&pool) &&
              hfoc_lwip_tcp_pool_free_count(&pool) == 0U &&
              hfoc_lwip_tcp_pool_reserved_count(&pool) == 0U &&
              hfoc_lwip_tcp_pool_published_count(&pool) == 0U &&
              hfoc_lwip_tcp_pool_generation(&pool, 0U) == 0U &&
              hfoc_lwip_tcp_pool_state(&pool, 0U) ==
                  HFOC_LWIP_TCP_POOL_STATE_FREE &&
              hfoc_lwip_tcp_pool_connection(&pool, 0U) == NULL,
          "unpublished pool accessors report inactive state");
    CHECK(hfoc_lwip_tcp_pool_reserve(
              &pool, &(struct hfoc_lwip_tcp_pool_token){
                  0U, 0U}) == HFOC_LWIP_TCP_POOL_BAD_STATE,
          "unpublished pool rejects reserve");

    CHECK(init_pool(&pool),
          "valid pool config publishes fixed four-slot pool");
    CHECK(hfoc_lwip_tcp_pool_init(&pool) ==
              HFOC_LWIP_TCP_POOL_BAD_STATE,
          "published pool rejects double init");
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(pool.slots[index].state ==
                  HFOC_LWIP_TCP_POOL_STATE_FREE &&
                  pool.slots[index].generation ==
                      HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
                  hfoc_lwip_tcp_pool_generation(&pool, index) ==
                      HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
                  hfoc_lwip_tcp_pool_state(&pool, index) ==
                      HFOC_LWIP_TCP_POOL_STATE_FREE &&
                  hfoc_lwip_tcp_pool_connection(&pool, index) != NULL,
              "published pool seeds free generation-one slots");
    }
    CHECK(hfoc_lwip_tcp_pool_published(&pool) &&
              pool.version == HFOC_LWIP_TCP_POOL_VERSION &&
              pool.free_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              pool.reserved_count == 0U &&
              pool.published_count == 0U &&
              hfoc_lwip_tcp_pool_free_count(&pool) ==
                  HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              hfoc_lwip_tcp_pool_reserved_count(&pool) == 0U &&
              hfoc_lwip_tcp_pool_published_count(&pool) == 0U,
          "published pool exposes exact initial counts");
    CHECK(hfoc_lwip_tcp_pool_generation(&pool, 4U) == 0U &&
              hfoc_lwip_tcp_pool_state(&pool, 4U) ==
                  HFOC_LWIP_TCP_POOL_STATE_FREE &&
              hfoc_lwip_tcp_pool_connection(&pool, 4U) == NULL,
          "pool accessors reject invalid index");
}

static void test_reserve_and_publish_four(void)
{
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool_token tokens[4];
    struct tcp_pcb *pcbs[4];
    uint32_t index;

    CHECK(init_pool(&pool),
          "four-slot fixture initializes pool");
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK &&
                  tokens[index].index == index &&
                  tokens[index].generation ==
                      HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
                  pool.slots[index].state ==
                      HFOC_LWIP_TCP_POOL_STATE_RESERVED &&
                  pool.reserved_count == index + 1U &&
                  pool.free_count ==
                      HFOC_LWIP_TCP_POOL_SLOT_COUNT - index - 1U,
              "reserve returns four generation-tagged tokens");
    }

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(publish_token(&pool, &tokens[index], &pcbs[index]) &&
                  pool.slots[index].state ==
                      HFOC_LWIP_TCP_POOL_STATE_PUBLISHED &&
                  pool.published_count == index + 1U &&
                  pool.reserved_count ==
                      HFOC_LWIP_TCP_POOL_SLOT_COUNT - index - 1U &&
                  hfoc_lwip_tcp_pool_published(&pool) &&
                  hfoc_lwip_tcp_pool_connection(
                      &pool, index) != NULL &&
                  hfoc_lwip_tcp_pool_connection(
                      &pool, index)->published &&
                  hfoc_lwip_tcp_pool_connection(
                      &pool, index)->generation ==
                      tokens[index].generation &&
                  hfoc_lwip_tcp_pool_connection(
                      &pool, index)->accept_count == 1U,
              "publish publishes connection with slot generation");
    }
    CHECK(pool.free_count == 0U &&
              pool.reserved_count == 0U &&
              pool.published_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "four publishes exhaust pool with exact counts");

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_abort(
                  &pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK &&
                  hfoc_lwip_tcp_pool_release(
                      &pool, &tokens[index]) ==
                      HFOC_LWIP_TCP_POOL_OK,
              "published pool fixture aborts and releases slots");
    }
    CHECK(pool.free_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              pool.reserved_count == 0U &&
              pool.published_count == 0U,
          "published pool fixture restores initial free count");
}

static void test_fifth_reservation_preserves_live_slots(void)
{
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool snapshot;
    struct hfoc_lwip_tcp_pool_token tokens[4];
    struct hfoc_lwip_tcp_pool_token fifth;
    struct tcp_pcb *pcbs[4];
    uint32_t index;

    CHECK(init_pool(&pool),
          "fifth-reservation fixture initializes pool");
    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_reserve(
                  &pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK &&
                  publish_token(&pool, &tokens[index],
                                &pcbs[index]),
              "fifth-reservation fixture fills all four slots");
    }

    memset(&fifth, 0, sizeof(fifth));
    fifth.index = 99U;
    fifth.generation = 99U;
    memcpy(&snapshot, &pool, sizeof(pool));
    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &fifth) ==
              HFOC_LWIP_TCP_POOL_EXHAUSTED &&
              fifth.index == 99U &&
              fifth.generation == 99U &&
              memcmp(&pool, &snapshot, sizeof(pool)) == 0 &&
              pool.free_count == 0U &&
              pool.reserved_count == 0U &&
              pool.published_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "fifth reservation fails without disturbing live slots");

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_abort(
                  &pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK &&
                  hfoc_lwip_tcp_pool_release(
                      &pool, &tokens[index]) ==
                      HFOC_LWIP_TCP_POOL_OK,
              "fifth-reservation fixture cleans all slots");
    }
}

static void test_release_live_rejected_and_reuse(void)
{
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool snapshot;
    struct hfoc_lwip_tcp_pool_token token;
    struct tcp_pcb *pcb;

    CHECK(init_pool(&pool),
          "reuse fixture initializes pool");
    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK &&
              publish_token(&pool, &token, &pcb),
          "reuse fixture publishes one slot");
    memcpy(&snapshot, &pool, sizeof(pool));
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_LIVE_CONNECTION &&
              memcmp(&pool, &snapshot, sizeof(pool)) == 0,
          "release rejects a live connection without mutation");

    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK &&
              pool.slots[token.index].state ==
                  HFOC_LWIP_TCP_POOL_STATE_FREE &&
              pool.slots[token.index].generation ==
                  token.generation + 1U &&
              pool.free_count == HFOC_LWIP_TCP_POOL_SLOT_COUNT,
          "abort followed by release enables slot reuse");

    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK &&
              token.generation == 2U &&
              publish_token(&pool, &token, &pcb),
          "reuse reserves incremented generation and republishes");
    pcb->state = CLOSED;
    CHECK(hfoc_lwip_tcp_pool_close(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_connection(
                  &pool, token.index)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK &&
              pool.slots[token.index].generation == 3U,
          "local close followed by release advances generation");
}

static void test_stale_and_wrong_state_rejection(void)
{
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token stale;
    struct hfoc_lwip_tcp_pool_token invalid;

    CHECK(init_pool(&pool),
          "stale-token fixture initializes pool");
    CHECK(hfoc_lwip_tcp_pool_reserve(
              &pool, &(struct hfoc_lwip_tcp_pool_token){
                  0U, 0U}) == HFOC_LWIP_TCP_POOL_OK,
          "stale-token fixture reserves slot");
    invalid.index = HFOC_LWIP_TCP_POOL_SLOT_COUNT;
    invalid.generation = 1U;
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &invalid) ==
              HFOC_LWIP_TCP_POOL_BAD_INDEX,
          "release rejects invalid index");

    token.index = 0U;
    token.generation = 1U;
    CHECK(hfoc_lwip_tcp_pool_abort(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_WRONG_STATE,
          "abort rejects reserved slot");
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK,
          "reserved slot can be released without publish");
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_DOUBLE_FREE,
          "second release of free slot is rejected");

    CHECK(hfoc_lwip_tcp_pool_reserve(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_reserve(&pool, &stale) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "stale-token fixture reserves two slots");
    stale.generation += 1U;
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &stale) ==
              HFOC_LWIP_TCP_POOL_STALE_TOKEN,
          "stale generation token is rejected");
    stale.generation -= 1U;
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &stale) ==
              HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "valid tokens release reserved slots");

    CHECK(hfoc_lwip_tcp_pool_reserve(
              &pool, &token) == HFOC_LWIP_TCP_POOL_OK &&
              hfoc_lwip_tcp_pool_publish(
                  &pool, &token, NULL, NULL) ==
                  HFOC_LWIP_TCP_POOL_NULL &&
              pool.slots[token.index].state ==
                  HFOC_LWIP_TCP_POOL_STATE_RESERVED,
          "publish null PCB leaves reserved state unchanged");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "pool fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_accessors();
    test_reserve_and_publish_four();
    test_fifth_reservation_preserves_live_slots();
    test_release_live_rejected_and_reuse();
    test_stale_and_wrong_state_rejection();

    if (failures != 0) {
        fprintf(stderr, "lwIP four-slot pool fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP four-slot connection pool fixture: OK\n");
    return 0;
}

#endif
