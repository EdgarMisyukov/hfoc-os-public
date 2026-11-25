#include "local_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LOCAL_SESSION_HOST_TEST

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

static struct local_session_table make_table(uint32_t capacity) {
    struct local_session_table table;

    memset(&table, 0xA5, sizeof(table));
    CHECK(local_session_table_init(&table, capacity) == LOCAL_SESSION_OK,
          "table fixture initializes");
    return table;
}

static void capture_table(const struct local_session_table *table,
                          unsigned char *original)
{
    memcpy(original, table, sizeof(*table));
}

static void check_table_unchanged(
    const struct local_session_table *table,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(table, original, sizeof(*table)) == 0, message);
}

static void test_status_strings(void) {
    enum local_session_status status;

    for (status = LOCAL_SESSION_OK;
         status <= LOCAL_SESSION_INVALID_STATE; ++status) {
        const char *string = local_session_status_string(status);

        CHECK(string != NULL &&
                  (status == LOCAL_SESSION_OK ?
                       strcmp(string, "LOCAL_SESSION_OK") == 0 :
                   status == LOCAL_SESSION_NULL ?
                       strcmp(string, "LOCAL_SESSION_NULL") == 0 :
                   status == LOCAL_SESSION_BAD_CAPACITY ?
                       strcmp(string, "LOCAL_SESSION_BAD_CAPACITY") == 0 :
                   status == LOCAL_SESSION_INVALID_VERSION ?
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_VERSION") == 0 :
                   status == LOCAL_SESSION_INVALID_SESSION_ID ?
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_SESSION_ID") == 0 :
                   status == LOCAL_SESSION_INVALID_PRINCIPAL_ID ?
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_PRINCIPAL_ID") == 0 :
                   status == LOCAL_SESSION_INVALID_GENERATION ?
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_GENERATION") == 0 :
                   status == LOCAL_SESSION_INVALID_LIFETIME ?
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_LIFETIME") == 0 :
                   status == LOCAL_SESSION_INVALID_TICK ?
                       strcmp(string, "LOCAL_SESSION_INVALID_TICK") == 0 :
                   status == LOCAL_SESSION_TABLE_FULL ?
                       strcmp(string, "LOCAL_SESSION_TABLE_FULL") == 0 :
                   status == LOCAL_SESSION_ID_EXHAUSTED ?
                       strcmp(string, "LOCAL_SESSION_ID_EXHAUSTED") == 0 :
                   status == LOCAL_SESSION_STALE_GENERATION ?
                       strcmp(string,
                              "LOCAL_SESSION_STALE_GENERATION") == 0 :
                   status == LOCAL_SESSION_CLOSED ?
                       strcmp(string, "LOCAL_SESSION_CLOSED") == 0 :
                   status == LOCAL_SESSION_EXPIRED ?
                       strcmp(string, "LOCAL_SESSION_EXPIRED") == 0 :
                   status == LOCAL_SESSION_NOT_OWNER ?
                       strcmp(string, "LOCAL_SESSION_NOT_OWNER") == 0 :
                       strcmp(string,
                              "LOCAL_SESSION_INVALID_STATE") == 0),
              "local session status string is exact for every status");
    }
    CHECK(local_session_status_string(
              (enum local_session_status)16U) == NULL,
          "local session undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session status handling performs no allocation");
}

static void test_table_init(void) {
    struct local_session_table table;
    uint32_t index;

    CHECK(local_session_table_init(NULL, 1U) == LOCAL_SESSION_NULL,
          "local session table init rejects null table");
    CHECK(local_session_table_init(&table, 0U) ==
              LOCAL_SESSION_BAD_CAPACITY,
          "local session zero capacity is rejected");
    CHECK(local_session_table_init(&table, LOCAL_SESSION_TABLE_MAX + 1U) ==
              LOCAL_SESSION_BAD_CAPACITY,
          "local session over-limit capacity is rejected");
    CHECK(local_session_table_init(&table, 2U) == LOCAL_SESSION_OK &&
              table.capacity == 2U &&
              table.next_id == LOCAL_SESSION_ID_MIN,
          "local session table init publishes exact capacity and next id");
    for (index = 0U; index < LOCAL_SESSION_TABLE_MAX; ++index) {
        CHECK(table.sessions[index].version == LOCAL_SESSION_VERSION &&
                  table.sessions[index].id == 0U &&
                  table.sessions[index].principal_id == 0U &&
                  table.sessions[index].generation == 0U &&
                  table.sessions[index].opened_tick == 0U &&
                  table.sessions[index].expires_tick == 0U &&
                  table.sessions[index].state ==
                      LOCAL_SESSION_STATE_CLOSED,
              "local session table init initializes every slot exactly");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session table init performs no allocation");
}

static void test_open_failures(void) {
    struct local_session_table table = make_table(2U);
    struct local_session_table bad_table = table;
    unsigned char original[sizeof(table)];
    uint32_t out_id = 0xDEADU;

    capture_table(&table, original);
    CHECK(local_session_open(NULL, 1U, 1U, 0U, 1U, &out_id) ==
              LOCAL_SESSION_NULL &&
              out_id == 0xDEADU,
          "local session open rejects null table without touching output");

    bad_table.capacity = 0U;
    out_id = 0xDEADU;
    CHECK(local_session_open(&bad_table, 1U, 1U, 0U, 1U, &out_id) ==
              LOCAL_SESSION_BAD_CAPACITY &&
              out_id == 0xDEADU,
          "local session open rejects zero capacity");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 0U, 1U, 0U, 1U, &out_id) ==
              LOCAL_SESSION_INVALID_PRINCIPAL_ID &&
              out_id == 0xDEADU,
          "local session open rejects zero principal id");
    check_table_unchanged(&table, original,
                          "zero principal open failure leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table,
                             LOCAL_SESSION_PRINCIPAL_ID_MAX + 1U,
                             1U, 0U, 1U, &out_id) ==
              LOCAL_SESSION_INVALID_PRINCIPAL_ID &&
              out_id == 0xDEADU,
          "local session open rejects over-limit principal id");
    check_table_unchanged(&table, original,
                          "over-limit principal open failure leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 1U, 0U, 0U, 1U, &out_id) ==
              LOCAL_SESSION_INVALID_GENERATION &&
              out_id == 0xDEADU,
          "local session open rejects zero generation");
    check_table_unchanged(&table, original,
                          "invalid generation open failure leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 1U, 1U, 0U, 0U, &out_id) ==
              LOCAL_SESSION_INVALID_LIFETIME &&
              out_id == 0xDEADU,
          "local session open rejects zero lifetime");
    check_table_unchanged(&table, original,
                          "invalid lifetime open failure leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 1U, 1U, 0U,
                             LOCAL_SESSION_LIFETIME_MAX + 1U, &out_id) ==
              LOCAL_SESSION_INVALID_LIFETIME &&
              out_id == 0xDEADU,
          "local session open rejects over-limit lifetime");
    check_table_unchanged(&table, original,
                          "over-limit lifetime open failure leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 1U, 1U, UINT64_MAX, 1U, &out_id) ==
              LOCAL_SESSION_INVALID_TICK &&
              out_id == 0xDEADU,
          "local session open rejects unrepresentable expiry");
    check_table_unchanged(&table, original,
                          "unrepresentable expiry leaves table unchanged");

    out_id = 0xDEADU;
    CHECK(local_session_open(&table, 1U, 1U, 0U, 1U, NULL) ==
              LOCAL_SESSION_NULL,
          "local session open rejects null output");
    check_table_unchanged(&table, original,
                          "null output open failure leaves table unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session open failures perform no allocation");
}

static void test_open_expiry_and_close(void) {
    struct local_session_table table = make_table(2U);
    uint32_t session_id = 0U;

    CHECK(local_session_open(&table, 7U, 1U, 100U, 100U, &session_id) ==
              LOCAL_SESSION_OK &&
              session_id == 1U &&
              table.next_id == 2U &&
              table.sessions[0].version == LOCAL_SESSION_VERSION &&
              table.sessions[0].id == 1U &&
              table.sessions[0].principal_id == 7U &&
              table.sessions[0].generation == 1U &&
              table.sessions[0].opened_tick == 100U &&
              table.sessions[0].expires_tick == 200U &&
              table.sessions[0].state == LOCAL_SESSION_STATE_OPEN,
          "local session open publishes exact bounded session fields");

    CHECK(local_session_validate(&table, session_id, 7U, 1U, 100U) ==
              LOCAL_SESSION_OK,
          "local session validates immediately after open");
    CHECK(local_session_validate(&table, session_id, 7U, 1U, 199U) ==
              LOCAL_SESSION_OK,
          "local session validates just before expiry");
    CHECK(local_session_validate(&table, session_id, 7U, 1U, 200U) ==
              LOCAL_SESSION_EXPIRED,
          "local session rejects at exact expiry");
    CHECK(local_session_validate(&table, session_id, 7U, 1U, 300U) ==
              LOCAL_SESSION_EXPIRED,
          "local session rejects after expiry");

    CHECK(local_session_close(&table, session_id) == LOCAL_SESSION_OK &&
              table.sessions[0].state == LOCAL_SESSION_STATE_CLOSED,
          "local session close marks the session closed");
    CHECK(local_session_validate(&table, session_id, 7U, 1U, 150U) ==
              LOCAL_SESSION_CLOSED,
          "local session rejects a closed handle");
    CHECK(local_session_close(&table, session_id) == LOCAL_SESSION_OK,
          "local session close is idempotent");
    CHECK(local_session_close(&table, 2U) ==
              LOCAL_SESSION_INVALID_SESSION_ID,
          "local session close rejects an unknown session id");
    CHECK(local_session_close(NULL, session_id) == LOCAL_SESSION_NULL,
          "local session close rejects null table");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session lifecycle performs no allocation");
}

static void test_full_and_non_reuse(void) {
    struct local_session_table table = make_table(2U);
    uint32_t first = 0U;
    uint32_t second = 0U;
    uint32_t third = 0U;

    CHECK(local_session_open(&table, 1U, 1U, 0U, 10U, &first) ==
              LOCAL_SESSION_OK &&
              first == 1U,
          "local session first open allocates id 1");
    CHECK(local_session_open(&table, 2U, 1U, 0U, 10U, &second) ==
              LOCAL_SESSION_OK &&
              second == 2U,
          "local session second open allocates id 2");
    CHECK(local_session_open(&table, 3U, 1U, 0U, 10U, &third) ==
              LOCAL_SESSION_TABLE_FULL &&
              third == 0U,
          "local session table-full open is rejected without output");

    CHECK(local_session_close(&table, first) == LOCAL_SESSION_OK,
          "local session first session closes before reuse");
    CHECK(local_session_open(&table, 3U, 1U, 0U, 10U, &third) ==
              LOCAL_SESSION_OK &&
              third == 3U,
          "local session closed slot reuses with a new monotonic id");
    CHECK(local_session_validate(&table, first, 1U, 1U, 0U) ==
              LOCAL_SESSION_INVALID_SESSION_ID,
          "local session old id cannot authorize after slot reuse");
    CHECK(local_session_validate(&table, third, 3U, 1U, 0U) ==
              LOCAL_SESSION_OK,
          "local session new id validates after slot reuse");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session non-reuse handling performs no allocation");
}

static void test_validation_failures(void) {
    struct local_session_table table = make_table(2U);
    uint32_t session_id = 0U;

    CHECK(local_session_open(&table, 5U, 1U, 50U, 100U, &session_id) ==
              LOCAL_SESSION_OK &&
              session_id == 1U,
          "local session validation fixture opens a session");

    CHECK(local_session_validate(&table, session_id, 5U, 2U, 60U) ==
              LOCAL_SESSION_STALE_GENERATION,
          "local session rejects a stale generation");
    CHECK(local_session_validate(&table, session_id, 6U, 1U, 60U) ==
              LOCAL_SESSION_NOT_OWNER,
          "local session rejects a wrong principal owner");
    CHECK(local_session_validate(&table, 0U, 5U, 1U, 60U) ==
              LOCAL_SESSION_INVALID_SESSION_ID,
          "local session validates zero session id as invalid");
    CHECK(local_session_validate(&table,
                                 LOCAL_SESSION_ID_MAX + 1U,
                                 5U, 1U, 60U) ==
              LOCAL_SESSION_INVALID_SESSION_ID,
          "local session validates over-limit session id as invalid");
    CHECK(local_session_validate(&table, session_id, 0U, 1U, 60U) ==
              LOCAL_SESSION_INVALID_PRINCIPAL_ID,
          "local session validates zero principal id as invalid");
    CHECK(local_session_validate(&table, session_id, 5U, 0U, 60U) ==
              LOCAL_SESSION_INVALID_GENERATION,
          "local session validates zero generation as invalid");
    CHECK(local_session_validate(NULL, session_id, 5U, 1U, 60U) ==
              LOCAL_SESSION_NULL,
          "local session validate rejects null table");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session validation failures perform no allocation");
}

static void test_id_exhaustion(void) {
    struct local_session_table table = make_table(1U);
    unsigned char original[sizeof(table)];
    uint32_t session_id = 0U;
    uint32_t next_id = 0U;

    table.next_id = LOCAL_SESSION_ID_MAX;
    CHECK(local_session_open(&table, 1U, 1U, 0U, 10U, &session_id) ==
              LOCAL_SESSION_OK &&
              session_id == LOCAL_SESSION_ID_MAX &&
              table.next_id == LOCAL_SESSION_ID_MAX + 1U,
          "local session publishes the maximum monotonic id");
    CHECK(local_session_close(&table, session_id) == LOCAL_SESSION_OK,
          "local session closes the maximum-id session before exhaustion");

    capture_table(&table, original);
    CHECK(local_session_open(&table, 2U, 1U, 0U, 10U, &next_id) ==
              LOCAL_SESSION_ID_EXHAUSTED &&
              next_id == 0U,
          "local session open rejects monotonic id exhaustion");
    check_table_unchanged(&table, original,
                          "id exhaustion open failure leaves table unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session id exhaustion performs no allocation");
}

static void test_corrupt_session_failures(void) {
    struct local_session_table table = make_table(2U);
    uint32_t session_id = 0U;

    CHECK(local_session_open(&table, 1U, 1U, 0U, 100U, &session_id) ==
              LOCAL_SESSION_OK &&
              session_id == 1U,
          "local session corrupt-state fixture opens a session");

    table.sessions[0].version = LOCAL_SESSION_VERSION + 1U;
    CHECK(local_session_validate(&table, session_id, 1U, 1U, 0U) ==
              LOCAL_SESSION_INVALID_VERSION,
          "local session rejects an unknown session version");
    table.sessions[0].version = LOCAL_SESSION_VERSION;

    table.sessions[0].state = (enum local_session_state)2U;
    CHECK(local_session_validate(&table, session_id, 1U, 1U, 0U) ==
              LOCAL_SESSION_INVALID_STATE,
          "local session rejects an invalid session state");
    CHECK(local_session_close(&table, session_id) ==
              LOCAL_SESSION_INVALID_STATE,
          "local session close rejects an invalid session state");

    table.sessions[0].state = LOCAL_SESSION_STATE_OPEN;
    CHECK(local_session_validate(&table, session_id, 1U, 1U, 0U) ==
              LOCAL_SESSION_OK &&
              local_session_close(&table, session_id) == LOCAL_SESSION_OK,
          "local session recovers after repairing a corrupt state fixture");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session corrupt-state handling performs no allocation");
}

static void test_generation_invalidation(void) {
    struct local_session_table table = make_table(2U);
    uint32_t principal_one = 0U;
    uint32_t principal_two = 0U;
    unsigned char original[sizeof(table)];

    CHECK(local_session_open(&table, 1U, 1U, 0U, 100U, &principal_one) ==
              LOCAL_SESSION_OK &&
              principal_one == 1U,
          "local session principal one opens generation 1");
    CHECK(local_session_open(&table, 2U, 1U, 0U, 100U, &principal_two) ==
              LOCAL_SESSION_OK &&
              principal_two == 2U,
          "local session principal two opens generation 1");

    CHECK(local_session_invalidate_generation(&table, 1U, 2U) ==
              LOCAL_SESSION_OK &&
              table.sessions[0].state == LOCAL_SESSION_STATE_CLOSED &&
              table.sessions[1].state == LOCAL_SESSION_STATE_OPEN,
          "local session generation invalidation closes only stale owner");

    CHECK(local_session_validate(&table, principal_one, 1U, 2U, 0U) ==
              LOCAL_SESSION_STALE_GENERATION,
          "stale generation handle is rejected after invalidation");
    CHECK(local_session_validate(&table, principal_one, 1U, 1U, 0U) ==
              LOCAL_SESSION_CLOSED,
          "closed stale handle cannot authorize with old generation");
    CHECK(local_session_validate(&table, principal_two, 2U, 1U, 0U) ==
              LOCAL_SESSION_OK,
          "unrelated principal session remains valid after invalidation");

    capture_table(&table, original);
    CHECK(local_session_invalidate_generation(NULL, 1U, 2U) ==
              LOCAL_SESSION_NULL,
          "local session generation invalidation rejects null table");
    check_table_unchanged(&table, original,
                          "null invalidation failure leaves table unchanged");
    capture_table(&table, original);
    CHECK(local_session_invalidate_generation(&table, 0U, 2U) ==
              LOCAL_SESSION_INVALID_PRINCIPAL_ID,
          "local session generation invalidation rejects zero principal");
    check_table_unchanged(&table, original,
                          "invalid principal invalidation leaves table unchanged");
    capture_table(&table, original);
    CHECK(local_session_invalidate_generation(&table, 1U, 0U) ==
              LOCAL_SESSION_INVALID_GENERATION,
          "local session generation invalidation rejects zero generation");
    check_table_unchanged(&table, original,
                          "invalid generation invalidation leaves table unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "local session generation invalidation performs no allocation");
}

int main(void) {
    test_status_strings();
    test_table_init();
    test_open_failures();
    test_open_expiry_and_close();
    test_full_and_non_reuse();
    test_validation_failures();
    test_id_exhaustion();
    test_corrupt_session_failures();
    test_generation_invalidation();

    if (failures != 0) {
        fprintf(stderr, "Local session host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Local session host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_LOCAL_SESSION_HOST_TEST"
#endif
