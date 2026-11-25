#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_pool.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_DEADLINE_HOST_TEST

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

static bool init_scheduler(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler)
{
    memset(scheduler, 0, sizeof(*scheduler));
    return hfoc_lwip_tcp_deadline_init(scheduler) ==
        HFOC_LWIP_TCP_DEADLINE_OK;
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

static struct hfoc_lwip_tcp_deadline_policy make_policy(void)
{
    struct hfoc_lwip_tcp_deadline_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_LWIP_TCP_DEADLINE_VERSION;
    policy.accept_timeout_ms = 100U;
    policy.progress_timeout_ms = 200U;
    policy.idle_timeout_ms = 300U;
    policy.close_timeout_ms = 400U;
    return policy;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_deadline_kind_string(
              HFOC_LWIP_TCP_DEADLINE_KIND_NONE) != NULL &&
              hfoc_lwip_tcp_deadline_kind_string(
                  HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT) != NULL &&
              hfoc_lwip_tcp_deadline_kind_string(
                  HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS) != NULL &&
              hfoc_lwip_tcp_deadline_kind_string(
                  HFOC_LWIP_TCP_DEADLINE_KIND_IDLE) != NULL &&
              hfoc_lwip_tcp_deadline_kind_string(
                  HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE) != NULL &&
              hfoc_lwip_tcp_deadline_kind_string(
                  (enum hfoc_lwip_tcp_deadline_kind)99U) == NULL,
          "deadline kind strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_deadline_status_string(
              HFOC_LWIP_TCP_DEADLINE_OK) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_NULL) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_CLOSED) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  HFOC_LWIP_TCP_DEADLINE_EXPIRED) != NULL &&
              hfoc_lwip_tcp_deadline_status_string(
                  (enum hfoc_lwip_tcp_deadline_status)99U) == NULL,
          "deadline status strings cover known and unknown values");
}

static void test_init_and_attach_validation(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_pool_token invalid;
    struct hfoc_lwip_tcp_deadline_snapshot snapshot;
    struct hfoc_lwip_tcp_pool pool;

    memset(&scheduler, 0, sizeof(scheduler));
    CHECK(hfoc_lwip_tcp_deadline_init(NULL) ==
              HFOC_LWIP_TCP_DEADLINE_NULL,
          "deadline init rejects null scheduler");
    CHECK(!hfoc_lwip_tcp_deadline_published(&scheduler) &&
              hfoc_lwip_tcp_deadline_active_count(&scheduler) == 0U &&
              hfoc_lwip_tcp_deadline_next_deadline_ms(
                  &scheduler, 1000U) ==
                  HFOC_LWIP_TCP_DEADLINE_NEXT_NONE,
          "unpublished deadline scheduler is fail-closed");

    CHECK(init_scheduler(&scheduler),
          "deadline scheduler publishes fixed state");
    CHECK(hfoc_lwip_tcp_deadline_init(&scheduler) ==
              HFOC_LWIP_TCP_DEADLINE_BAD_STATE,
          "published deadline scheduler rejects double init");
    CHECK(hfoc_lwip_tcp_deadline_published(&scheduler) &&
              scheduler.version == HFOC_LWIP_TCP_DEADLINE_VERSION &&
              scheduler.active_count == 0U &&
              hfoc_lwip_tcp_deadline_active_count(&scheduler) == 0U,
          "published deadline scheduler exposes exact empty state");

    invalid.index = HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT;
    invalid.generation = 1U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &invalid, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN,
          "attach rejects invalid slot index");
    invalid.index = 0U;
    invalid.generation = 0U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &invalid, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN,
          "attach rejects invalid generation");

    policy = make_policy();
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, NULL, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_NULL,
          "attach rejects null token");

    CHECK(init_pool(&pool) && reserve_token(&pool, &token),
          "validation fixture reserves a pool token");
    policy.version = 0U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY,
          "attach rejects invalid policy version");
    policy = make_policy();
    policy.accept_timeout_ms = 0U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY,
          "attach rejects zero accept timeout");
    policy = make_policy();
    policy.progress_timeout_ms =
        HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX + 1U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY,
          "attach rejects over-maximum progress timeout");
    policy = make_policy();
    snapshot = hfoc_lwip_tcp_deadline_snapshot(&scheduler, 1000U);
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.active_count == 1U &&
              scheduler.entries[token.index].active &&
              scheduler.entries[token.index].generation ==
                  token.generation,
          "attach publishes one generation-tagged deadline entry");
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED,
          "attach rejects double attach");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) == HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.active_count == 0U,
          "cancel removes attached entry");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) ==
              HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED,
          "second cancel reports not attached");
    (void)snapshot;
    CHECK(hfoc_lwip_tcp_pool_release(&pool, &token) ==
              HFOC_LWIP_TCP_POOL_OK,
          "validation fixture releases pool token");
}

static void test_attach_check_and_touch(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token token;
    enum hfoc_lwip_tcp_deadline_kind kind;
    struct hfoc_lwip_tcp_pool pool;

    CHECK(init_scheduler(&scheduler) &&
              init_pool(&pool) && reserve_token(&pool, &token),
          "check fixture initializes scheduler and token");
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.entries[token.index].accept_deadline_ms == 1100U &&
              scheduler.entries[token.index].progress_deadline_ms == 1200U &&
              scheduler.entries[token.index].idle_deadline_ms == 1300U,
          "attach computes exact accept/progress/idle deadlines");

    kind = HFOC_LWIP_TCP_DEADLINE_KIND_NONE;
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &token, 1099U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE,
          "check before accept deadline reports no expiry");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &token, 1100U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_EXPIRED &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT,
          "check at accept deadline reports accept expiry");

    CHECK(hfoc_lwip_tcp_deadline_touch(
              &scheduler, &token, 1200U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.entries[token.index].accept_deadline_ms ==
                  HFOC_LWIP_TCP_DEADLINE_DISABLED &&
              scheduler.entries[token.index].progress_deadline_ms == 1400U &&
              scheduler.entries[token.index].idle_deadline_ms == 1500U,
          "touch disables accept and extends progress/idle deadlines");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &token, 1399U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_deadline_check(
                  &scheduler, &token, 1400U, &kind) ==
                  HFOC_LWIP_TCP_DEADLINE_EXPIRED &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS,
          "extended progress deadline expires exactly at deadline");

    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) == HFOC_LWIP_TCP_DEADLINE_OK,
          "accept/touch fixture cancels first deadline");

    policy.accept_timeout_ms = 100U;
    policy.progress_timeout_ms = 1000U;
    policy.idle_timeout_ms = 200U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 2000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_deadline_touch(
                  &scheduler, &token, 2100U) ==
                  HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_deadline_check(
                  &scheduler, &token, 2299U, &kind) ==
                  HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_deadline_check(
                  &scheduler, &token, 2300U, &kind) ==
                  HFOC_LWIP_TCP_DEADLINE_EXPIRED &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_IDLE,
          "idle deadline expires independently of progress deadline");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) == HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "check fixture releases deadline and pool state");
}

static void test_begin_close_rejects_late_events(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_deadline_scheduler snapshot;
    enum hfoc_lwip_tcp_deadline_kind kind;
    struct hfoc_lwip_tcp_pool pool;

    CHECK(init_scheduler(&scheduler) &&
              init_pool(&pool) && reserve_token(&pool, &token),
          "close fixture initializes scheduler and token");
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK,
          "close fixture attaches deadline");
    CHECK(hfoc_lwip_tcp_deadline_begin_close(
              &scheduler, &token, 1500U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.entries[token.index].closing &&
              scheduler.entries[token.index].close_deadline_ms == 1900U &&
              scheduler.entries[token.index].accept_deadline_ms ==
                  HFOC_LWIP_TCP_DEADLINE_DISABLED &&
              scheduler.entries[token.index].progress_deadline_ms ==
                  HFOC_LWIP_TCP_DEADLINE_DISABLED &&
              scheduler.entries[token.index].idle_deadline_ms ==
                  HFOC_LWIP_TCP_DEADLINE_DISABLED,
          "begin-close arms close deadline and disables activity deadlines");

    memcpy(&snapshot, &scheduler, sizeof(scheduler));
    CHECK(hfoc_lwip_tcp_deadline_touch(
              &scheduler, &token, 1899U) ==
              HFOC_LWIP_TCP_DEADLINE_CLOSED &&
              memcmp(&scheduler, &snapshot, sizeof(scheduler)) == 0,
          "late touch after begin-close is rejected without extension");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &token, 1899U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE,
          "close check before deadline reports no expiry");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &token, 1900U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_EXPIRED &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE,
          "close deadline expires exactly at deadline");
    CHECK(hfoc_lwip_tcp_deadline_touch(
              &scheduler, &token, 1901U) ==
              HFOC_LWIP_TCP_DEADLINE_CLOSED &&
              memcmp(&scheduler, &snapshot, sizeof(scheduler)) == 0,
          "touch after close expiry cannot extend closed generation");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) == HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "close fixture releases deadline and pool state");
}

static void test_stale_token_and_reuse(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token first;
    struct hfoc_lwip_tcp_pool_token second;
    struct hfoc_lwip_tcp_deadline_scheduler snapshot;
    enum hfoc_lwip_tcp_deadline_kind kind;
    struct hfoc_lwip_tcp_pool pool;

    CHECK(init_scheduler(&scheduler) &&
              init_pool(&pool) && reserve_token(&pool, &first),
          "reuse fixture reserves first generation");
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &first, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_deadline_cancel(
                  &scheduler, &first) ==
                  HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &first) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reuse fixture releases first generation");
    CHECK(reserve_token(&pool, &second) &&
              second.index == first.index &&
              second.generation == first.generation + 1U,
          "pool reuse returns incremented generation");
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &second, &policy, 2000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK,
          "reuse fixture attaches second generation");

    memcpy(&snapshot, &scheduler, sizeof(scheduler));
    CHECK(hfoc_lwip_tcp_deadline_touch(
              &scheduler, &first, 2500U) ==
              HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN &&
              memcmp(&scheduler, &snapshot, sizeof(scheduler)) == 0,
          "stale touch cannot revive or extend reused generation");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &first, 2500U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN &&
              memcmp(&scheduler, &snapshot, sizeof(scheduler)) == 0,
          "stale check cannot mutate reused generation");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &first) ==
              HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN &&
              memcmp(&scheduler, &snapshot, sizeof(scheduler)) == 0,
          "stale cancel cannot remove reused generation");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &second, 2099U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_OK,
          "current generation remains live after stale attempts");
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &second) == HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &second) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "reuse fixture releases second generation");
}

static void test_wraparound_and_next_deadline(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token tokens[
        HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT];
    struct hfoc_lwip_tcp_pool_token stale;
    enum hfoc_lwip_tcp_deadline_kind kind;
    struct hfoc_lwip_tcp_pool pool;
    uint32_t index;

    CHECK(init_scheduler(&scheduler) && init_pool(&pool),
          "wraparound fixture initializes scheduler and pool");
    for (index = 0U; index < HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT;
         ++index) {
        CHECK(reserve_token(&pool, &tokens[index]),
              "wraparound fixture reserves four tokens");
    }
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &tokens[0], &policy, UINT32_MAX - 50U) ==
              HFOC_LWIP_TCP_DEADLINE_OK &&
              scheduler.entries[tokens[0].index].accept_deadline_ms == 49U &&
              scheduler.entries[tokens[0].index].progress_deadline_ms ==
                  149U &&
              scheduler.entries[tokens[0].index].idle_deadline_ms == 249U,
          "wrapped deadline arithmetic is exact");
    CHECK(hfoc_lwip_tcp_deadline_next_deadline_ms(
              &scheduler, UINT32_MAX - 10U) == 49U,
          "next deadline selects wrapped accept deadline");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &tokens[0], 48U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_OK,
          "wrapped check before deadline is not expired");
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &tokens[0], 49U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_EXPIRED &&
              kind == HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT,
          "wrapped check at deadline reports accept expiry");

    stale = tokens[0];
    stale.generation = HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT + 1U;
    CHECK(hfoc_lwip_tcp_deadline_check(
              &scheduler, &stale, 100U, &kind) ==
              HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN,
          "check rejects bogus generation token");

    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &tokens[0]) == HFOC_LWIP_TCP_DEADLINE_OK,
          "wraparound fixture cancels first deadline");
    for (index = 0U; index < HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_release(&pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK,
              "wraparound fixture releases pool tokens");
    }
}

static void test_snapshot_and_failure_no_mutation(void)
{
    struct hfoc_lwip_tcp_deadline_scheduler scheduler;
    struct hfoc_lwip_tcp_deadline_policy policy = make_policy();
    struct hfoc_lwip_tcp_pool_token token;
    struct hfoc_lwip_tcp_deadline_snapshot snapshot;
    struct hfoc_lwip_tcp_deadline_scheduler before;
    struct hfoc_lwip_tcp_pool pool;

    CHECK(init_scheduler(&scheduler) &&
              init_pool(&pool) && reserve_token(&pool, &token) &&
              hfoc_lwip_tcp_deadline_attach(
                  &scheduler, &token, &policy, 1000U) ==
                  HFOC_LWIP_TCP_DEADLINE_OK,
          "snapshot fixture attaches one deadline");
    snapshot = hfoc_lwip_tcp_deadline_snapshot(&scheduler, 1000U);
    CHECK(snapshot.version == HFOC_LWIP_TCP_DEADLINE_VERSION &&
              snapshot.active_count == 1U &&
              snapshot.next_deadline_ms == 1100U,
          "snapshot exposes bounded active state");

    memcpy(&before, &scheduler, sizeof(scheduler));
    policy.idle_timeout_ms = 0U;
    CHECK(hfoc_lwip_tcp_deadline_attach(
              &scheduler, &token, &policy, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY &&
              memcmp(&scheduler, &before, sizeof(scheduler)) == 0,
          "failed attach does not mutate scheduler");
    CHECK(hfoc_lwip_tcp_deadline_begin_close(
              &scheduler, &token, 1000U) ==
              HFOC_LWIP_TCP_DEADLINE_OK,
          "snapshot fixture begins close");
    policy.close_timeout_ms = 0U;
    CHECK(hfoc_lwip_tcp_deadline_cancel(
              &scheduler, &token) == HFOC_LWIP_TCP_DEADLINE_OK &&
              hfoc_lwip_tcp_pool_release(&pool, &token) ==
                  HFOC_LWIP_TCP_POOL_OK,
          "snapshot fixture releases state");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "deadline fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_attach_validation();
    test_attach_check_and_touch();
    test_begin_close_rejects_late_events();
    test_stale_token_and_reuse();
    test_wraparound_and_next_deadline();
    test_snapshot_and_failure_no_mutation();

    if (failures != 0) {
        fprintf(stderr, "lwIP connection deadline fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP connection deadline scheduler fixture: OK\n");
    return 0;
}

#endif
