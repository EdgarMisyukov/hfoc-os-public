#include "https_peer_throttle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HTTPS_PEER_THROTTLE_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct https_peer_throttle_table make_table(
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t window_ticks,
    uint32_t cooldown_ticks)
{
    struct https_peer_throttle_table table;

    memset(&table, 0xA5, sizeof(table));
    CHECK(https_peer_throttle_init(
              &table, capacity, failure_limit, window_ticks,
              cooldown_ticks) == HTTPS_PEER_THROTTLE_OK,
          "peer-throttle fixture initializes");
    return table;
}

static void capture_table(
    const struct https_peer_throttle_table *table,
    unsigned char *original)
{
    memcpy(original, table, sizeof(*table));
}

static void check_table_unchanged(
    const struct https_peer_throttle_table *table,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(table, original, sizeof(*table)) == 0, message);
}

static void test_status_strings(void)
{
    static const char *strings[] = {
        "HTTPS_PEER_THROTTLE_OK",
        "HTTPS_PEER_THROTTLE_NULL",
        "HTTPS_PEER_THROTTLE_BAD_CAPACITY",
        "HTTPS_PEER_THROTTLE_INVALID_VERSION",
        "HTTPS_PEER_THROTTLE_INVALID_PEER_ID",
        "HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT",
        "HTTPS_PEER_THROTTLE_INVALID_WINDOW",
        "HTTPS_PEER_THROTTLE_INVALID_COOLDOWN",
        "HTTPS_PEER_THROTTLE_INVALID_TICK",
        "HTTPS_PEER_THROTTLE_PEER_BLOCKED",
        "HTTPS_PEER_THROTTLE_PEER_NOT_FOUND",
        "HTTPS_PEER_THROTTLE_INVALID_STATE",
        "HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(strings) / sizeof(strings[0]);
         ++index) {
        const char *string = https_peer_throttle_status_string(
            (enum https_peer_throttle_status)index);

        CHECK(string != NULL &&
                  strcmp(string, strings[index]) == 0,
              "peer-throttle status string is exact");
    }
    CHECK(https_peer_throttle_status_string(
              (enum https_peer_throttle_status)99U) == NULL,
          "peer-throttle undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle status handling performs no allocation");
}

static void test_init(void)
{
    struct https_peer_throttle_table table;
    uint32_t index;

    CHECK(https_peer_throttle_init(NULL, 1U, 1U, 1U, 1U) ==
              HTTPS_PEER_THROTTLE_NULL,
          "peer-throttle init rejects null table");
    CHECK(https_peer_throttle_init(&table, 0U, 1U, 1U, 1U) ==
              HTTPS_PEER_THROTTLE_BAD_CAPACITY,
          "peer-throttle zero capacity is rejected");
    CHECK(https_peer_throttle_init(
              &table,
              HTTPS_PEER_THROTTLE_MAX_PEERS + 1U,
              1U,
              1U,
              1U) == HTTPS_PEER_THROTTLE_BAD_CAPACITY,
          "peer-throttle over-limit capacity is rejected");
    CHECK(https_peer_throttle_init(&table, 1U, 0U, 1U, 1U) ==
              HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT,
          "peer-throttle zero failure limit is rejected");
    CHECK(https_peer_throttle_init(
              &table,
              1U,
              HTTPS_PEER_THROTTLE_FAILURE_LIMIT_MAX + 1U,
              1U,
              1U) == HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT,
          "peer-throttle over-limit failure limit is rejected");
    CHECK(https_peer_throttle_init(&table, 1U, 1U, 0U, 1U) ==
              HTTPS_PEER_THROTTLE_INVALID_WINDOW,
          "peer-throttle zero window is rejected");
    CHECK(https_peer_throttle_init(
              &table,
              1U,
              1U,
              HTTPS_PEER_THROTTLE_WINDOW_MAX + 1U,
              1U) == HTTPS_PEER_THROTTLE_INVALID_WINDOW,
          "peer-throttle over-limit window is rejected");
    CHECK(https_peer_throttle_init(&table, 1U, 1U, 1U, 0U) ==
              HTTPS_PEER_THROTTLE_INVALID_COOLDOWN,
          "peer-throttle zero cooldown is rejected");
    CHECK(https_peer_throttle_init(
              &table,
              1U,
              1U,
              1U,
              HTTPS_PEER_THROTTLE_COOLDOWN_MAX + 1U) ==
              HTTPS_PEER_THROTTLE_INVALID_COOLDOWN,
          "peer-throttle over-limit cooldown is rejected");

    CHECK(https_peer_throttle_init(&table, 2U, 3U, 100U, 50U) ==
              HTTPS_PEER_THROTTLE_OK &&
              table.version == HTTPS_PEER_THROTTLE_VERSION &&
              table.capacity == 2U &&
              table.failure_limit == 3U &&
              table.window_ticks == 100U &&
              table.cooldown_ticks == 50U &&
              table.count == 0U,
          "peer-throttle init publishes exact configuration");
    for (index = 0U;
         index < HTTPS_PEER_THROTTLE_MAX_PEERS;
         ++index) {
        CHECK(table.buckets[index].version ==
                      HTTPS_PEER_THROTTLE_VERSION &&
                  table.buckets[index].peer_id == 0U &&
                  table.buckets[index].failure_count == 0U &&
                  table.buckets[index].first_failure_tick == 0U &&
                  table.buckets[index].last_failure_tick == 0U &&
                  table.buckets[index].blocked_until_tick == 0U &&
                  table.buckets[index].state ==
                      HTTPS_PEER_THROTTLE_BUCKET_FREE,
              "peer-throttle init initializes every bucket exactly");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle init performs no allocation");
}

static void test_validation_no_mutation(void)
{
    struct https_peer_throttle_table table =
        make_table(2U, 3U, 100U, 50U);
    struct https_peer_throttle_table bad_table;
    unsigned char original[sizeof(table)];
    uint64_t out = 0xDEADBEEFDEADBEEFULL;

    capture_table(&table, original);
    CHECK(https_peer_throttle_record_failure(
              NULL, 1U, 0U, &out) == HTTPS_PEER_THROTTLE_NULL &&
              out == 0xDEADBEEFDEADBEEFULL,
          "peer-throttle record rejects null table without touching output");
    CHECK(https_peer_throttle_record_failure(
              &table, 1U, 0U, NULL) == HTTPS_PEER_THROTTLE_NULL,
          "peer-throttle record rejects null output");

    bad_table = table;
    bad_table.capacity = 0U;
    CHECK(https_peer_throttle_record_failure(
              &bad_table, 1U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_BAD_CAPACITY,
          "peer-throttle record rejects zero capacity");
    bad_table = table;
    bad_table.version = HTTPS_PEER_THROTTLE_VERSION + 1U;
    CHECK(https_peer_throttle_record_failure(
              &bad_table, 1U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_INVALID_VERSION,
          "peer-throttle record rejects unknown version");
    bad_table = table;
    bad_table.failure_limit = 0U;
    CHECK(https_peer_throttle_record_failure(
              &bad_table, 1U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_INVALID_FAILURE_LIMIT,
          "peer-throttle record rejects invalid failure limit");
    bad_table = table;
    bad_table.window_ticks = 0U;
    CHECK(https_peer_throttle_record_failure(
              &bad_table, 1U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_INVALID_WINDOW,
          "peer-throttle record rejects invalid window");
    bad_table = table;
    bad_table.cooldown_ticks = 0U;
    CHECK(https_peer_throttle_record_failure(
              &bad_table, 1U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_INVALID_COOLDOWN,
          "peer-throttle record rejects invalid cooldown");

    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(https_peer_throttle_record_failure(
              &table, 0U, 0U, &out) ==
              HTTPS_PEER_THROTTLE_INVALID_PEER_ID &&
              out == 0xDEADBEEFDEADBEEFULL,
          "peer-throttle record rejects zero peer id");
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(https_peer_throttle_record_failure(
              &table,
              HTTPS_PEER_THROTTLE_PEER_ID_MAX + 1U,
              0U,
              &out) == HTTPS_PEER_THROTTLE_INVALID_PEER_ID &&
              out == 0xDEADBEEFDEADBEEFULL,
          "peer-throttle record rejects over-limit peer id");
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(https_peer_throttle_record_failure(
              &table,
              1U,
              UINT64_MAX - (uint64_t)table.cooldown_ticks + 1U,
              &out) == HTTPS_PEER_THROTTLE_INVALID_TICK &&
              out == 0xDEADBEEFDEADBEEFULL,
          "peer-throttle record rejects unrepresentable tick");
    check_table_unchanged(
        &table, original,
        "peer-throttle validation failures leave table unchanged");

    CHECK(https_peer_throttle_check(NULL, 1U, 0U) ==
              HTTPS_PEER_THROTTLE_NULL,
          "peer-throttle check rejects null table");
    bad_table = table;
    bad_table.version = HTTPS_PEER_THROTTLE_VERSION + 1U;
    CHECK(https_peer_throttle_check(&bad_table, 1U, 0U) ==
              HTTPS_PEER_THROTTLE_INVALID_VERSION,
          "peer-throttle check rejects unknown version");
    CHECK(https_peer_throttle_check(&table, 0U, 0U) ==
              HTTPS_PEER_THROTTLE_INVALID_PEER_ID,
          "peer-throttle check rejects zero peer id");

    CHECK(https_peer_throttle_reset(NULL, 1U) ==
              HTTPS_PEER_THROTTLE_NULL,
          "peer-throttle reset rejects null table");
    bad_table = table;
    bad_table.version = HTTPS_PEER_THROTTLE_VERSION + 1U;
    CHECK(https_peer_throttle_reset(&bad_table, 1U) ==
              HTTPS_PEER_THROTTLE_INVALID_VERSION,
          "peer-throttle reset rejects unknown version");
    CHECK(https_peer_throttle_reset(&table, 0U) ==
              HTTPS_PEER_THROTTLE_INVALID_PEER_ID,
          "peer-throttle reset rejects zero peer id");
    check_table_unchanged(
        &table, original,
        "peer-throttle check and reset failures leave table unchanged");

    CHECK(https_peer_throttle_local_allowed(
              (enum https_peer_throttle_local_operation)0U) ==
              HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION,
          "peer-throttle local guard rejects invalid operation");
    CHECK(https_peer_throttle_local_allowed(
              (enum https_peer_throttle_local_operation)99U) ==
              HTTPS_PEER_THROTTLE_INVALID_LOCAL_OPERATION,
          "peer-throttle local guard rejects undefined operation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle validation performs no allocation");
}

static void test_failure_window_and_block(void)
{
    struct https_peer_throttle_table table =
        make_table(4U, 3U, 100U, 50U);
    unsigned char original[sizeof(table)];
    uint64_t out = 0U;

    CHECK(https_peer_throttle_record_failure(
              &table, 7U, 1000U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.count == 1U &&
              table.buckets[0].peer_id == 7U &&
              table.buckets[0].failure_count == 1U &&
              table.buckets[0].first_failure_tick == 1000U &&
              table.buckets[0].last_failure_tick == 1000U &&
              table.buckets[0].state ==
                  HTTPS_PEER_THROTTLE_BUCKET_ACTIVE,
          "peer-throttle first failure publishes one active bucket");
    CHECK(https_peer_throttle_record_failure(
              &table, 7U, 1010U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.buckets[0].failure_count == 2U &&
              table.buckets[0].last_failure_tick == 1010U,
          "peer-throttle second failure increments the same bucket");
    CHECK(https_peer_throttle_record_failure(
              &table, 7U, 1020U, &out) ==
              HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 1070U &&
              table.buckets[0].failure_count == 3U &&
              table.buckets[0].blocked_until_tick == 1070U &&
              table.buckets[0].state ==
                  HTTPS_PEER_THROTTLE_BUCKET_BLOCKED,
          "peer-throttle third failure blocks with exact cooldown");

    CHECK(https_peer_throttle_check(&table, 7U, 1069U) ==
              HTTPS_PEER_THROTTLE_PEER_BLOCKED,
          "peer-throttle rejects blocked peer before expiry");
    CHECK(https_peer_throttle_check(&table, 7U, 1070U) ==
              HTTPS_PEER_THROTTLE_OK,
          "blocked peer is allowed at exact expiry");

    capture_table(&table, original);
    out = 0U;
    CHECK(https_peer_throttle_record_failure(
              &table, 7U, 1030U, &out) ==
              HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 1070U,
          "peer-throttle repeated blocked failure reports exact deadline");
    check_table_unchanged(
        &table, original,
        "repeated blocked failures do not extend cooldown");

    CHECK(https_peer_throttle_record_failure(
              &table, 7U, 1070U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.buckets[0].failure_count == 1U &&
              table.buckets[0].first_failure_tick == 1070U &&
              table.buckets[0].last_failure_tick == 1070U &&
              table.buckets[0].blocked_until_tick == 0U &&
              table.buckets[0].state ==
                  HTTPS_PEER_THROTTLE_BUCKET_ACTIVE,
          "peer-throttle failure after expiry starts a fresh bucket");

    CHECK(https_peer_throttle_record_failure(
              &table, 8U, 2000U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.buckets[1].failure_count == 1U &&
              table.buckets[1].first_failure_tick == 2000U,
          "peer-throttle separate peer starts a separate bucket");
    CHECK(https_peer_throttle_record_failure(
              &table, 8U, 2100U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.buckets[1].failure_count == 2U &&
              table.buckets[1].first_failure_tick == 2000U,
          "peer-throttle failure at window end stays in the same window");
    CHECK(https_peer_throttle_record_failure(
              &table, 8U, 2101U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.buckets[1].failure_count == 1U &&
              table.buckets[1].first_failure_tick == 2101U &&
              table.buckets[1].last_failure_tick == 2101U,
          "peer-throttle expired window starts a fresh failure bucket");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle window handling performs no allocation");
}

static void test_unrelated_peer_and_local(void)
{
    struct https_peer_throttle_table table =
        make_table(4U, 3U, 100U, 50U);
    uint64_t out = 0U;

    CHECK(https_peer_throttle_record_failure(
              &table, 9U, 100U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 9U, 110U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 9U, 120U, &out) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 170U,
          "peer-throttle abusive peer reaches blocked state");

    CHECK(https_peer_throttle_check(&table, 42U, 130U) ==
              HTTPS_PEER_THROTTLE_OK,
          "one abusive peer cannot block unrelated peers");
    CHECK(https_peer_throttle_record_failure(
              &table, 42U, 130U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_check(&table, 42U, 131U) ==
                  HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_check(&table, 9U, 131U) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED,
          "unrelated peer failures are tracked independently");

    CHECK(https_peer_throttle_local_allowed(
              HTTPS_PEER_THROTTLE_LOCAL_MANAGEMENT) ==
              HTTPS_PEER_THROTTLE_OK,
          "local management remains allowed while peer is blocked");
    CHECK(https_peer_throttle_local_allowed(
              HTTPS_PEER_THROTTLE_LOCAL_HEALTH) ==
              HTTPS_PEER_THROTTLE_OK,
          "local health remains allowed while peer is blocked");
    CHECK(https_peer_throttle_local_allowed(
              HTTPS_PEER_THROTTLE_LOCAL_SUPERVISION) ==
              HTTPS_PEER_THROTTLE_OK,
          "local supervision remains allowed while peer is blocked");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle isolation handling performs no allocation");
}

static void test_no_permanent_blacklist_and_reset(void)
{
    struct https_peer_throttle_table table =
        make_table(4U, 3U, 100U, 50U);
    uint64_t out = 0U;
    uint32_t index;

    CHECK(https_peer_throttle_record_failure(
              &table, 5U, 1000U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 5U, 1010U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 5U, 1020U, &out) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 1070U,
          "peer-throttle reset fixture blocks one peer");
    CHECK(https_peer_throttle_check(&table, 5U, 1070U) ==
              HTTPS_PEER_THROTTLE_OK,
          "peer-throttle cooldown expiry clears the blacklist");
    CHECK(https_peer_throttle_record_failure(
              &table, 5U, 1070U, &out) == HTTPS_PEER_THROTTLE_OK &&
              table.buckets[0].failure_count == 1U &&
              table.buckets[0].first_failure_tick == 1070U &&
              table.buckets[0].blocked_until_tick == 0U,
          "peer-throttle post-expiry record is not permanently blacklisted");
    CHECK(https_peer_throttle_reset(&table, 5U) ==
              HTTPS_PEER_THROTTLE_OK,
          "peer-throttle reset clears the post-expiry bucket");

    CHECK(https_peer_throttle_record_failure(
              &table, 1U, 3000U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 2U, 3001U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 3U, 3002U, &out) == HTTPS_PEER_THROTTLE_OK &&
              table.count == 3U,
          "peer-throttle reset setup holds three compact buckets");
    CHECK(https_peer_throttle_reset(&table, 2U) ==
              HTTPS_PEER_THROTTLE_OK &&
              table.count == 2U &&
              table.buckets[0].peer_id == 1U &&
              table.buckets[1].peer_id == 3U,
          "peer-throttle reset removes and compacts the exact peer");
    for (index = table.count;
         index < HTTPS_PEER_THROTTLE_MAX_PEERS;
         ++index) {
        CHECK(table.buckets[index].state ==
                      HTTPS_PEER_THROTTLE_BUCKET_FREE &&
                  table.buckets[index].peer_id == 0U &&
                  table.buckets[index].failure_count == 0U,
              "peer-throttle reset restores free buckets outside count");
    }
    CHECK(https_peer_throttle_check(&table, 2U, 3003U) ==
              HTTPS_PEER_THROTTLE_OK,
          "peer-throttle reset makes the peer unknown");
    CHECK(https_peer_throttle_reset(&table, 99U) ==
              HTTPS_PEER_THROTTLE_PEER_NOT_FOUND,
          "peer-throttle reset rejects unknown peer");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle reset performs no allocation");
}

static void test_table_replacement(void)
{
    struct https_peer_throttle_table table =
        make_table(2U, 3U, 1000U, 100U);
    uint64_t out = 0U;

    CHECK(https_peer_throttle_record_failure(
              &table, 10U, 100U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 10U, 200U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 10U, 300U, &out) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 400U,
          "peer-throttle replacement fixture blocks first peer");
    CHECK(https_peer_throttle_record_failure(
              &table, 11U, 500U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 11U, 600U, &out) == HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_record_failure(
                  &table, 11U, 700U, &out) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              out == 800U &&
              table.count == 2U,
          "peer-throttle replacement fixture fills capacity");
    CHECK(https_peer_throttle_record_failure(
              &table, 12U, 900U, &out) == HTTPS_PEER_THROTTLE_OK &&
              out == 0U &&
              table.count == 2U &&
              table.buckets[0].peer_id == 11U &&
              table.buckets[1].peer_id == 12U,
          "full table admits a new peer by replacing oldest bucket");
    CHECK(https_peer_throttle_check(&table, 10U, 750U) ==
              HTTPS_PEER_THROTTLE_OK &&
              https_peer_throttle_check(&table, 11U, 750U) ==
                  HTTPS_PEER_THROTTLE_PEER_BLOCKED &&
              https_peer_throttle_check(&table, 12U, 750U) ==
                  HTTPS_PEER_THROTTLE_OK,
          "peer-throttle replacement preserves the newer blocked peer");
    CHECK(https_peer_throttle_record_failure(
              &table, 13U, 950U, &out) == HTTPS_PEER_THROTTLE_OK &&
              table.count == 2U &&
              table.buckets[0].peer_id == 12U &&
              table.buckets[1].peer_id == 13U,
          "peer-throttle bounded table keeps admitting unrelated peers");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "peer-throttle replacement performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init();
    test_validation_no_mutation();
    test_failure_window_and_block();
    test_unrelated_peer_and_local();
    test_no_permanent_blacklist_and_reset();
    test_table_replacement();

    if (failures != 0) {
        fprintf(stderr,
                "HTTPS peer-failure throttle host test: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("HTTPS peer-failure throttle host test: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
