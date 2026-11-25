#include "capability.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CAPABILITY_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct capability_table table;

static void table_reset(void) {
    memset(&table, 0, sizeof(table));
}

static bool table_published_empty(const struct capability_table *t) {
    return t != NULL && t->published && t->open_count == 0U;
}

static void test_validation_and_unpublished_table(void) {
    struct capability capability = {1U, 2U, 0x1000U};
    struct capability output;
    uint64_t handle = 0U;

    table_reset();
    CHECK(capability_table_init(NULL) == CAPABILITY_NULL,
          "table init rejects null table");
    CHECK(capability_table_destroy(NULL) == CAPABILITY_NULL,
          "table destroy rejects null table");
    CHECK(capability_table_reserve(NULL, &handle) == CAPABILITY_NULL,
          "table reserve rejects null table");
    CHECK(capability_table_commit(NULL, 1U, &capability) ==
              CAPABILITY_NULL,
          "table commit rejects null table");
    CHECK(capability_table_rollback(NULL, 1U) == CAPABILITY_NULL,
          "table rollback rejects null table");
    CHECK(capability_table_add(NULL, &capability, &handle) ==
              CAPABILITY_NULL,
          "table add rejects null table");
    CHECK(capability_table_close(NULL, 1U) == CAPABILITY_NULL,
          "table close rejects null table");
    CHECK(capability_table_revoke(NULL, 1U) == CAPABILITY_NULL,
          "table revoke rejects null table");
    CHECK(capability_table_get(NULL, 1U, &output) == CAPABILITY_NULL,
          "table get rejects null table");
    CHECK(capability_table_open_count(NULL) == 0U,
          "null table open count is zero");
    CHECK(!capability_table_is_published(NULL),
          "null table is not published");

    CHECK(capability_table_reserve(&table, &handle) == CAPABILITY_BAD_STATE,
          "reserve rejects unpublished table");
    CHECK(capability_table_commit(&table, 1U, &capability) ==
              CAPABILITY_BAD_STATE,
          "commit rejects unpublished table");
    CHECK(capability_table_rollback(&table, 1U) == CAPABILITY_BAD_STATE,
          "rollback rejects unpublished table");
    CHECK(capability_table_add(&table, &capability, &handle) ==
              CAPABILITY_BAD_STATE,
          "add rejects unpublished table");
    CHECK(capability_table_close(&table, 1U) == CAPABILITY_BAD_STATE,
          "close rejects unpublished table");
    CHECK(capability_table_revoke(&table, 1U) == CAPABILITY_BAD_STATE,
          "revoke rejects unpublished table");
    CHECK(capability_table_get(&table, 1U, &output) ==
              CAPABILITY_BAD_STATE,
          "get rejects unpublished table");

    CHECK(capability_table_init(&table) == CAPABILITY_OK &&
              table_published_empty(&table),
          "table init publishes empty table");
    CHECK(capability_table_init(&table) == CAPABILITY_BAD_STATE,
          "double table init is rejected");
}

static void test_add_get_close_revoke_duplicate(void) {
    struct capability capability_a = {1U, 2U, 0x1000U};
    struct capability capability_b = {2U, 4U, 0x2000U};
    struct capability output;
    uint64_t handle_a = 0U;
    uint64_t handle_b = 0U;
    uint64_t duplicate = 0U;

    table_reset();
    CHECK(capability_table_init(&table) == CAPABILITY_OK,
          "add/get fixture initializes table");
    CHECK(capability_table_add(&table, &capability_a, &handle_a) ==
              CAPABILITY_OK &&
              handle_a != PROCESS_CAPABILITY_HANDLE_INVALID,
          "first capability add publishes a handle");
    CHECK(capability_table_add(&table, &capability_b, &handle_b) ==
              CAPABILITY_OK &&
              handle_b != handle_a,
          "second capability add publishes a distinct handle");
    CHECK(capability_table_open_count(&table) == 2U,
          "two adds publish two handles");
    CHECK(capability_table_get(&table, handle_a, &output) ==
              CAPABILITY_OK &&
              output.type == capability_a.type &&
              output.flags == capability_a.flags &&
              output.object == capability_a.object,
          "get returns first capability exactly");
    CHECK(capability_table_get(&table, handle_b, &output) ==
              CAPABILITY_OK &&
              output.type == capability_b.type &&
              output.flags == capability_b.flags &&
              output.object == capability_b.object,
          "get returns second capability exactly");

    CHECK(capability_table_add(&table, &capability_a, &duplicate) ==
              CAPABILITY_DUPLICATE &&
              duplicate == 0U,
          "exact duplicate add is rejected without publishing a handle");
    CHECK(capability_table_open_count(&table) == 2U &&
              capability_table_get(&table, handle_a, &output) ==
                  CAPABILITY_OK,
          "duplicate rejection leaves existing handles unchanged");

    CHECK(capability_table_close(&table, handle_a) == CAPABILITY_OK &&
              capability_table_open_count(&table) == 1U,
          "close removes a published handle");
    CHECK(capability_table_get(&table, handle_a, &output) ==
              CAPABILITY_BAD_HANDLE,
          "closed handle is no longer readable");
    CHECK(capability_table_revoke(&table, handle_b) == CAPABILITY_OK &&
              capability_table_open_count(&table) == 0U,
          "revoke removes a published handle");
    CHECK(capability_table_get(&table, handle_b, &output) ==
              CAPABILITY_BAD_HANDLE,
          "revoked handle is no longer readable");

    CHECK(capability_table_add(&table, &capability_a, &handle_a) ==
              CAPABILITY_OK &&
              capability_table_get(&table, handle_a, &output) ==
                  CAPABILITY_OK,
          "released capability can be republished");
    CHECK(capability_table_close(&table, handle_a) == CAPABILITY_OK,
          "republished capability closes cleanly");
}

static void test_pending_unpublished_handles(void) {
    struct capability capability = {3U, 0U, 0x3000U};
    struct capability output;
    uint64_t handle_a = 0U;
    uint64_t handle_b = 0U;

    table_reset();
    CHECK(capability_table_init(&table) == CAPABILITY_OK,
          "pending fixture initializes table");
    CHECK(capability_table_reserve(&table, &handle_a) == CAPABILITY_OK &&
              handle_a != PROCESS_CAPABILITY_HANDLE_INVALID,
          "first pending handle is reserved");
    CHECK(capability_table_reserve(&table, &handle_b) == CAPABILITY_OK &&
              handle_b != handle_a,
          "second pending handle is reserved");
    CHECK(capability_table_open_count(&table) == 0U,
          "pending handles do not count as published");

    CHECK(capability_table_get(&table, handle_a, &output) ==
              CAPABILITY_NOT_PUBLISHED,
          "pending handle is not readable");
    CHECK(capability_table_close(&table, handle_a) ==
              CAPABILITY_NOT_PUBLISHED,
          "pending handle cannot be closed");
    CHECK(capability_table_revoke(&table, handle_a) ==
              CAPABILITY_NOT_PUBLISHED,
          "pending handle cannot be revoked");

    CHECK(capability_table_commit(&table, handle_a, &capability) ==
              CAPABILITY_OK &&
              capability_table_open_count(&table) == 1U,
          "pending handle commits to published state");
    CHECK(capability_table_get(&table, handle_a, &output) ==
              CAPABILITY_OK &&
              output.object == capability.object,
          "committed handle becomes readable");
    CHECK(capability_table_rollback(&table, handle_a) ==
              CAPABILITY_BAD_STATE,
          "published handle cannot be rolled back");
    CHECK(capability_table_commit(&table, 0U, &capability) ==
              CAPABILITY_BAD_HANDLE,
          "invalid handle commit is rejected");
    CHECK(capability_table_commit(&table, handle_a, NULL) ==
              CAPABILITY_NULL,
          "commit rejects null capability");

    CHECK(capability_table_rollback(&table, handle_b) == CAPABILITY_OK &&
              capability_table_open_count(&table) == 1U,
          "pending handle rolls back without changing open count");
    CHECK(capability_table_rollback(&table, handle_b) ==
              CAPABILITY_BAD_HANDLE,
          "rolled-back handle cannot roll back again");
    CHECK(capability_table_get(&table, handle_b, &output) ==
              CAPABILITY_BAD_HANDLE,
          "rolled-back handle is not readable");

    CHECK(capability_table_close(&table, handle_a) == CAPABILITY_OK &&
              capability_table_open_count(&table) == 0U,
          "committed fixture closes cleanly");
}

static void test_exhaustion_and_reuse(void) {
    uint64_t handles[PROCESS_CAPABILITY_CAPACITY];
    uint64_t extra = 0U;
    uint64_t reused = 0U;
    struct capability capability;
    struct capability output;
    unsigned int index;

    table_reset();
    CHECK(capability_table_init(&table) == CAPABILITY_OK,
          "exhaustion fixture initializes table");
    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        capability.type = index;
        capability.flags = 0U;
        capability.object = index;
        CHECK(capability_table_add(&table, &capability, &handles[index]) ==
                  CAPABILITY_OK,
              "capability exhaustion fill add succeeds");
    }
    CHECK(capability_table_open_count(&table) ==
              PROCESS_CAPABILITY_CAPACITY,
          "capability table reaches exact capacity");

    capability.type = 0xFFFFU;
    capability.flags = 0U;
    capability.object = 0xFFFFU;
    CHECK(capability_table_add(&table, &capability, &extra) ==
              CAPABILITY_FULL &&
              extra == 0U &&
              capability_table_open_count(&table) ==
                  PROCESS_CAPABILITY_CAPACITY,
          "full capability table rejects add without mutation");

    for (index = 0U; index < PROCESS_CAPABILITY_CAPACITY; ++index) {
        CHECK(capability_table_close(&table, handles[index]) ==
                  CAPABILITY_OK,
              "capability exhaustion cleanup close succeeds");
    }
    CHECK(capability_table_open_count(&table) == 0U,
          "capability exhaustion cleanup empties table");

    capability.type = 1U;
    capability.flags = 0U;
    capability.object = 1U;
    CHECK(capability_table_add(&table, &capability, &reused) ==
              CAPABILITY_OK &&
              capability_table_get(&table, reused, &output) ==
                  CAPABILITY_OK,
          "released slots are reusable");
    CHECK(capability_table_close(&table, reused) == CAPABILITY_OK,
          "reused capability closes cleanly");
}

static void test_reverse_order_rollback(void) {
    uint64_t handle_a = 0U;
    uint64_t handle_b = 0U;
    uint64_t handle_c = 0U;
    struct capability capability = {4U, 8U, 0x4000U};
    struct capability output;

    table_reset();
    CHECK(capability_table_init(&table) == CAPABILITY_OK,
          "rollback fixture initializes table");
    CHECK(capability_table_reserve(&table, &handle_a) == CAPABILITY_OK,
          "rollback fixture reserves first handle");
    CHECK(capability_table_reserve(&table, &handle_b) == CAPABILITY_OK,
          "rollback fixture reserves second handle");
    CHECK(capability_table_rollback(&table, handle_b) == CAPABILITY_OK &&
              capability_table_rollback(&table, handle_a) == CAPABILITY_OK &&
              capability_table_open_count(&table) == 0U,
          "pending handles roll back in reverse order");
    CHECK(capability_table_get(&table, handle_a, &output) ==
              CAPABILITY_BAD_HANDLE &&
              capability_table_get(&table, handle_b, &output) ==
                  CAPABILITY_BAD_HANDLE,
          "rolled-back handles are invalid");

    CHECK(capability_table_reserve(&table, &handle_c) == CAPABILITY_OK &&
              handle_c != handle_a && handle_c != handle_b,
          "rollback does not reuse a stale handle");
    CHECK(capability_table_rollback(&table, handle_c) == CAPABILITY_OK,
          "rollback fixture cleans final pending handle");

    CHECK(capability_table_add(&table, &capability, &handle_a) ==
              CAPABILITY_OK &&
              capability_table_add(&table, &capability, &handle_b) ==
                  CAPABILITY_DUPLICATE,
          "add duplicate is rejected after a successful add");
    CHECK(capability_table_open_count(&table) == 1U &&
              capability_table_get(&table, handle_a, &output) ==
                  CAPABILITY_OK,
          "duplicate add leaves previous capability published");
    CHECK(capability_table_close(&table, handle_a) == CAPABILITY_OK,
          "rollback fixture closes published capability");
}

static void test_exact_type_presence(void) {
    struct capability capability_a = {
        CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0x1000U
    };
    uint64_t handle_a = 0U;
    uint64_t pending = 0U;

    table_reset();
    CHECK(!capability_table_has(NULL, CAPABILITY_TYPE_SYSCALL_WRITE),
          "null table has no capability type");
    CHECK(!capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_WRITE),
          "unpublished table has no capability type");
    CHECK(capability_table_init(&table) == CAPABILITY_OK,
          "type presence fixture initializes table");
    CHECK(!capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_WRITE) &&
              !capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_YIELD) &&
              !capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_EXIT),
          "empty table has no capability type");

    CHECK(capability_table_add(&table, &capability_a, &handle_a) ==
              CAPABILITY_OK,
          "type presence fixture publishes write capability");
    CHECK(capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_WRITE),
          "published write capability is present by exact type");
    CHECK(!capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_YIELD),
          "different type is not present");

    CHECK(capability_table_reserve(&table, &pending) == CAPABILITY_OK,
          "type presence fixture reserves pending yield handle");
    CHECK(!capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_YIELD),
          "pending handle does not authorize its type");
    CHECK(capability_table_rollback(&table, pending) == CAPABILITY_OK,
          "type presence fixture rolls back pending handle");
    CHECK(capability_table_close(&table, handle_a) == CAPABILITY_OK,
          "type presence fixture closes write capability");
    CHECK(!capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_WRITE),
          "closed capability is no longer present");
    CHECK(capability_table_destroy(&table) == CAPABILITY_OK &&
              !capability_table_has(&table, CAPABILITY_TYPE_SYSCALL_WRITE),
          "destroyed table has no capability type");
}

int main(void) {
    test_validation_and_unpublished_table();
    test_add_get_close_revoke_duplicate();
    test_pending_unpublished_handles();
    test_exhaustion_and_reuse();
    test_reverse_order_rollback();
    test_exact_type_presence();

    if (failures != 0) {
        fprintf(stderr, "Capability host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Capability host tests: ALL OK");
    return 0;
}

#endif
