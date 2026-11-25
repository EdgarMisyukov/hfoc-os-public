#include "integration_failure_isolation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_FAILURE_ISOLATION_HOST_TEST

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

static struct integration_failure_isolation_table make_table(
    uint32_t capacity)
{
    struct integration_failure_isolation_table table;

    memset(&table, 0xA5, sizeof(table));
    CHECK(integration_failure_isolation_init(&table, capacity) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "integration failure isolation fixture initializes");
    return table;
}

static void capture_table(
    const struct integration_failure_isolation_table *table,
    unsigned char *original)
{
    memcpy(original, table, sizeof(*table));
}

static void check_table_unchanged(
    const struct integration_failure_isolation_table *table,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(table, original, sizeof(*table)) == 0, message);
}

static void check_no_allocation(void)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration failure isolation performs no allocation");
}

static void isolate_peer(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    enum integration_failure_kind kind,
    uint64_t start_tick)
{
    uint32_t index;

    for (index = 0U; index < INTEGRATION_FAILURE_ISOLATION_THRESHOLD;
         ++index) {
        CHECK(integration_failure_isolation_record(
                  table, peer_id, kind, start_tick + index) ==
                  (index + 1U == INTEGRATION_FAILURE_ISOLATION_THRESHOLD ?
                       INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED :
                       INTEGRATION_FAILURE_ISOLATION_OK),
              "integration failure isolation fixture records fault");
    }
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_FAILURE_ISOLATION_VERSION == 1U,
          "integration failure isolation version is exactly 1");
    CHECK(INTEGRATION_FAILURE_ISOLATION_PEER_ID_MIN == 1U &&
              INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX == 65535U,
          "integration failure isolation peer bounds are exact");
    CHECK(INTEGRATION_FAILURE_ISOLATION_MAX_PEERS == 64U,
          "integration failure isolation maximum peers is exactly 64");
    CHECK(INTEGRATION_FAILURE_ISOLATION_THRESHOLD == 3U,
          "integration failure isolation threshold is exactly 3");
    CHECK(INTEGRATION_FAILURE_ISOLATION_COOLDOWN_MALFORMED == 10U &&
              INTEGRATION_FAILURE_ISOLATION_COOLDOWN_UNAVAILABLE == 20U &&
              INTEGRATION_FAILURE_ISOLATION_COOLDOWN_SLOW == 30U &&
              INTEGRATION_FAILURE_ISOLATION_COOLDOWN_MAX == 30U,
          "integration failure isolation cooldowns are exact");
    CHECK(INTEGRATION_FAILURE_MALFORMED == 1 &&
              INTEGRATION_FAILURE_UNAVAILABLE == 2 &&
              INTEGRATION_FAILURE_SLOW == 3,
          "integration failure kinds are exact");
    CHECK(INTEGRATION_PEER_STATE_HEALTHY == 0 &&
              INTEGRATION_PEER_STATE_DEGRADED == 1 &&
              INTEGRATION_PEER_STATE_ISOLATED == 2,
          "integration peer states are exact");
    CHECK(INTEGRATION_LOCAL_MANAGEMENT == 1 &&
              INTEGRATION_LOCAL_SERVICE_SUPERVISION == 2 &&
              INTEGRATION_LOCAL_STATE_RECOVERY == 3,
          "integration local operations are exact");
    check_no_allocation();
}

static void test_init(void)
{
    struct integration_failure_isolation_table table;
    uint32_t index;

    CHECK(integration_failure_isolation_init(NULL, 1U) ==
              INTEGRATION_FAILURE_ISOLATION_NULL,
          "integration failure isolation init rejects null table");
    CHECK(integration_failure_isolation_init(&table, 0U) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY,
          "integration failure isolation init rejects zero capacity");
    CHECK(integration_failure_isolation_init(
              &table, INTEGRATION_FAILURE_ISOLATION_MAX_PEERS + 1U) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY,
          "integration failure isolation init rejects over-limit capacity");
    CHECK(integration_failure_isolation_init(&table, 3U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              table.version == INTEGRATION_FAILURE_ISOLATION_VERSION &&
              table.capacity == 3U &&
              table.count == 0U,
          "integration failure isolation init publishes exact empty state");
    for (index = 0U;
         index < INTEGRATION_FAILURE_ISOLATION_MAX_PEERS; ++index) {
        CHECK(table.peers[index].peer_id == 0U &&
                  table.peers[index].consecutive_failures == 0U &&
                  table.peers[index].state == INTEGRATION_PEER_STATE_HEALTHY &&
                  table.peers[index].last_failure_tick == 0U &&
                  table.peers[index].isolated_until_tick == 0U,
              "integration failure isolation init zeroes every peer slot");
    }
    check_no_allocation();
}

static void test_validation_failures(void)
{
    struct integration_failure_isolation_table table = make_table(4U);
    struct integration_failure_isolation_table bad_state = table;
    unsigned char original[sizeof(table)];

    capture_table(&table, original);
    CHECK(integration_failure_isolation_record(
              NULL, 7U, INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_NULL,
          "integration failure isolation record rejects null table");
    CHECK(integration_failure_isolation_check(
              NULL, 7U, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_NULL,
          "integration failure isolation check rejects null table");
    CHECK(integration_failure_isolation_recover(NULL, 7U) ==
              INTEGRATION_FAILURE_ISOLATION_NULL,
          "integration failure isolation recover rejects null table");
    check_table_unchanged(&table, original,
                          "null failure isolation failures leave table unchanged");

    CHECK(integration_failure_isolation_record(
              &table, 0U, INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID,
          "integration failure isolation rejects zero peer id");
    CHECK(integration_failure_isolation_record(
              &table,
              INTEGRATION_FAILURE_ISOLATION_PEER_ID_MAX + 1U,
              INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID,
          "integration failure isolation rejects over-limit peer id");
    CHECK(integration_failure_isolation_record(
              &table, 7U,
              (enum integration_failure_kind)0U, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND,
          "integration failure isolation rejects zero fault kind");
    CHECK(integration_failure_isolation_record(
              &table, 7U,
              (enum integration_failure_kind)4U, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND,
          "integration failure isolation rejects unknown fault kind");
    CHECK(integration_failure_isolation_record(
              &table, 7U, INTEGRATION_FAILURE_MALFORMED, UINT64_MAX) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_STATE,
          "integration failure isolation rejects cooldown overflow");
    CHECK(integration_failure_isolation_check(
              &table, 0U, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID,
          "integration failure isolation check rejects invalid peer id");
    CHECK(integration_failure_isolation_recover(
              &table, 0U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID,
          "integration failure isolation recover rejects invalid peer id");
    check_table_unchanged(&table, original,
                          "invalid failure isolation inputs leave table unchanged");

    bad_state.version = 0U;
    CHECK(integration_failure_isolation_record(
              &bad_state, 7U, INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_STATE,
          "integration failure isolation rejects bad version");
    bad_state = table;
    bad_state.capacity = 0U;
    CHECK(integration_failure_isolation_record(
              &bad_state, 7U, INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY,
          "integration failure isolation rejects zero capacity");
    bad_state = table;
    bad_state.count = bad_state.capacity + 1U;
    CHECK(integration_failure_isolation_check(
              &bad_state, 7U, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_BAD_STATE,
          "integration failure isolation rejects corrupted count");
    check_no_allocation();
}

static void test_degradation_and_isolation(void)
{
    struct integration_failure_isolation_table table = make_table(4U);

    CHECK(integration_failure_isolation_record(
              &table, 7U, INTEGRATION_FAILURE_MALFORMED, 100U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              table.count == 1U &&
              table.peers[0].peer_id == 7U &&
              table.peers[0].consecutive_failures == 1U &&
              table.peers[0].state == INTEGRATION_PEER_STATE_DEGRADED &&
              table.peers[0].last_failure_tick == 100U &&
              table.peers[0].isolated_until_tick == 0U,
          "first integration failure publishes one degraded peer");

    CHECK(integration_failure_isolation_record(
              &table, 7U, INTEGRATION_FAILURE_UNAVAILABLE, 101U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              table.count == 1U &&
              table.peers[0].consecutive_failures == 2U &&
              table.peers[0].state == INTEGRATION_PEER_STATE_DEGRADED &&
              table.peers[0].last_failure_tick == 101U,
          "second integration failure advances the same degraded peer");

    CHECK(integration_failure_isolation_record(
              &table, 7U, INTEGRATION_FAILURE_SLOW, 102U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
              table.count == 1U &&
              table.peers[0].consecutive_failures == 3U &&
              table.peers[0].state == INTEGRATION_PEER_STATE_ISOLATED &&
              table.peers[0].last_failure_tick == 102U &&
              table.peers[0].isolated_until_tick == 132U,
          "third integration failure publishes exact slow isolation");

    CHECK(integration_failure_isolation_check(&table, 7U, 131U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED,
          "isolated integration peer is blocked before cooldown deadline");
    CHECK(integration_failure_isolation_check(&table, 7U, 132U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "isolated integration peer is allowed after cooldown deadline");
    CHECK(integration_failure_isolation_check(&table, 99U, 132U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "unknown integration peer is allowed until first failure");

    CHECK(integration_failure_isolation_record(
              &table, 7U, INTEGRATION_FAILURE_MALFORMED, 103U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
              table.peers[0].consecutive_failures == 3U &&
              table.peers[0].isolated_until_tick == 132U,
          "recorded fault during active isolation stays isolated without mutation");
    check_no_allocation();
}

static void test_per_kind_cooldowns(void)
{
    struct integration_failure_isolation_table table = make_table(4U);

    isolate_peer(&table, 10U, INTEGRATION_FAILURE_MALFORMED, 1000U);
    CHECK(table.peers[0].isolated_until_tick == 1012U,
          "malformed isolation uses exact ten-tick cooldown");
    isolate_peer(&table, 20U, INTEGRATION_FAILURE_UNAVAILABLE, 2000U);
    CHECK(table.peers[1].isolated_until_tick == 2022U,
          "unavailable isolation uses exact twenty-tick cooldown");
    isolate_peer(&table, 30U, INTEGRATION_FAILURE_SLOW, 3000U);
    CHECK(table.peers[2].isolated_until_tick == 3032U,
          "slow isolation uses exact thirty-tick cooldown");

    CHECK(integration_failure_isolation_check(&table, 10U, 1011U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
              integration_failure_isolation_check(&table, 10U, 1012U) ==
                  INTEGRATION_FAILURE_ISOLATION_OK,
          "malformed cooldown boundary is exact");
    CHECK(integration_failure_isolation_check(&table, 20U, 2021U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
              integration_failure_isolation_check(&table, 20U, 2022U) ==
                  INTEGRATION_FAILURE_ISOLATION_OK,
          "unavailable cooldown boundary is exact");
    CHECK(integration_failure_isolation_check(&table, 30U, 3031U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
              integration_failure_isolation_check(&table, 30U, 3032U) ==
                  INTEGRATION_FAILURE_ISOLATION_OK,
          "slow cooldown boundary is exact");
    check_no_allocation();
}

static void test_full_table_and_recover(void)
{
    struct integration_failure_isolation_table table = make_table(2U);
    unsigned char original[sizeof(table)];

    CHECK(integration_failure_isolation_record(
              &table, 1U, INTEGRATION_FAILURE_MALFORMED, 10U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              integration_failure_isolation_record(
                  &table, 2U, INTEGRATION_FAILURE_MALFORMED, 20U) ==
                  INTEGRATION_FAILURE_ISOLATION_OK,
          "full-table fixture fills capacity");
    capture_table(&table, original);
    CHECK(integration_failure_isolation_record(
              &table, 3U, INTEGRATION_FAILURE_MALFORMED, 30U) ==
              INTEGRATION_FAILURE_ISOLATION_TABLE_FULL,
          "full integration isolation table rejects unknown peer");
    check_table_unchanged(&table, original,
                          "full-table rejection leaves table unchanged");

    CHECK(integration_failure_isolation_recover(&table, 1U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              table.count == 1U &&
              table.peers[0].peer_id == 2U &&
              table.peers[0].consecutive_failures == 1U,
          "integration failure isolation recover compacts exact peer");
    CHECK(integration_failure_isolation_recover(&table, 99U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND,
          "integration failure isolation recover rejects unknown peer");
    CHECK(integration_failure_isolation_record(
              &table, 3U, INTEGRATION_FAILURE_MALFORMED, 30U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
              table.count == 2U &&
              table.peers[1].peer_id == 3U,
          "recovered integration failure slot is reusable");
    check_no_allocation();
}

static void test_local_operations_never_blocked(void)
{
    struct integration_failure_isolation_table table = make_table(2U);

    isolate_peer(&table, 1U, INTEGRATION_FAILURE_MALFORMED, 10U);
    isolate_peer(&table, 2U, INTEGRATION_FAILURE_SLOW, 20U);

    CHECK(integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_MANAGEMENT) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "local management is allowed while integration peers are isolated");
    CHECK(integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_SERVICE_SUPERVISION) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "local service supervision is allowed while integration peers are isolated");
    CHECK(integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_STATE_RECOVERY) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "local state recovery is allowed while integration peers are isolated");
    CHECK(integration_failure_isolation_local_allowed(
              (enum integration_local_operation)0U) ==
              INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION &&
              integration_failure_isolation_local_allowed(
                  (enum integration_local_operation)4U) ==
                  INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION,
          "unknown local operation is rejected");
    check_no_allocation();
}

static const char *expected_status_string(
    enum integration_failure_isolation_status status)
{
    switch (status) {
    case INTEGRATION_FAILURE_ISOLATION_OK:
        return "INTEGRATION_FAILURE_ISOLATION_OK";
    case INTEGRATION_FAILURE_ISOLATION_NULL:
        return "INTEGRATION_FAILURE_ISOLATION_NULL";
    case INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY:
        return "INTEGRATION_FAILURE_ISOLATION_BAD_CAPACITY";
    case INTEGRATION_FAILURE_ISOLATION_BAD_STATE:
        return "INTEGRATION_FAILURE_ISOLATION_BAD_STATE";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_PEER_ID";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_FAULT_KIND";
    case INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION:
        return "INTEGRATION_FAILURE_ISOLATION_INVALID_LOCAL_OPERATION";
    case INTEGRATION_FAILURE_ISOLATION_TABLE_FULL:
        return "INTEGRATION_FAILURE_ISOLATION_TABLE_FULL";
    case INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED:
        return "INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED";
    case INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND:
        return "INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_failure_isolation_status status;

    for (status = INTEGRATION_FAILURE_ISOLATION_OK;
         status <= INTEGRATION_FAILURE_ISOLATION_PEER_NOT_FOUND; ++status) {
        const char *string =
            integration_failure_isolation_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "integration failure isolation status string is exact");
    }
    CHECK(integration_failure_isolation_status_string(
              (enum integration_failure_isolation_status)10U) == NULL,
          "undefined integration failure isolation status is rejected");
    check_no_allocation();
}

int main(void)
{
    test_exact_constants();
    test_init();
    test_validation_failures();
    test_degradation_and_isolation();
    test_per_kind_cooldowns();
    test_full_table_and_recover();
    test_local_operations_never_blocked();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr,
                "Integration failure isolation host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration failure isolation host tests: ALL OK\n");
    return 0;
}

#endif
