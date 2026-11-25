#include "packet_pressure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PACKET_PRESSURE_HOST_TEST

#define TEST_COUNT 8U
#define TEST_CAPACITY 128U
#define TEST_HEADROOM 0U
#define TEST_LENGTH 128U
#define TEST_STORAGE_SIZE \
    (TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct packet_pool_descriptor pool;
static unsigned char storage[TEST_STORAGE_SIZE];

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

static void reset_pool(void)
{
    memset(&pool, 0, sizeof(pool));
    memset(storage, 0xA5, sizeof(storage));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static bool init_pool(void)
{
    reset_pool();
    return packet_pool_init(
               &pool, storage, sizeof(storage),
               TEST_COUNT, TEST_CAPACITY,
               TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_OK;
}

static bool init_pressure(struct packet_pressure *pressure)
{
    memset(pressure, 0, sizeof(*pressure));
    return packet_pressure_init(
               pressure, &pool) == PACKET_PRESSURE_OK &&
           packet_pressure_published(pressure);
}

static bool snapshot_matches(
    const struct packet_pressure_snapshot *snapshot,
    uint32_t generation,
    uint32_t free_count,
    uint32_t in_use_count,
    uint32_t high_water_count,
    uint32_t rx_drop_count,
    uint32_t tx_backpressure_count,
    uint32_t invalid_frame_count)
{
    return snapshot != NULL &&
           snapshot->version == PACKET_PRESSURE_VERSION &&
           snapshot->generation == generation &&
           snapshot->pool_count == TEST_COUNT &&
           snapshot->pool_free_count == free_count &&
           snapshot->pool_in_use_count == in_use_count &&
           snapshot->pool_high_water_count == high_water_count &&
           snapshot->rx_drop_count == rx_drop_count &&
           snapshot->tx_backpressure_count ==
               tx_backpressure_count &&
           snapshot->invalid_frame_count == invalid_frame_count;
}

static void test_status_strings(void)
{
    static const struct {
        enum packet_pressure_status status;
        const char *string;
    } statuses[] = {
        {PACKET_PRESSURE_OK, "PACKET_PRESSURE_OK"},
        {PACKET_PRESSURE_NULL, "PACKET_PRESSURE_NULL"},
        {PACKET_PRESSURE_BAD_STATE, "PACKET_PRESSURE_BAD_STATE"},
        {PACKET_PRESSURE_INVALID_VERSION,
         "PACKET_PRESSURE_INVALID_VERSION"},
        {PACKET_PRESSURE_ALREADY_PUBLISHED,
         "PACKET_PRESSURE_ALREADY_PUBLISHED"},
        {PACKET_PRESSURE_UNPUBLISHED,
         "PACKET_PRESSURE_UNPUBLISHED"},
        {PACKET_PRESSURE_BAD_POOL, "PACKET_PRESSURE_BAD_POOL"},
        {PACKET_PRESSURE_BAD_COUNTERS,
         "PACKET_PRESSURE_BAD_COUNTERS"},
        {PACKET_PRESSURE_TRANSACTION_OPEN,
         "PACKET_PRESSURE_TRANSACTION_OPEN"},
        {PACKET_PRESSURE_NO_TRANSACTION,
         "PACKET_PRESSURE_NO_TRANSACTION"},
        {PACKET_PRESSURE_OVERFLOW, "PACKET_PRESSURE_OVERFLOW"},
        {PACKET_PRESSURE_GENERATION_OVERFLOW,
         "PACKET_PRESSURE_GENERATION_OVERFLOW"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = packet_pressure_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "packet pressure status string is exact");
    }
    CHECK(packet_pressure_status_string(
              (enum packet_pressure_status)12U) == NULL,
          "packet pressure undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure status handling performs no allocation");
}

static void test_init_contracts(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot snapshot;

    CHECK(init_pool(),
          "packet pressure init fixture initializes pool");
    memset(&pressure, 0, sizeof(pressure));
    CHECK(packet_pressure_init(
              NULL, &pool) == PACKET_PRESSURE_NULL,
          "packet pressure init rejects null pressure");
    CHECK(packet_pressure_init(
              &pressure, NULL) == PACKET_PRESSURE_BAD_POOL,
          "packet pressure init rejects null pool");
    CHECK(packet_pressure_init(
              &pressure, &pool) == PACKET_PRESSURE_OK &&
              packet_pressure_published(&pressure),
          "packet pressure init publishes valid pressure");
    CHECK(packet_pressure_init(
              &pressure, &pool) ==
              PACKET_PRESSURE_ALREADY_PUBLISHED,
          "packet pressure init rejects duplicate publication");
    CHECK(packet_pressure_published(NULL) == false,
          "packet pressure published rejects null pressure");
    CHECK(packet_pressure_snapshot(
              NULL, &snapshot) == PACKET_PRESSURE_NULL,
          "packet pressure snapshot rejects null pressure");
    CHECK(packet_pressure_snapshot(
              &pressure, NULL) == PACKET_PRESSURE_NULL,
          "packet pressure snapshot rejects null output");

    reset_pool();
    memset(&pressure, 0, sizeof(pressure));
    CHECK(packet_pressure_init(
              &pressure, &pool) == PACKET_PRESSURE_BAD_POOL,
          "packet pressure init rejects unpublished pool");

    CHECK(init_pool(),
          "packet pressure incoherent fixture initializes pool");
    memset(&pressure, 0, sizeof(pressure));
    pool.free_count = TEST_COUNT + 1U;
    CHECK(packet_pressure_init(
              &pressure, &pool) == PACKET_PRESSURE_BAD_POOL &&
              !packet_pressure_published(&pressure),
          "packet pressure init rejects incoherent pool counts");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure init paths perform no allocation");
}

static void test_initial_snapshot_and_preconditions(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot snapshot;

    CHECK(init_pool() && init_pressure(&pressure),
          "packet pressure initial fixture initializes");
    CHECK(packet_pressure_snapshot(
              &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 1U, TEST_COUNT, 0U, 0U, 0U, 0U, 0U),
          "packet pressure initial snapshot is exact");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK,
          "packet pressure begin update opens transaction");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_TRANSACTION_OPEN,
          "packet pressure begin rejects duplicate transaction");
    CHECK(packet_pressure_refresh_pool_counts(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_rollback(&pressure) ==
                  PACKET_PRESSURE_OK,
          "packet pressure refresh and rollback close transaction");

    CHECK(packet_pressure_refresh_pool_counts(
              &pressure) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure refresh rejects missing transaction");
    CHECK(packet_pressure_record_rx_drop(
              &pressure, 1U) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure RX record rejects missing transaction");
    CHECK(packet_pressure_record_tx_backpressure(
              &pressure, 1U) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure TX record rejects missing transaction");
    CHECK(packet_pressure_record_invalid_frame(
              &pressure, 1U) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure invalid-frame record rejects missing transaction");
    CHECK(packet_pressure_commit(
              &pressure) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure commit rejects missing transaction");
    CHECK(packet_pressure_rollback(
              &pressure) == PACKET_PRESSURE_NO_TRANSACTION,
          "packet pressure rollback rejects missing transaction");

    CHECK(init_pressure(&pressure),
          "packet pressure invalid-version fixture initializes");
    pressure.version = 0U;
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_INVALID_VERSION,
          "packet pressure begin rejects invalid version");

    memset(&pressure, 0, sizeof(pressure));
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_UNPUBLISHED,
          "packet pressure begin rejects unpublished pressure");
    CHECK(packet_pressure_snapshot(
              &pressure, &snapshot) == PACKET_PRESSURE_UNPUBLISHED,
          "packet pressure snapshot rejects unpublished pressure");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure precondition paths perform no allocation");
}

static void test_atomic_commit_and_generation(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot first;
    struct packet_pressure_snapshot second;

    CHECK(init_pool() && init_pressure(&pressure),
          "packet pressure commit fixture initializes");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_record_rx_drop(
                  &pressure, 5U) == PACKET_PRESSURE_OK &&
              packet_pressure_record_tx_backpressure(
                  &pressure, 3U) == PACKET_PRESSURE_OK &&
              packet_pressure_record_invalid_frame(
                  &pressure, 2U) == PACKET_PRESSURE_OK,
          "packet pressure pending event records accumulate");
    CHECK(packet_pressure_snapshot(
              &pressure, &first) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &first, 1U, TEST_COUNT, 0U, 0U, 0U, 0U, 0U),
          "packet pressure pending mutations are invisible to readers");
    CHECK(packet_pressure_commit(
              &pressure) == PACKET_PRESSURE_OK,
          "packet pressure commit publishes pending snapshot");
    CHECK(packet_pressure_snapshot(
              &pressure, &first) == PACKET_PRESSURE_OK &&
              packet_pressure_snapshot(
                  &pressure, &second) == PACKET_PRESSURE_OK &&
              memcmp(&first, &second, sizeof(first)) == 0 &&
              snapshot_matches(
                  &first, 2U, TEST_COUNT, 0U, 0U, 5U, 3U, 2U),
          "packet pressure committed generation is coherent and identical");

    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_record_rx_drop(
                  &pressure, 1U) == PACKET_PRESSURE_OK &&
              packet_pressure_record_invalid_frame(
                  &pressure, 1U) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK,
          "packet pressure second transaction commits");
    CHECK(packet_pressure_snapshot(
              &pressure, &first) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &first, 3U, TEST_COUNT, 0U, 0U, 6U, 3U, 3U),
          "packet pressure second committed snapshot is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure commit path performs no allocation");
}

static void test_pool_refresh_and_high_water(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot snapshot;
    struct packet_pool_slot_descriptor first;
    struct packet_pool_slot_descriptor second;

    CHECK(init_pool() && init_pressure(&pressure),
          "packet pressure refresh fixture initializes");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_RX,
                  &first) == PACKET_POOL_OK &&
              packet_pool_commit(
                  &pool, &first) == PACKET_POOL_OK &&
              packet_pressure_refresh_pool_counts(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 2U,
                  TEST_COUNT - 1U, 1U, 1U, 0U, 0U, 0U),
          "packet pressure refresh publishes one active packet");

    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_RX,
                  &second) == PACKET_POOL_OK &&
              packet_pool_commit(
                  &pool, &second) == PACKET_POOL_OK &&
              packet_pressure_refresh_pool_counts(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 3U,
                  TEST_COUNT - 2U, 2U, 2U, 0U, 0U, 0U),
          "packet pressure refresh raises high-water with in-use");

    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pool_release(
                  &pool, &first) == PACKET_POOL_OK &&
              packet_pressure_refresh_pool_counts(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 4U,
                  TEST_COUNT - 1U, 1U, 2U, 0U, 0U, 0U),
          "packet pressure high-water remains monotonic after release");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure refresh path performs no allocation");
}

static void test_event_failures_and_rollback(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot snapshot;

    CHECK(init_pool() && init_pressure(&pressure),
          "packet pressure event-failure fixture initializes");
    CHECK(packet_pressure_record_rx_drop(
              NULL, 1U) == PACKET_PRESSURE_NULL &&
              packet_pressure_record_tx_backpressure(
                  NULL, 1U) == PACKET_PRESSURE_NULL &&
              packet_pressure_record_invalid_frame(
                  NULL, 1U) == PACKET_PRESSURE_NULL,
          "packet pressure event records reject null pressure");

    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_record_rx_drop(
                  &pressure, 0U) == PACKET_PRESSURE_BAD_COUNTERS &&
              packet_pressure_record_tx_backpressure(
                  &pressure, 0U) == PACKET_PRESSURE_BAD_COUNTERS &&
              packet_pressure_record_invalid_frame(
                  &pressure, 0U) == PACKET_PRESSURE_BAD_COUNTERS &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 1U, TEST_COUNT, 0U, 0U, 0U, 0U, 0U),
          "packet pressure zero event records fail without mutation");

    pressure.pending_snapshot.rx_drop_count = UINT32_MAX;
    CHECK(packet_pressure_record_rx_drop(
              &pressure, 1U) == PACKET_PRESSURE_OVERFLOW &&
              pressure.pending_snapshot.rx_drop_count == UINT32_MAX &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 1U, TEST_COUNT, 0U, 0U, 0U, 0U, 0U),
          "packet pressure RX count overflow fails without mutation");

    CHECK(packet_pressure_rollback(
              &pressure) == PACKET_PRESSURE_OK,
          "packet pressure rollback closes failed transaction");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_record_tx_backpressure(
                  &pressure, 4U) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 2U, TEST_COUNT, 0U, 0U, 0U, 4U, 0U),
          "packet pressure rollback preserves published state for retry");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure event failure paths perform no allocation");
}

static void test_bad_counters_and_generation_overflow(void)
{
    struct packet_pressure pressure;
    struct packet_pressure_snapshot snapshot;

    CHECK(init_pool() && init_pressure(&pressure),
          "packet pressure bad-counter fixture initializes");
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK,
          "packet pressure bad-counter opens transaction");
    pressure.pending_snapshot.pool_free_count = TEST_COUNT;
    pressure.pending_snapshot.pool_in_use_count = 1U;
    CHECK(packet_pressure_commit(
              &pressure) == PACKET_PRESSURE_BAD_COUNTERS &&
              pressure.transaction_open &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot_matches(
                  &snapshot, 1U, TEST_COUNT, 0U, 0U, 0U, 0U, 0U),
          "packet pressure commit rejects incoherent pending counters");
    CHECK(packet_pressure_rollback(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_begin_update(
                  &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_commit(
                  &pressure) == PACKET_PRESSURE_OK,
          "packet pressure failed commit can be rolled back and retried");

    CHECK(init_pressure(&pressure),
          "packet pressure generation fixture initializes");
    pressure.published_snapshot.generation = UINT32_MAX;
    CHECK(packet_pressure_begin_update(
              &pressure) == PACKET_PRESSURE_OK &&
              packet_pressure_record_invalid_frame(
                  &pressure, 1U) == PACKET_PRESSURE_OK,
          "packet pressure generation fixture opens transaction");
    CHECK(packet_pressure_commit(
              &pressure) == PACKET_PRESSURE_GENERATION_OVERFLOW &&
              pressure.transaction_open &&
              packet_pressure_snapshot(
                  &pressure, &snapshot) == PACKET_PRESSURE_OK &&
              snapshot.generation == UINT32_MAX &&
              snapshot.invalid_frame_count == 0U,
          "packet pressure generation overflow fails without publication");
    CHECK(packet_pressure_rollback(
              &pressure) == PACKET_PRESSURE_OK,
          "packet pressure generation overflow rollback closes transaction");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pressure bad-counter/generation paths perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_contracts();
    test_initial_snapshot_and_preconditions();
    test_atomic_commit_and_generation();
    test_pool_refresh_and_high_water();
    test_event_failures_and_rollback();
    test_bad_counters_and_generation_overflow();

    if (failures != 0) {
        fprintf(stderr,
                "Packet pressure host tests: FAILED\n");
        return 1;
    }
    printf("Packet pressure host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_PACKET_PRESSURE_HOST_TEST must be defined"
#endif
