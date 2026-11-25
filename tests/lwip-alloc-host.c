#include "hfoc_lwip.h"

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
    hfoc_lwip_alloc_shutdown();
    return hfoc_lwip_alloc_init();
}

static bool pool_is_initial(
    const struct hfoc_lwip_alloc_snapshot *snapshot,
    unsigned int pool_class,
    uint32_t slot_size,
    uint32_t slot_count)
{
    return snapshot != NULL &&
           pool_class < HFOC_LWIP_POOL_COUNT &&
           snapshot->pools[pool_class].slot_size == slot_size &&
           snapshot->pools[pool_class].slot_count == slot_count &&
           snapshot->pools[pool_class].free_count == slot_count &&
           snapshot->pools[pool_class].used_count == 0U &&
           snapshot->pools[pool_class].high_water_count == 0U;
}

static void test_unpublished_fail_closed(void)
{
    struct hfoc_lwip_alloc_snapshot snapshot;
    unsigned int pool_class;

    hfoc_lwip_alloc_shutdown();
    CHECK(!hfoc_lwip_alloc_ready(),
          "unpublished allocator is not ready");
    CHECK(hfoc_lwip_alloc(1U) == NULL,
          "unpublished allocator rejects allocation");
    CHECK(hfoc_lwip_alloc_calloc(1U, 1U) == NULL,
          "unpublished allocator rejects calloc");
    CHECK(!hfoc_lwip_alloc_free(NULL),
          "unpublished allocator rejects free");

    snapshot = hfoc_lwip_alloc_snapshot();
    CHECK(snapshot.version == 0U &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes == 0U &&
              snapshot.total_high_water_bytes == 0U,
          "unpublished allocator snapshot is empty");
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        CHECK(snapshot.pools[pool_class].slot_size == 0U &&
                  snapshot.pools[pool_class].slot_count == 0U &&
                  snapshot.pools[pool_class].free_count == 0U &&
                  snapshot.pools[pool_class].used_count == 0U &&
                  snapshot.pools[pool_class].high_water_count == 0U,
              "unpublished allocator pool snapshot is empty");
        CHECK(hfoc_lwip_alloc_free_count(pool_class) == 0U &&
                  hfoc_lwip_alloc_used_count(pool_class) == 0U &&
                  hfoc_lwip_alloc_high_water_count(pool_class) == 0U,
              "unpublished allocator per-class accessors are empty");
    }
}

static void test_init_publishes_exact_pools(void)
{
    struct hfoc_lwip_alloc_snapshot snapshot;

    CHECK(init_allocator(), "allocator init publishes fixed pools");
    CHECK(!hfoc_lwip_alloc_init(),
          "duplicate allocator init is rejected");
    CHECK(hfoc_lwip_alloc_ready(),
          "published allocator is ready");

    snapshot = hfoc_lwip_alloc_snapshot();
    CHECK(snapshot.version == HFOC_LWIP_ALLOC_VERSION &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes ==
                  HFOC_LWIP_ALLOC_TOTAL_BYTES &&
              snapshot.total_high_water_bytes == 0U,
          "published allocator snapshot has exact initial totals");
    CHECK(pool_is_initial(
              &snapshot, HFOC_LWIP_POOL_CLASS_TINY,
              HFOC_LWIP_POOL_SLOT_SIZE_TINY,
              HFOC_LWIP_POOL_SLOT_COUNT_TINY),
          "tiny pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_LWIP_POOL_CLASS_SMALL,
              HFOC_LWIP_POOL_SLOT_SIZE_SMALL,
              HFOC_LWIP_POOL_SLOT_COUNT_SMALL),
          "small pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_LWIP_POOL_CLASS_MEDIUM,
              HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM,
              HFOC_LWIP_POOL_SLOT_COUNT_MEDIUM),
          "medium pool publishes exact initial counts");
    CHECK(pool_is_initial(
              &snapshot, HFOC_LWIP_POOL_CLASS_LARGE,
              HFOC_LWIP_POOL_SLOT_SIZE_LARGE,
              HFOC_LWIP_POOL_SLOT_COUNT_LARGE),
          "large pool publishes exact initial counts");
}

static void test_size_class_boundaries(void)
{
    void *tiny;
    void *small;
    void *medium;
    void *large;

    CHECK(init_allocator(), "size boundary fixture initializes allocator");
    tiny = hfoc_lwip_alloc(1U);
    CHECK(tiny != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 1U,
          "one-byte allocation uses tiny pool");
    CHECK(hfoc_lwip_alloc(64U) != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 2U,
          "64-byte allocation uses tiny pool");

    small = hfoc_lwip_alloc(65U);
    CHECK(small != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_SMALL) == 1U,
          "65-byte allocation uses small pool");
    CHECK(hfoc_lwip_alloc(256U) != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_SMALL) == 2U,
          "256-byte allocation uses small pool");

    medium = hfoc_lwip_alloc(257U);
    CHECK(medium != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_MEDIUM) == 1U,
          "257-byte allocation uses medium pool");
    CHECK(hfoc_lwip_alloc(1024U) != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_MEDIUM) == 2U,
          "1024-byte allocation uses medium pool");

    large = hfoc_lwip_alloc(1025U);
    CHECK(large != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_LARGE) == 1U,
          "1025-byte allocation uses large pool");
    CHECK(hfoc_lwip_alloc(4096U) != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_LARGE) == 2U,
          "4096-byte allocation uses large pool");

    CHECK(hfoc_lwip_alloc(0U) == NULL &&
              hfoc_lwip_alloc(4097U) == NULL,
          "zero and over-maximum requests are rejected");
}

static void test_exhaustion_and_reuse(void)
{
    void *slots[HFOC_LWIP_POOL_SLOT_COUNT_TINY];
    size_t index;
    void *replacement;

    CHECK(init_allocator(), "exhaustion fixture initializes allocator");
    for (index = 0U; index < HFOC_LWIP_POOL_SLOT_COUNT_TINY; ++index) {
        slots[index] = hfoc_lwip_alloc(64U);
        CHECK(slots[index] != NULL,
              "tiny pool allocation succeeds until exhaustion");
    }
    CHECK(hfoc_lwip_alloc(64U) == NULL,
          "tiny pool exhaustion returns NULL");
    CHECK(hfoc_lwip_alloc_free_count(
              HFOC_LWIP_POOL_CLASS_TINY) == 0U &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) ==
                  HFOC_LWIP_POOL_SLOT_COUNT_TINY &&
              hfoc_lwip_alloc_high_water_count(
                  HFOC_LWIP_POOL_CLASS_TINY) ==
                  HFOC_LWIP_POOL_SLOT_COUNT_TINY,
          "tiny pool exhaustion records exact counters");

    CHECK(hfoc_lwip_alloc_free(slots[0]),
          "freed tiny slot returns to allocator");
    CHECK(hfoc_lwip_alloc_free_count(
              HFOC_LWIP_POOL_CLASS_TINY) == 1U,
          "free updates tiny pool free count");
    replacement = hfoc_lwip_alloc(64U);
    CHECK(replacement != NULL,
          "freed tiny slot is reusable");
    CHECK(hfoc_lwip_alloc_free(replacement),
          "replacement tiny slot is freed");
}

static void test_calloc_validation_and_zeroing(void)
{
    uint8_t *zeroed;
    size_t index;

    CHECK(init_allocator(), "calloc fixture initializes allocator");
    CHECK(hfoc_lwip_alloc_calloc(0U, 1U) == NULL &&
              hfoc_lwip_alloc_calloc(1U, 0U) == NULL,
          "calloc rejects zero count or size");
    CHECK(hfoc_lwip_alloc_calloc(SIZE_MAX, 2U) == NULL,
          "calloc rejects multiplication overflow");
    CHECK(hfoc_lwip_alloc_used_count(
              HFOC_LWIP_POOL_CLASS_TINY) == 0U,
          "calloc rejection does not mutate counters");

    zeroed = hfoc_lwip_alloc_calloc(4U, 16U);
    CHECK(zeroed != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 1U,
          "valid calloc allocates one tiny slot");
    for (index = 0U; index < 64U; ++index) {
        if (zeroed[index] != 0U) {
            break;
        }
    }
    CHECK(index == 64U,
          "valid calloc returns zeroed memory");
    CHECK(hfoc_lwip_alloc_free(zeroed),
          "calloc result is freed");
}

static void test_free_validation(void)
{
    void *pointer;

    CHECK(init_allocator(), "free validation fixture initializes allocator");
    pointer = hfoc_lwip_alloc(64U);
    CHECK(pointer != NULL, "free validation allocates one slot");
    CHECK(!hfoc_lwip_alloc_free(NULL),
          "null free is rejected");
    CHECK(!hfoc_lwip_alloc_free((uint8_t *)pointer + 1U),
          "misaligned free is rejected");
    CHECK(!hfoc_lwip_alloc_free((void *)(uintptr_t)0x1000U),
          "foreign free is rejected");
    CHECK(hfoc_lwip_alloc_free(pointer),
          "valid free succeeds");
    CHECK(!hfoc_lwip_alloc_free(pointer),
          "double free is rejected");
    CHECK(hfoc_lwip_alloc_free_count(
              HFOC_LWIP_POOL_CLASS_TINY) ==
              HFOC_LWIP_POOL_SLOT_COUNT_TINY &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 0U,
          "free validation leaves exact counts");
}

static void test_shutdown_restores_initial_counts(void)
{
    struct hfoc_lwip_alloc_snapshot snapshot;
    unsigned int pool_class;

    CHECK(init_allocator(), "shutdown fixture initializes allocator");
    CHECK(hfoc_lwip_alloc(1024U) != NULL &&
              hfoc_lwip_alloc(4096U) != NULL,
          "shutdown fixture allocates live slots");
    CHECK(hfoc_lwip_alloc_used_count(
              HFOC_LWIP_POOL_CLASS_MEDIUM) == 1U &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_LARGE) == 1U,
          "shutdown fixture has nonzero used counts");

    hfoc_lwip_alloc_shutdown();
    CHECK(!hfoc_lwip_alloc_ready(),
          "shutdown makes allocator unpublished");
    snapshot = hfoc_lwip_alloc_snapshot();
    CHECK(snapshot.version == 0U &&
              snapshot.total_used_bytes == 0U &&
              snapshot.total_free_bytes == 0U &&
              snapshot.total_high_water_bytes == 0U,
          "shutdown clears aggregate accounting");
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        CHECK(hfoc_lwip_alloc_free_count(pool_class) == 0U &&
                  hfoc_lwip_alloc_used_count(pool_class) == 0U &&
                  hfoc_lwip_alloc_high_water_count(pool_class) == 0U,
              "shutdown clears per-class accounting");
    }

    CHECK(init_allocator(), "allocator re-initializes after shutdown");
    snapshot = hfoc_lwip_alloc_snapshot();
    CHECK(pool_is_initial(
              &snapshot, HFOC_LWIP_POOL_CLASS_TINY,
              HFOC_LWIP_POOL_SLOT_SIZE_TINY,
              HFOC_LWIP_POOL_SLOT_COUNT_TINY) &&
              pool_is_initial(
                  &snapshot, HFOC_LWIP_POOL_CLASS_SMALL,
                  HFOC_LWIP_POOL_SLOT_SIZE_SMALL,
                  HFOC_LWIP_POOL_SLOT_COUNT_SMALL) &&
              pool_is_initial(
                  &snapshot, HFOC_LWIP_POOL_CLASS_MEDIUM,
                  HFOC_LWIP_POOL_SLOT_SIZE_MEDIUM,
                  HFOC_LWIP_POOL_SLOT_COUNT_MEDIUM) &&
              pool_is_initial(
                  &snapshot, HFOC_LWIP_POOL_CLASS_LARGE,
                  HFOC_LWIP_POOL_SLOT_SIZE_LARGE,
                  HFOC_LWIP_POOL_SLOT_COUNT_LARGE),
          "re-init restores exact initial pool counts");
}

static void test_lwip_adapter_wrappers(void)
{
    void *first;
    uint8_t *zeroed;

    CHECK(init_allocator(), "adapter fixture initializes allocator");
    first = hfoc_lwip_mem_malloc(16U);
    CHECK(first != NULL &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 1U,
          "lwIP malloc wrapper uses HFOC tiny pool");
    hfoc_lwip_mem_free(NULL);
    hfoc_lwip_mem_free(first);
    zeroed = hfoc_lwip_mem_calloc(4U, 8U);
    CHECK(zeroed != NULL &&
              zeroed[0] == 0U && zeroed[31] == 0U,
          "lwIP calloc wrapper returns zeroed memory");
    hfoc_lwip_mem_free(zeroed);
    CHECK(hfoc_lwip_alloc_free_count(
              HFOC_LWIP_POOL_CLASS_TINY) ==
              HFOC_LWIP_POOL_SLOT_COUNT_TINY &&
              hfoc_lwip_alloc_used_count(
                  HFOC_LWIP_POOL_CLASS_TINY) == 0U,
          "lwIP free wrapper restores exact counts");
}

int main(void)
{
    test_unpublished_fail_closed();
    test_init_publishes_exact_pools();
    test_size_class_boundaries();
    test_exhaustion_and_reuse();
    test_calloc_validation_and_zeroing();
    test_free_validation();
    test_shutdown_restores_initial_counts();
    test_lwip_adapter_wrappers();

    if (failures == 0) {
        printf("lwIP allocator boundary host tests: ALL OK\n");
    }
    return failures != 0;
}
