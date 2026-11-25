#include "packet_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PACKET_POOL_HOST_TEST

#define TEST_COUNT 4U
#define TEST_CAPACITY 64U
#define TEST_HEADROOM 16U
#define TEST_LENGTH 64U
#define TEST_STORAGE_SIZE (TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))

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

static bool slot_is_initial(
    const struct packet_pool_slot_descriptor *descriptor,
    uint32_t slot)
{
    return descriptor != NULL &&
           descriptor->slot == slot &&
           descriptor->generation == 1U &&
           descriptor->owner == PACKET_POOL_OWNER_FREE &&
           descriptor->direction == PACKET_POOL_DIRECTION_NONE;
}

static void test_status_strings(void)
{
    static const struct {
        enum packet_pool_status status;
        const char *string;
    } statuses[] = {
        {PACKET_POOL_OK, "PACKET_POOL_OK"},
        {PACKET_POOL_NULL, "PACKET_POOL_NULL"},
        {PACKET_POOL_BAD_STATE, "PACKET_POOL_BAD_STATE"},
        {PACKET_POOL_BAD_COUNT, "PACKET_POOL_BAD_COUNT"},
        {PACKET_POOL_BAD_CAPACITY, "PACKET_POOL_BAD_CAPACITY"},
        {PACKET_POOL_BAD_HEADROOM, "PACKET_POOL_BAD_HEADROOM"},
        {PACKET_POOL_BAD_LENGTH, "PACKET_POOL_BAD_LENGTH"},
        {PACKET_POOL_BAD_STORAGE, "PACKET_POOL_BAD_STORAGE"},
        {PACKET_POOL_OVERFLOW, "PACKET_POOL_OVERFLOW"},
        {PACKET_POOL_BAD_SLOT, "PACKET_POOL_BAD_SLOT"},
        {PACKET_POOL_EXHAUSTED, "PACKET_POOL_EXHAUSTED"},
        {PACKET_POOL_BAD_DIRECTION, "PACKET_POOL_BAD_DIRECTION"},
        {PACKET_POOL_STALE_DESCRIPTOR,
         "PACKET_POOL_STALE_DESCRIPTOR"},
        {PACKET_POOL_WRONG_OWNER, "PACKET_POOL_WRONG_OWNER"},
        {PACKET_POOL_DOUBLE_FREE, "PACKET_POOL_DOUBLE_FREE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = packet_pool_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "packet pool status string is exact");
    }
    CHECK(packet_pool_status_string(
              (enum packet_pool_status)15U) == NULL,
          "packet pool undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet pool status handling performs no allocation");
}

static void test_unpublished_and_null_behavior(void)
{
    struct packet_pool_slot_descriptor descriptor;

    reset_pool();
    CHECK(packet_pool_init(
              NULL, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_NULL,
          "packet pool init rejects null pool");
    CHECK(packet_pool_init(
              &pool, NULL, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_NULL,
          "packet pool init rejects null storage");
    CHECK(packet_pool_init(
              NULL, NULL, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_NULL,
          "packet pool init rejects null pool and storage");

    CHECK(packet_pool_is_published(NULL) == false &&
              packet_pool_count(NULL) == 0U &&
              packet_pool_capacity(NULL) == 0U &&
              packet_pool_headroom(NULL) == 0U &&
              packet_pool_length(NULL) == 0U &&
              packet_pool_storage_size(NULL) == 0U,
          "null pool accessors return inactive state");
    CHECK(packet_pool_slot(
              &pool, 0U, &descriptor) == PACKET_POOL_BAD_STATE,
          "unpublished pool rejects slot descriptor");
    CHECK(packet_pool_slot(
              &pool, TEST_COUNT, &descriptor) ==
              PACKET_POOL_BAD_STATE,
          "unpublished pool rejects out-of-range slot");
    CHECK(packet_pool_slot(
              &pool, 0U, NULL) == PACKET_POOL_NULL,
          "null slot descriptor is rejected");
}

static void test_invalid_configuration_publishes_nothing(void)
{
    reset_pool();
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              PACKET_POOL_COUNT_MIN - 1U, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_COUNT &&
              !packet_pool_is_published(&pool),
          "zero packet count publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              PACKET_POOL_COUNT_MAX + 1U, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_COUNT &&
              !packet_pool_is_published(&pool),
          "over-limit packet count publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, PACKET_POOL_CAPACITY_MIN - 1U,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_CAPACITY &&
              !packet_pool_is_published(&pool),
          "zero packet capacity publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, PACKET_POOL_CAPACITY_MAX + 1U,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_CAPACITY &&
              !packet_pool_is_published(&pool),
          "over-limit packet capacity publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              PACKET_POOL_HEADROOM_MAX + 1U,
              TEST_LENGTH) == PACKET_POOL_BAD_HEADROOM &&
              !packet_pool_is_published(&pool),
          "over-limit headroom publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, PACKET_POOL_LENGTH_MIN - 1U) ==
              PACKET_POOL_BAD_LENGTH &&
              !packet_pool_is_published(&pool),
          "zero maximum length publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_CAPACITY + 1U) ==
              PACKET_POOL_BAD_LENGTH &&
              !packet_pool_is_published(&pool),
          "maximum length above capacity publishes no pool");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage) + 1U,
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_STORAGE &&
              !packet_pool_is_published(&pool),
          "mismatched storage size publishes no pool");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid configuration performs no allocation");
}

static void test_multiplication_overflow_publishes_nothing(void)
{
    reset_pool();
    CHECK(packet_pool_init(
              &pool, storage, (size_t)-1,
              UINT32_MAX, UINT32_MAX,
              UINT32_MAX, UINT32_MAX) == PACKET_POOL_OVERFLOW &&
              !packet_pool_is_published(&pool) &&
              packet_pool_count(&pool) == 0U &&
              packet_pool_capacity(&pool) == 0U,
          "arithmetic overflow publishes no pool");

    reset_pool();
    CHECK(packet_pool_init(
              &pool, storage, PACKET_POOL_STORAGE_SIZE_MAX + 1U,
              PACKET_POOL_COUNT_MAX, PACKET_POOL_CAPACITY_MAX,
              PACKET_POOL_HEADROOM_MAX, PACKET_POOL_CAPACITY_MAX) ==
              PACKET_POOL_OVERFLOW &&
              !packet_pool_is_published(&pool) &&
              packet_pool_storage_size(&pool) == 0U,
          "fixed storage budget overflow publishes no pool");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "overflow rejection performs no allocation");
}

static void test_valid_publication_and_slot_metadata(void)
{
    struct packet_pool_slot_descriptor descriptor;
    uint32_t slot;

    CHECK(init_pool(),
          "valid packet pool config publishes a bounded pool");
    CHECK(packet_pool_is_published(&pool) &&
              packet_pool_count(&pool) == TEST_COUNT &&
              packet_pool_capacity(&pool) == TEST_CAPACITY &&
              packet_pool_headroom(&pool) == TEST_HEADROOM &&
              packet_pool_length(&pool) == TEST_LENGTH &&
              packet_pool_storage_size(&pool) == sizeof(storage) &&
              pool.storage == storage &&
              pool.version == PACKET_POOL_VERSION,
          "published pool exposes exact fixed configuration");
    for (slot = 0U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_slot(
                  &pool, slot, &descriptor) == PACKET_POOL_OK &&
                  slot_is_initial(&descriptor, slot),
              "published pool seeds free/none/generation-one slot");
    }
    CHECK(packet_pool_slot(
              &pool, TEST_COUNT, &descriptor) == PACKET_POOL_BAD_SLOT,
          "out-of-range slot descriptor is rejected");
    CHECK(packet_pool_slot(
              &pool, 0U, NULL) == PACKET_POOL_NULL,
          "null slot descriptor output is rejected");
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_BAD_STATE &&
              packet_pool_count(&pool) == TEST_COUNT &&
              packet_pool_capacity(&pool) == TEST_CAPACITY,
          "duplicate publication is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid publication performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_unpublished_and_null_behavior();
    test_invalid_configuration_publishes_nothing();
    test_multiplication_overflow_publishes_nothing();
    test_valid_publication_and_slot_metadata();

    if (failures == 0) {
        printf("Packet pool descriptor host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_PACKET_POOL_HOST_TEST"
#endif
