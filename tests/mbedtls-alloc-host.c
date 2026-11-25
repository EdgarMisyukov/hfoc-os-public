#include "hfoc_mbedtls_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool init_allocator(void)
{
    hfoc_mbedtls_alloc_shutdown();
    return hfoc_mbedtls_alloc_init();
}

static bool pool_is_initial(
    const struct hfoc_mbedtls_alloc_snapshot *snapshot,
    unsigned int pool_class,
    uint32_t slot_size,
    uint32_t slot_count)
{
    return snapshot != NULL &&
           pool_class < HFOC_MBEDTLS_POOL_COUNT &&
           snapshot->pools[pool_class].slot_size == slot_size &&
           snapshot->pools[pool_class].slot_count == slot_count &&
           snapshot->pools[pool_class].free_count == slot_count &&
           snapshot->pools[pool_class].used_count == 0U &&
           snapshot->pools[pool_class].high_water_count == 0U;
}

static bool connection_is_initial(
    const struct hfoc_mbedtls_alloc_snapshot *snapshot,
    uint32_t id)
{
    const struct hfoc_mbedtls_connection_snapshot *connection;

    if (snapshot == NULL || id == 0U ||
        id > HFOC_MBEDTLS_CONNECTION_COUNT) {
        return false;
    }
    connection = &snapshot->connections[id - 1U];
    return !connection->active &&
           connection->id == 0U &&
           connection->allocation_count == 0U &&
           connection->used_bytes == 0U &&
           connection->budget_bytes == HFOC_MBEDTLS_CONNECTION_BUDGET_BYTES;
}

static void test_unpublished_fail_closed(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    unsigned int pool_class;

    hfoc_mbedtls_alloc_shutdown();
    CHECK(!hfoc_mbedtls_alloc_ready(),
          "unpublished allocator is not ready");
    CHECK(hfoc_mbedtls_alloc(1U) == NULL,
          "unpublished allocator rejects allocation");
    CHECK(hfoc_mbedtls_calloc(1U, 1U) == NULL,
          "unpublished allocator rejects calloc");
    CHECK(!hfoc_mbedtls_alloc_free(NULL),
          "unpublished allocator rejects free");
    CHECK(!hfoc_mbedtls_alloc_connection_open(NULL),
          "unpublished allocator rejects connection open");

    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.version == 0U &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes == 0U &&
              snapshot.total_high_water_bytes == 0U,
          "unpublished allocator snapshot is empty");
    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        CHECK(snapshot.pools[pool_class].slot_size == 0U &&
                  snapshot.pools[pool_class].slot_count == 0U &&
                  snapshot.pools[pool_class].free_count == 0U &&
                  snapshot.pools[pool_class].used_count == 0U &&
                  snapshot.pools[pool_class].high_water_count == 0U,
              "unpublished allocator pool snapshot is empty");
        CHECK(hfoc_mbedtls_alloc_free_count(pool_class) == 0U &&
                  hfoc_mbedtls_alloc_used_count(pool_class) == 0U &&
                  hfoc_mbedtls_alloc_high_water_count(pool_class) == 0U,
              "unpublished allocator per-class accessors are empty");
    }
}

static void test_init_publishes_exact_pools(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    uint32_t id;

    CHECK(init_allocator(), "allocator init publishes fixed pools");
    CHECK(!hfoc_mbedtls_alloc_init(),
          "duplicate allocator init is rejected");
    CHECK(hfoc_mbedtls_alloc_ready(),
          "published allocator is ready");

    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.version == HFOC_MBEDTLS_ALLOC_VERSION &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes ==
                  HFOC_MBEDTLS_ALLOC_TOTAL_BYTES &&
              snapshot.total_high_water_bytes == 0U,
          "published allocator snapshot has exact initial totals");
    CHECK(pool_is_initial(
              &snapshot, HFOC_MBEDTLS_POOL_CLASS_TINY,
              HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY,
              HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY),
          "tiny pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_MBEDTLS_POOL_CLASS_SMALL,
              HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL,
              HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL),
          "small pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_MBEDTLS_POOL_CLASS_MEDIUM,
              HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM,
              HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM),
          "medium pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_MBEDTLS_POOL_CLASS_LARGE,
              HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE,
              HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE),
          "large pool publishes exact initial counts");
    CHECK(connection_is_initial(&snapshot, 1U) &&
              connection_is_initial(&snapshot, 2U) &&
              connection_is_initial(&snapshot, 3U) &&
              connection_is_initial(&snapshot, 4U),
          "all connection snapshots publish exact initial budgets");

    CHECK(hfoc_mbedtls_alloc_connection_open(&id) &&
              id == 1U,
          "first connection opens with id 1");
    CHECK(hfoc_mbedtls_alloc_connection_close(id),
          "opened connection closes");
}

static void test_size_class_boundaries(void)
{
    void *tiny;
    void *small;
    void *medium;
    void *large;

    CHECK(init_allocator(), "size boundary fixture initializes allocator");
    tiny = hfoc_mbedtls_alloc(1U);
    CHECK(tiny != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_TINY) == 1U,
          "one-byte allocation uses tiny pool");
    CHECK(hfoc_mbedtls_alloc(64U) != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_TINY) == 2U,
          "64-byte allocation uses tiny pool");

    small = hfoc_mbedtls_alloc(65U);
    CHECK(small != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_SMALL) == 1U,
          "65-byte allocation uses small pool");
    CHECK(hfoc_mbedtls_alloc(256U) != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_SMALL) == 2U,
          "256-byte allocation uses small pool");

    medium = hfoc_mbedtls_alloc(257U);
    CHECK(medium != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_MEDIUM) == 1U,
          "257-byte allocation uses medium pool");
    CHECK(hfoc_mbedtls_alloc(1024U) != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_MEDIUM) == 2U,
          "1024-byte allocation uses medium pool");

    large = hfoc_mbedtls_alloc(1025U);
    CHECK(large != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_LARGE) == 1U,
          "1025-byte allocation uses large pool");
    CHECK(hfoc_mbedtls_alloc(4096U) != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_LARGE) == 2U,
          "4096-byte allocation uses large pool");

    CHECK(hfoc_mbedtls_alloc(0U) == NULL &&
              hfoc_mbedtls_alloc(4097U) == NULL,
          "zero and over-maximum requests are rejected");
}

static void test_calloc_validation_and_zeroing(void)
{
    uint8_t *zeroed;
    size_t index;

    CHECK(init_allocator(), "calloc fixture initializes allocator");
    CHECK(hfoc_mbedtls_calloc(0U, 1U) == NULL &&
              hfoc_mbedtls_calloc(1U, 0U) == NULL,
          "calloc rejects zero count or size");
    CHECK(hfoc_mbedtls_calloc(SIZE_MAX, 2U) == NULL,
          "calloc rejects multiplication overflow");
    CHECK(hfoc_mbedtls_alloc_used_count(
              HFOC_MBEDTLS_POOL_CLASS_TINY) == 0U,
          "calloc rejection does not mutate counters");

    zeroed = hfoc_mbedtls_calloc(4U, 16U);
    CHECK(zeroed != NULL &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_TINY) == 1U,
          "valid calloc allocates one tiny slot");
    for (index = 0U; index < 64U; ++index) {
        if (zeroed[index] != 0U) {
            break;
        }
    }
    CHECK(index == 64U,
          "valid calloc returns zeroed memory");
    CHECK(hfoc_mbedtls_alloc_free(zeroed),
          "calloc result is freed");
}

static void test_free_validation(void)
{
    void *pointer;

    CHECK(init_allocator(), "free validation fixture initializes allocator");
    pointer = hfoc_mbedtls_alloc(64U);
    CHECK(pointer != NULL, "free validation allocates one slot");
    CHECK(!hfoc_mbedtls_alloc_free(NULL),
          "null free is rejected");
    CHECK(!hfoc_mbedtls_alloc_free((uint8_t *)pointer + 1U),
          "misaligned free is rejected");
    CHECK(!hfoc_mbedtls_alloc_free((void *)(uintptr_t)0x1000U),
          "foreign free is rejected");
    CHECK(hfoc_mbedtls_alloc_free(pointer),
          "valid free succeeds");
    CHECK(!hfoc_mbedtls_alloc_free(pointer),
          "double free is rejected");
    CHECK(hfoc_mbedtls_alloc_free_count(
              HFOC_MBEDTLS_POOL_CLASS_TINY) ==
              HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_TINY) == 0U,
          "free validation leaves exact counts");
}

static void test_zeroization_class(
    unsigned int pool_class,
    size_t size,
    uint32_t count)
{
    void *slots[HFOC_MBEDTLS_POOL_SLOT_COUNT_MAX];
    uint32_t index;
    uint8_t *reused;
    size_t byte_index;
    bool zeroed = true;

    CHECK(init_allocator(), "zeroization fixture initializes allocator");
    for (index = 0U; index < count; ++index) {
        slots[index] = hfoc_mbedtls_alloc(size);
        if (slots[index] != NULL) {
            memset(slots[index], 0xA5, size);
        }
    }
    CHECK(hfoc_mbedtls_alloc_used_count(pool_class) == count,
          "zeroization fixture fills exact class slots");
    for (index = 0U; index < count; ++index) {
        CHECK(hfoc_mbedtls_alloc_free(slots[index]),
              "zeroization fixture releases every slot");
    }
    reused = hfoc_mbedtls_alloc(size);
    CHECK(reused != NULL,
          "zeroization fixture reuses one released slot");
    for (byte_index = 0U; byte_index < size; ++byte_index) {
        if (reused[byte_index] != 0U) {
            zeroed = false;
            break;
        }
    }
    CHECK(zeroed,
          "released slot is zeroized before reuse");
    CHECK(hfoc_mbedtls_alloc_free(reused),
          "zeroization fixture releases reused slot");
}

static void test_zeroization_classes(void)
{
    test_zeroization_class(
        HFOC_MBEDTLS_POOL_CLASS_TINY,
        HFOC_MBEDTLS_POOL_SLOT_SIZE_TINY,
        HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY);
    test_zeroization_class(
        HFOC_MBEDTLS_POOL_CLASS_SMALL,
        HFOC_MBEDTLS_POOL_SLOT_SIZE_SMALL,
        HFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL);
    test_zeroization_class(
        HFOC_MBEDTLS_POOL_CLASS_MEDIUM,
        HFOC_MBEDTLS_POOL_SLOT_SIZE_MEDIUM,
        HFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM);
    test_zeroization_class(
        HFOC_MBEDTLS_POOL_CLASS_LARGE,
        HFOC_MBEDTLS_POOL_SLOT_SIZE_LARGE,
        HFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE);
}

static void test_connection_budget_exhaustion_closes_handshake(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    uint32_t id;
    void *context;
    void *handshake;
    void *ecp;
    void *certificate;
    void *extra_large;

    CHECK(init_allocator(), "handshake fixture initializes allocator");
    CHECK(hfoc_mbedtls_alloc_connection_open(&id) && id == 1U,
          "handshake fixture opens connection 1");
    CHECK(hfoc_mbedtls_alloc_connection_enter(id),
          "handshake fixture enters connection 1");

    context = hfoc_mbedtls_calloc(1U, 64U);
    handshake = hfoc_mbedtls_calloc(1U, 256U);
    ecp = hfoc_mbedtls_calloc(1U, 1024U);
    certificate = hfoc_mbedtls_calloc(1U, 4096U);
    CHECK(context != NULL && handshake != NULL && ecp != NULL &&
              certificate != NULL,
          "one handshake allocation from every class succeeds");
    CHECK(hfoc_mbedtls_alloc_used_count(
              HFOC_MBEDTLS_POOL_CLASS_LARGE) == 1U,
          "handshake uses one large physical slot");

    extra_large = hfoc_mbedtls_calloc(1U, 4096U);
    CHECK(extra_large == NULL,
          "second large allocation exhausts per-connection budget");
    CHECK(hfoc_mbedtls_alloc_used_count(
              HFOC_MBEDTLS_POOL_CLASS_LARGE) == 1U,
          "budget exhaustion does not mutate physical counts");

    CHECK(hfoc_mbedtls_alloc_free(context) &&
              hfoc_mbedtls_alloc_free(handshake) &&
              hfoc_mbedtls_alloc_free(ecp) &&
              hfoc_mbedtls_alloc_free(certificate),
          "failed handshake rollback releases every allocation");
    CHECK(hfoc_mbedtls_alloc_connection_exit(),
          "failed handshake exits connection scope");
    CHECK(hfoc_mbedtls_alloc_connection_close(id),
          "failed handshake closes connection");

    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes ==
                  HFOC_MBEDTLS_ALLOC_TOTAL_BYTES &&
              connection_is_initial(&snapshot, id),
          "handshake teardown restores exact initial counts");
}

static void test_connection_isolation(void)
{
    uint32_t first;
    uint32_t second;
    void *first_large;
    void *second_large;

    CHECK(init_allocator(), "isolation fixture initializes allocator");
    CHECK(hfoc_mbedtls_alloc_connection_open(&first) &&
              first == 1U,
          "isolation fixture opens first connection");
    CHECK(hfoc_mbedtls_alloc_connection_enter(first),
          "isolation fixture enters first connection");
    first_large = hfoc_mbedtls_calloc(1U, 4096U);
    CHECK(first_large != NULL,
          "first connection consumes its large budget");
    CHECK(hfoc_mbedtls_alloc_connection_exit(),
          "isolation fixture exits first connection");

    CHECK(hfoc_mbedtls_alloc_connection_open(&second) &&
              second == 2U,
          "isolation fixture opens second connection");
    CHECK(hfoc_mbedtls_alloc_connection_enter(second),
          "isolation fixture enters second connection");
    second_large = hfoc_mbedtls_calloc(1U, 4096U);
    CHECK(second_large != NULL,
          "second connection still receives its large budget");

    CHECK(hfoc_mbedtls_alloc_free(first_large),
          "isolation fixture frees first large allocation");
    CHECK(hfoc_mbedtls_alloc_free(second_large),
          "isolation fixture frees second large allocation");
    CHECK(hfoc_mbedtls_alloc_connection_exit() &&
              hfoc_mbedtls_alloc_connection_close(first) &&
              hfoc_mbedtls_alloc_connection_close(second),
          "isolation fixture closes both connections");
}

static void test_connections_share_pool_concurrently(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    uint32_t first;
    uint32_t second;
    void *first_tiny;
    void *second_tiny;
    void *first_second;

    CHECK(init_allocator(), "concurrent fixture initializes allocator");
    CHECK(hfoc_mbedtls_alloc_connection_open(&first) && first == 1U,
          "concurrent fixture opens first connection");
    CHECK(hfoc_mbedtls_alloc_connection_enter(first),
          "concurrent fixture enters first connection");
    first_tiny = hfoc_mbedtls_calloc(1U, 64U);
    CHECK(first_tiny != NULL,
          "concurrent fixture allocates under first connection");

    CHECK(hfoc_mbedtls_alloc_connection_open(&second) && second == 2U,
          "concurrent fixture opens second connection while first is live");
    CHECK(hfoc_mbedtls_alloc_connection_enter(second),
          "a second serving handshake enters its own scope");
    second_tiny = hfoc_mbedtls_calloc(1U, 64U);
    CHECK(second_tiny != NULL,
          "second connection receives its own allocation");

    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.connections[first - 1U].used_bytes == 64U &&
              snapshot.connections[second - 1U].used_bytes == 64U,
          "interleaved allocations are charged to their own connection");

    CHECK(hfoc_mbedtls_alloc_connection_exit(),
          "second connection leaves its scope");
    first_second = hfoc_mbedtls_calloc(1U, 64U);
    CHECK(first_second != NULL,
          "the first connection keeps its scope after the second exits");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.connections[first - 1U].used_bytes == 128U &&
              snapshot.connections[second - 1U].used_bytes == 64U,
          "the exited second connection receives no later allocation");

    CHECK(hfoc_mbedtls_alloc_free(first_tiny) &&
              hfoc_mbedtls_alloc_free(second_tiny) &&
              hfoc_mbedtls_alloc_free(first_second),
          "concurrent fixture frees every allocation");
    CHECK(hfoc_mbedtls_alloc_connection_exit(),
          "first connection leaves its scope");
    CHECK(hfoc_mbedtls_alloc_connection_close(first) &&
              hfoc_mbedtls_alloc_connection_close(second),
          "concurrent fixture closes both connections");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.total_used_bytes == 0U &&
              connection_is_initial(&snapshot, first) &&
              connection_is_initial(&snapshot, second),
          "concurrent teardown restores exact initial counts");
}

static void test_connection_close_zeroizes_live_allocations(void)
{
    uint32_t id;
    void *live;
    uint8_t *reused;
    size_t index;
    bool zeroed = true;

    CHECK(init_allocator(), "connection teardown fixture initializes");
    CHECK(hfoc_mbedtls_alloc_connection_open(&id) && id == 1U,
          "connection teardown fixture opens connection");
    CHECK(hfoc_mbedtls_alloc_connection_enter(id),
          "connection teardown fixture enters connection");
    live = hfoc_mbedtls_alloc(64U);
    CHECK(live != NULL,
          "connection teardown fixture allocates live slot");
    memset(live, 0xA5, 64U);
    CHECK(hfoc_mbedtls_alloc_connection_exit(),
          "connection teardown fixture exits before close");
    CHECK(hfoc_mbedtls_alloc_connection_close(id),
          "connection teardown closes with live allocation");

    reused = hfoc_mbedtls_alloc(64U);
    CHECK(reused != NULL,
          "connection teardown frees live slot for reuse");
    for (index = 0U; index < 64U; ++index) {
        if (reused[index] != 0U) {
            zeroed = false;
            break;
        }
    }
    CHECK(zeroed,
          "connection close zeroizes live allocation");
    CHECK(hfoc_mbedtls_alloc_free(reused),
          "connection teardown fixture releases reused slot");
}

static void test_shutdown_restores_initial_counts(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    unsigned int pool_class;

    CHECK(init_allocator(), "shutdown fixture initializes allocator");
    CHECK(hfoc_mbedtls_alloc(1024U) != NULL &&
              hfoc_mbedtls_alloc(4096U) != NULL,
          "shutdown fixture allocates live slots");

    hfoc_mbedtls_alloc_shutdown();
    CHECK(!hfoc_mbedtls_alloc_ready(),
          "shutdown makes allocator unpublished");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.version == 0U &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes == 0U &&
              snapshot.total_high_water_bytes == 0U,
          "shutdown clears aggregate accounting");
    for (pool_class = 0U; pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        CHECK(hfoc_mbedtls_alloc_free_count(pool_class) == 0U &&
                  hfoc_mbedtls_alloc_used_count(pool_class) == 0U &&
                  hfoc_mbedtls_alloc_high_water_count(pool_class) == 0U,
              "shutdown clears per-class accounting");
    }

    CHECK(init_allocator(), "allocator re-initializes after shutdown");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes ==
                  HFOC_MBEDTLS_ALLOC_TOTAL_BYTES &&
              snapshot.total_high_water_bytes == 0U,
          "re-init restores exact initial counts");
}

static void test_mbedtls_adapter_wrappers(void)
{
    uint8_t *zeroed;
    size_t index;

    CHECK(init_allocator(), "adapter fixture initializes allocator");
    zeroed = hfoc_mbedtls_calloc(4U, 8U);
    CHECK(zeroed != NULL &&
              zeroed[0] == 0U && zeroed[31] == 0U,
          "mbedTLS calloc wrapper returns zeroed memory");
    hfoc_mbedtls_free(zeroed);
    CHECK(hfoc_mbedtls_alloc_free_count(
              HFOC_MBEDTLS_POOL_CLASS_TINY) ==
              HFOC_MBEDTLS_POOL_SLOT_COUNT_TINY &&
              hfoc_mbedtls_alloc_used_count(
                  HFOC_MBEDTLS_POOL_CLASS_TINY) == 0U,
          "mbedTLS free wrapper restores exact counts");
    for (index = 0U; index < 4U; ++index) {
        hfoc_mbedtls_free(NULL);
    }
}

int main(void)
{
    test_unpublished_fail_closed();
    test_init_publishes_exact_pools();
    test_size_class_boundaries();
    test_calloc_validation_and_zeroing();
    test_free_validation();
    test_zeroization_classes();
    test_connection_budget_exhaustion_closes_handshake();
    test_connection_isolation();
    test_connections_share_pool_concurrently();
    test_connection_close_zeroizes_live_allocations();
    test_shutdown_restores_initial_counts();
    test_mbedtls_adapter_wrappers();

    if (failures == 0) {
        printf("mbedTLS allocator boundary host tests: ALL OK\n");
    }
    return failures != 0;
}
