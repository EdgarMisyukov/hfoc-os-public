#include "auth_throttle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUTH_THROTTLE_HOST_TEST

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

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct auth_throttle_table make_table(
    uint32_t capacity,
    uint32_t failure_limit,
    uint32_t cooldown_ticks)
{
    struct auth_throttle_table table;

    memset(&table, 0xA5, sizeof(table));
    CHECK(auth_throttle_table_init(&table, capacity, failure_limit,
                                   cooldown_ticks) == AUTH_THROTTLE_OK,
          "auth throttle fixture initializes");
    return table;
}

static void capture_table(const struct auth_throttle_table *table,
                          unsigned char *original)
{
    memcpy(original, table, sizeof(*table));
}

static void check_table_unchanged(
    const struct auth_throttle_table *table,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(table, original, sizeof(*table)) == 0, message);
}

static void test_status_strings(void) {
    enum auth_throttle_status status;

    for (status = AUTH_THROTTLE_OK;
         status <= AUTH_THROTTLE_INVALID_STATE; ++status) {
        const char *string = auth_throttle_status_string(status);

        CHECK(string != NULL &&
                  (status == AUTH_THROTTLE_OK ?
                       strcmp(string, "AUTH_THROTTLE_OK") == 0 :
                   status == AUTH_THROTTLE_NULL ?
                       strcmp(string, "AUTH_THROTTLE_NULL") == 0 :
                   status == AUTH_THROTTLE_BAD_CAPACITY ?
                       strcmp(string, "AUTH_THROTTLE_BAD_CAPACITY") == 0 :
                   status == AUTH_THROTTLE_INVALID_VERSION ?
                       strcmp(string,
                              "AUTH_THROTTLE_INVALID_VERSION") == 0 :
                   status == AUTH_THROTTLE_INVALID_PRINCIPAL_ID ?
                       strcmp(string,
                              "AUTH_THROTTLE_INVALID_PRINCIPAL_ID") == 0 :
                   status == AUTH_THROTTLE_INVALID_FAILURE_LIMIT ?
                       strcmp(string,
                              "AUTH_THROTTLE_INVALID_FAILURE_LIMIT") == 0 :
                   status == AUTH_THROTTLE_INVALID_COOLDOWN ?
                       strcmp(string,
                              "AUTH_THROTTLE_INVALID_COOLDOWN") == 0 :
                   status == AUTH_THROTTLE_INVALID_TICK ?
                       strcmp(string, "AUTH_THROTTLE_INVALID_TICK") == 0 :
                   status == AUTH_THROTTLE_TABLE_FULL ?
                       strcmp(string, "AUTH_THROTTLE_TABLE_FULL") == 0 :
                   status == AUTH_THROTTLE_LOCKED ?
                       strcmp(string, "AUTH_THROTTLE_LOCKED") == 0 :
                       strcmp(string,
                              "AUTH_THROTTLE_INVALID_STATE") == 0),
              "auth throttle status string is exact for every status");
    }
    CHECK(auth_throttle_status_string(
              (enum auth_throttle_status)11U) == NULL,
          "auth throttle undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle status handling performs no allocation");
}

static void test_table_init(void) {
    struct auth_throttle_table table;
    uint32_t index;

    CHECK(auth_throttle_table_init(NULL, 1U, 1U, 1U) ==
              AUTH_THROTTLE_NULL,
          "auth throttle table init rejects null table");
    CHECK(auth_throttle_table_init(&table, 0U, 1U, 1U) ==
              AUTH_THROTTLE_BAD_CAPACITY,
          "auth throttle zero capacity is rejected");
    CHECK(auth_throttle_table_init(&table,
                                   AUTH_THROTTLE_TABLE_MAX + 1U,
                                   1U, 1U) == AUTH_THROTTLE_BAD_CAPACITY,
          "auth throttle over-limit capacity is rejected");
    CHECK(auth_throttle_table_init(&table, 1U, 0U, 1U) ==
              AUTH_THROTTLE_INVALID_FAILURE_LIMIT,
          "auth throttle zero failure limit is rejected");
    CHECK(auth_throttle_table_init(&table, 1U,
                                   AUTH_THROTTLE_FAILURE_LIMIT_MAX + 1U,
                                   1U) ==
              AUTH_THROTTLE_INVALID_FAILURE_LIMIT,
          "auth throttle over-limit failure limit is rejected");
    CHECK(auth_throttle_table_init(&table, 1U, 1U, 0U) ==
              AUTH_THROTTLE_INVALID_COOLDOWN,
          "auth throttle zero cooldown is rejected");
    CHECK(auth_throttle_table_init(&table, 1U, 1U,
                                   AUTH_THROTTLE_COOLDOWN_MAX + 1U) ==
              AUTH_THROTTLE_INVALID_COOLDOWN,
          "auth throttle over-limit cooldown is rejected");

    CHECK(auth_throttle_table_init(&table, 2U, 3U, 10U) ==
              AUTH_THROTTLE_OK &&
              table.version == AUTH_THROTTLE_VERSION &&
              table.capacity == 2U &&
              table.failure_limit == 3U &&
              table.cooldown_ticks == 10U,
          "auth throttle table init publishes exact configuration");
    for (index = 0U; index < AUTH_THROTTLE_TABLE_MAX; ++index) {
        CHECK(table.entries[index].version == AUTH_THROTTLE_VERSION &&
                  table.entries[index].principal_id == 0U &&
                  table.entries[index].failure_count == 0U &&
                  table.entries[index].locked_until_tick == 0U &&
                  table.entries[index].state ==
                      AUTH_THROTTLE_ENTRY_FREE,
              "auth throttle table init initializes every entry exactly");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle table init performs no allocation");
}

static void test_record_failure_validation(void) {
    struct auth_throttle_table table = make_table(2U, 3U, 10U);
    struct auth_throttle_table bad_table = table;
    unsigned char original[sizeof(table)];
    uint64_t out = 0xDEADBEEFDEADBEEFULL;

    capture_table(&table, original);
    CHECK(auth_throttle_record_failure(NULL, 1U, 0U, &out) ==
              AUTH_THROTTLE_NULL &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects null table without touching output");
    CHECK(auth_throttle_record_failure(&table, 1U, 0U, NULL) ==
              AUTH_THROTTLE_NULL,
          "auth throttle record rejects null output");

    bad_table.capacity = 0U;
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(&bad_table, 1U, 0U, &out) ==
              AUTH_THROTTLE_BAD_CAPACITY &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects zero capacity");

    table.version = AUTH_THROTTLE_VERSION + 1U;
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(&table, 1U, 0U, &out) ==
              AUTH_THROTTLE_INVALID_VERSION &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects unknown table version");
    table.version = AUTH_THROTTLE_VERSION;

    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(&table, 0U, 0U, &out) ==
              AUTH_THROTTLE_INVALID_PRINCIPAL_ID &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects zero principal id");
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(
              &table, AUTH_THROTTLE_PRINCIPAL_ID_MAX + 1U,
              0U, &out) == AUTH_THROTTLE_INVALID_PRINCIPAL_ID &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects over-limit principal id");
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(&table, 1U, UINT64_MAX, &out) ==
              AUTH_THROTTLE_INVALID_TICK &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle record rejects unrepresentable lock tick");
    check_table_unchanged(&table, original,
                          "auth throttle validation failures leave table unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle validation performs no allocation");
}

static void test_table_full(void) {
    struct auth_throttle_table table = make_table(2U, 3U, 10U);
    unsigned char original[sizeof(table)];
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 1U, 0U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U,
          "auth throttle first principal occupies a slot");
    CHECK(auth_throttle_record_failure(&table, 2U, 0U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U,
          "auth throttle second principal occupies a slot");

    capture_table(&table, original);
    out = 0xDEADBEEFDEADBEEFULL;
    CHECK(auth_throttle_record_failure(&table, 3U, 0U, &out) ==
              AUTH_THROTTLE_TABLE_FULL &&
              out == 0xDEADBEEFDEADBEEFULL,
          "auth throttle table-full is rejected without output");
    check_table_unchanged(&table, original,
                          "auth throttle table-full leaves table unchanged");
    CHECK(auth_throttle_check(&table, 1U, 0U) == AUTH_THROTTLE_OK &&
              auth_throttle_check(&table, 2U, 0U) == AUTH_THROTTLE_OK,
          "auth throttle table-full does not block existing principals");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle table-full handling performs no allocation");
}

static void test_count_limit_cooldown(void) {
    struct auth_throttle_table table = make_table(4U, 3U, 100U);
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 7U, 1000U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U &&
              table.entries[0].principal_id == 7U &&
              table.entries[0].failure_count == 1U &&
              table.entries[0].locked_until_tick == 0U &&
              table.entries[0].state == AUTH_THROTTLE_ENTRY_ACTIVE,
          "auth throttle first failure publishes count without lock");
    CHECK(auth_throttle_record_failure(&table, 7U, 1010U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U &&
              table.entries[0].failure_count == 2U,
          "auth throttle second failure increments the same principal");
    CHECK(auth_throttle_record_failure(&table, 7U, 1020U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 1120U &&
              table.entries[0].failure_count == 3U &&
              table.entries[0].locked_until_tick == 1120U,
          "auth throttle reaching the limit publishes exact cooldown");

    CHECK(auth_throttle_check(&table, 7U, 1119U) == AUTH_THROTTLE_LOCKED,
          "auth throttle rejects principal before cooldown expiry");
    CHECK(auth_throttle_check(&table, 7U, 1120U) == AUTH_THROTTLE_OK,
          "auth throttle allows principal at exact cooldown expiry");

    out = 0U;
    CHECK(auth_throttle_record_failure(&table, 7U, 1100U, &out) ==
              AUTH_THROTTLE_LOCKED &&
              out == 1120U &&
              table.entries[0].failure_count == 3U &&
              table.entries[0].locked_until_tick == 1120U,
          "auth throttle locked failure publishes current lock without extension");
    CHECK(auth_throttle_record_failure(&table, 7U, 1105U, &out) ==
              AUTH_THROTTLE_LOCKED &&
              out == 1120U &&
              table.entries[0].failure_count == 3U &&
              table.entries[0].locked_until_tick == 1120U,
          "auth throttle repeated locked failures do not extend cooldown");

    CHECK(auth_throttle_record_failure(&table, 7U, 1120U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U &&
              table.entries[0].failure_count == 1U &&
              table.entries[0].locked_until_tick == 0U,
          "auth throttle fresh failure after expiry starts a new count");
    CHECK(auth_throttle_record_failure(&table, 7U, 1130U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U &&
              table.entries[0].failure_count == 2U,
          "auth throttle new window tracks the second failure");
    CHECK(auth_throttle_record_failure(&table, 7U, 1140U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 1240U &&
              table.entries[0].failure_count == 3U &&
              table.entries[0].locked_until_tick == 1240U,
          "auth throttle new window reaches the limit and locks again");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle cooldown handling performs no allocation");
}

static void test_limit_one(void) {
    struct auth_throttle_table table = make_table(1U, 1U, 50U);
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 9U, 100U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 150U &&
              table.entries[0].failure_count == 1U &&
              table.entries[0].locked_until_tick == 150U,
          "auth throttle limit-one failure locks immediately");
    CHECK(auth_throttle_check(&table, 9U, 149U) == AUTH_THROTTLE_LOCKED &&
              auth_throttle_check(&table, 9U, 150U) == AUTH_THROTTLE_OK,
          "auth throttle limit-one cooldown has an exact expiry");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle limit-one handling performs no allocation");
}

static void test_unrelated_principals(void) {
    struct auth_throttle_table table = make_table(2U, 2U, 100U);
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 7U, 0U, &out) ==
              AUTH_THROTTLE_OK,
          "auth throttle unrelated fixture records principal 7");
    CHECK(auth_throttle_record_failure(&table, 8U, 0U, &out) ==
              AUTH_THROTTLE_OK,
          "auth throttle unrelated fixture records principal 8");
    CHECK(auth_throttle_record_failure(&table, 7U, 10U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 110U,
          "auth throttle locks principal 7");
    CHECK(auth_throttle_check(&table, 7U, 50U) == AUTH_THROTTLE_LOCKED &&
              auth_throttle_check(&table, 8U, 50U) == AUTH_THROTTLE_OK,
          "auth throttle lock on one principal does not block another");
    CHECK(auth_throttle_record_failure(&table, 8U, 60U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 160U &&
              table.entries[1].principal_id == 8U &&
              table.entries[1].failure_count == 2U,
          "auth throttle unrelated principal tracks its own independent count");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle unrelated principal handling performs no allocation");
}

static void test_reset(void) {
    struct auth_throttle_table table = make_table(2U, 3U, 10U);
    unsigned char original[sizeof(table)];
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 7U, 0U, &out) ==
              AUTH_THROTTLE_OK,
          "auth throttle reset fixture records principal 7");
    CHECK(auth_throttle_record_failure(&table, 8U, 0U, &out) ==
              AUTH_THROTTLE_OK,
          "auth throttle reset fixture records principal 8");

    CHECK(auth_throttle_reset(&table, 7U) == AUTH_THROTTLE_OK &&
              table.entries[0].state == AUTH_THROTTLE_ENTRY_FREE &&
              table.entries[0].principal_id == 0U &&
              table.entries[0].failure_count == 0U &&
              table.entries[0].locked_until_tick == 0U &&
              table.entries[1].state == AUTH_THROTTLE_ENTRY_ACTIVE,
          "auth throttle reset clears only the selected principal");
    CHECK(auth_throttle_check(&table, 7U, 0U) == AUTH_THROTTLE_OK &&
              auth_throttle_check(&table, 8U, 0U) == AUTH_THROTTLE_OK,
          "auth throttle reset leaves unrelated principal usable");

    capture_table(&table, original);
    CHECK(auth_throttle_reset(&table, 7U) == AUTH_THROTTLE_OK,
          "auth throttle reset of an unknown principal is idempotent");
    check_table_unchanged(&table, original,
                          "auth throttle unknown reset leaves table unchanged");

    CHECK(auth_throttle_reset(NULL, 7U) == AUTH_THROTTLE_NULL,
          "auth throttle reset rejects null table");
    CHECK(auth_throttle_reset(&table, 0U) ==
              AUTH_THROTTLE_INVALID_PRINCIPAL_ID,
          "auth throttle reset rejects zero principal id");
    table.version = AUTH_THROTTLE_VERSION + 1U;
    CHECK(auth_throttle_reset(&table, 7U) ==
              AUTH_THROTTLE_INVALID_VERSION,
          "auth throttle reset rejects unknown table version");
    table.version = AUTH_THROTTLE_VERSION;
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle reset performs no allocation");
}

static void test_corrupt_state(void) {
    struct auth_throttle_table table = make_table(1U, 3U, 10U);
    unsigned char original[sizeof(table)];
    struct auth_throttle_entry *entry = &table.entries[0];
    uint64_t out = 0U;

    CHECK(auth_throttle_record_failure(&table, 5U, 0U, &out) ==
              AUTH_THROTTLE_OK,
          "auth throttle corrupt-state fixture records principal");

    entry->version = AUTH_THROTTLE_VERSION + 1U;
    capture_table(&table, original);
    CHECK(auth_throttle_check(&table, 5U, 0U) ==
              AUTH_THROTTLE_INVALID_STATE &&
              auth_throttle_record_failure(&table, 5U, 0U, &out) ==
                  AUTH_THROTTLE_INVALID_STATE &&
              auth_throttle_reset(&table, 5U) ==
                  AUTH_THROTTLE_INVALID_STATE,
          "auth throttle rejects an unknown entry version");
    check_table_unchanged(&table, original,
                          "auth throttle invalid version leaves table unchanged");
    entry->version = AUTH_THROTTLE_VERSION;

    entry->state = (enum auth_throttle_entry_state)2U;
    capture_table(&table, original);
    CHECK(auth_throttle_check(&table, 5U, 0U) ==
              AUTH_THROTTLE_INVALID_STATE,
          "auth throttle rejects an invalid entry state");
    check_table_unchanged(&table, original,
                          "auth throttle invalid state leaves table unchanged");
    entry->state = AUTH_THROTTLE_ENTRY_ACTIVE;

    entry->failure_count = table.failure_limit + 1U;
    capture_table(&table, original);
    CHECK(auth_throttle_check(&table, 5U, 0U) ==
              AUTH_THROTTLE_INVALID_STATE,
          "auth throttle rejects an over-limit failure count");
    check_table_unchanged(&table, original,
                          "auth throttle over-limit count leaves table unchanged");
    entry->failure_count = 1U;

    entry->locked_until_tick = 1U;
    capture_table(&table, original);
    CHECK(auth_throttle_check(&table, 5U, 0U) ==
              AUTH_THROTTLE_INVALID_STATE,
          "auth throttle rejects a lock tick below the failure limit");
    check_table_unchanged(&table, original,
                          "auth throttle invalid lock leaves table unchanged");
    entry->locked_until_tick = 0U;
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle corrupt-state handling performs no allocation");
}

static volatile unsigned int interrupt_depth;

static enum auth_throttle_status record_from_interrupt_context(
    struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick,
    uint64_t *out_locked_until_tick)
{
    enum auth_throttle_status status;

    ++interrupt_depth;
    status = auth_throttle_record_failure(
        table, principal_id, now_tick, out_locked_until_tick);
    --interrupt_depth;
    return status;
}

static enum auth_throttle_status check_from_interrupt_context(
    const struct auth_throttle_table *table,
    uint32_t principal_id,
    uint64_t now_tick)
{
    enum auth_throttle_status status;

    ++interrupt_depth;
    status = auth_throttle_check(table, principal_id, now_tick);
    --interrupt_depth;
    return status;
}

static void test_interrupt_context_use(void) {
    struct auth_throttle_table table = make_table(2U, 2U, 10U);
    uint64_t out = 0U;

    CHECK(record_from_interrupt_context(&table, 4U, 100U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 0U,
          "auth throttle records from an interrupt-context path");
    CHECK(record_from_interrupt_context(&table, 4U, 110U, &out) ==
              AUTH_THROTTLE_OK &&
              out == 120U,
          "auth throttle locks from an interrupt-context path");
    CHECK(check_from_interrupt_context(&table, 4U, 119U) ==
              AUTH_THROTTLE_LOCKED &&
              check_from_interrupt_context(&table, 4U, 120U) ==
                  AUTH_THROTTLE_OK,
          "auth throttle checks from an interrupt-context path");
    CHECK(interrupt_depth == 0U &&
              alloc_attempts == 0U && free_attempts == 0U,
          "auth throttle interrupt-context use performs no allocation");
}

int main(void) {
    test_status_strings();
    test_table_init();
    test_record_failure_validation();
    test_table_full();
    test_count_limit_cooldown();
    test_limit_one();
    test_unrelated_principals();
    test_reset();
    test_corrupt_state();
    test_interrupt_context_use();

    if (failures != 0) {
        fprintf(stderr, "Auth throttle host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Auth throttle host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_AUTH_THROTTLE_HOST_TEST"
#endif
