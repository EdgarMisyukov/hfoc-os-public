#include "packet_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PACKET_LIFECYCLE_HOST_TEST

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

static bool descriptor_is(
    const struct packet_pool_slot_descriptor *descriptor,
    uint32_t slot,
    uint32_t generation,
    enum packet_pool_owner owner,
    enum packet_pool_direction direction)
{
    return descriptor != NULL &&
           descriptor->slot == slot &&
           descriptor->generation == generation &&
           descriptor->owner == owner &&
           descriptor->direction == direction;
}

static bool counts_are(
    uint32_t free_count,
    uint32_t reserved_count,
    uint32_t active_count)
{
    return packet_pool_free_count(&pool) == free_count &&
           packet_pool_reserved_count(&pool) == reserved_count &&
           packet_pool_active_count(&pool) == active_count;
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
              "packet lifecycle status string is exact");
    }
    CHECK(packet_pool_status_string(
              (enum packet_pool_status)15U) == NULL,
          "packet lifecycle undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet lifecycle status handling performs no allocation");
}

static void test_null_and_unpublished_behavior(void)
{
    struct packet_pool_slot_descriptor descriptor;

    reset_pool();
    CHECK(packet_pool_reserve(
              NULL, PACKET_POOL_DIRECTION_RX,
              &descriptor) == PACKET_POOL_NULL,
          "reserve rejects null pool");
    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_RX,
              NULL) == PACKET_POOL_NULL,
          "reserve rejects null descriptor");
    CHECK(packet_pool_commit(
              NULL, &descriptor) == PACKET_POOL_NULL,
          "commit rejects null pool");
    CHECK(packet_pool_commit(
              &pool, NULL) == PACKET_POOL_NULL,
          "commit rejects null descriptor");
    CHECK(packet_pool_transfer(
              NULL, &descriptor,
              PACKET_POOL_DIRECTION_TX) == PACKET_POOL_NULL,
          "transfer rejects null pool");
    CHECK(packet_pool_transfer(
              &pool, NULL,
              PACKET_POOL_DIRECTION_TX) == PACKET_POOL_NULL,
          "transfer rejects null descriptor");
    CHECK(packet_pool_release(
              NULL, &descriptor) == PACKET_POOL_NULL,
          "release rejects null pool");
    CHECK(packet_pool_release(
              &pool, NULL) == PACKET_POOL_NULL,
          "release rejects null descriptor");
    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_RX,
              &descriptor) == PACKET_POOL_BAD_STATE &&
              counts_are(0U, 0U, 0U),
          "unpublished pool rejects reserve without mutation");
    CHECK(packet_pool_commit(
              &pool, &descriptor) == PACKET_POOL_BAD_STATE &&
              counts_are(0U, 0U, 0U),
          "unpublished pool rejects commit without mutation");
    CHECK(packet_pool_transfer(
              &pool, &descriptor,
              PACKET_POOL_DIRECTION_TX) == PACKET_POOL_BAD_STATE &&
              counts_are(0U, 0U, 0U),
          "unpublished pool rejects transfer without mutation");
    CHECK(packet_pool_release(
              &pool, &descriptor) == PACKET_POOL_BAD_STATE &&
              counts_are(0U, 0U, 0U),
          "unpublished pool rejects release without mutation");
    CHECK(packet_pool_free_count(NULL) == 0U &&
              packet_pool_reserved_count(NULL) == 0U &&
              packet_pool_active_count(NULL) == 0U,
          "null pool count accessors return inactive state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "null and unpublished handling performs no allocation");
}

static void test_reserve_commit_transfer_release(void)
{
    struct packet_pool_slot_descriptor rx;
    struct packet_pool_slot_descriptor tx;
    struct packet_pool_slot_descriptor current;
    struct packet_pool_slot_descriptor stale;
    struct packet_pool_slot_descriptor nonowner;

    CHECK(init_pool(),
          "lifecycle fixture initializes pool");
    CHECK(counts_are(TEST_COUNT, 0U, 0U),
          "published pool starts with every slot free");
    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_NONE,
              &rx) == PACKET_POOL_BAD_DIRECTION &&
              counts_are(TEST_COUNT, 0U, 0U),
          "reserve rejects NONE direction without mutation");

    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_RX,
              &rx) == PACKET_POOL_OK &&
              descriptor_is(
                  &rx, 0U, 1U,
                  PACKET_POOL_OWNER_RESERVED,
                  PACKET_POOL_DIRECTION_RX) &&
              counts_are(TEST_COUNT - 1U, 1U, 0U),
          "reserve claims exact free slot as RX");
    CHECK(packet_pool_slot(
              &pool, 0U, &current) == PACKET_POOL_OK &&
              descriptor_is(
                  &current, 0U, 1U,
                  PACKET_POOL_OWNER_RESERVED,
                  PACKET_POOL_DIRECTION_RX),
          "reserved slot metadata is visible through slot accessor");

    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_TX,
              &tx) == PACKET_POOL_OK &&
              descriptor_is(
                  &tx, 1U, 1U,
                  PACKET_POOL_OWNER_RESERVED,
                  PACKET_POOL_DIRECTION_TX) &&
              counts_are(TEST_COUNT - 2U, 2U, 0U),
          "second reserve claims next slot as TX");

    CHECK(packet_pool_commit(
              &pool, &rx) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT - 2U, 1U, 1U),
          "commit publishes reserved RX slot as active");
    CHECK(packet_pool_slot(
              &pool, 0U, &current) == PACKET_POOL_OK &&
              descriptor_is(
                  &current, 0U, 1U,
                  PACKET_POOL_OWNER_ACTIVE,
                  PACKET_POOL_DIRECTION_RX),
          "committed RX slot exposes active owner and direction");
    CHECK(packet_pool_commit(
              &pool, &rx) == PACKET_POOL_WRONG_OWNER &&
              counts_are(TEST_COUNT - 2U, 1U, 1U),
          "duplicate commit is rejected without mutation");

    CHECK(packet_pool_commit(
              &pool, &tx) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT - 2U, 0U, 2U),
          "commit publishes reserved TX slot as active");
    CHECK(packet_pool_transfer(
              &pool, &rx,
              PACKET_POOL_DIRECTION_TX) == PACKET_POOL_OK &&
              packet_pool_slot(
                  &pool, 0U, &current) == PACKET_POOL_OK &&
              descriptor_is(
                  &current, 0U, 1U,
                  PACKET_POOL_OWNER_ACTIVE,
                  PACKET_POOL_DIRECTION_TX) &&
              counts_are(TEST_COUNT - 2U, 0U, 2U),
          "transfer moves an active RX packet to TX");
    CHECK(packet_pool_transfer(
              &pool, &current,
              PACKET_POOL_DIRECTION_NONE) ==
              PACKET_POOL_BAD_DIRECTION &&
              counts_are(TEST_COUNT - 2U, 0U, 2U),
          "transfer rejects NONE destination without mutation");
    CHECK(packet_pool_transfer(
              &pool, &current,
              PACKET_POOL_DIRECTION_TX) ==
              PACKET_POOL_BAD_DIRECTION &&
              counts_are(TEST_COUNT - 2U, 0U, 2U),
          "transfer rejects same destination without mutation");

    stale = current;
    CHECK(packet_pool_release(
              &pool, &current) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "release returns active packet exactly once");
    CHECK(packet_pool_slot(
              &pool, 0U, &current) == PACKET_POOL_OK &&
              descriptor_is(
                  &current, 0U, 2U,
                  PACKET_POOL_OWNER_FREE,
                  PACKET_POOL_DIRECTION_NONE),
          "release publishes free/none slot and advances generation");
    CHECK(packet_pool_release(
              &pool, &current) == PACKET_POOL_DOUBLE_FREE &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "double release is rejected without mutation");
    CHECK(packet_pool_commit(
              &pool, &stale) == PACKET_POOL_STALE_DESCRIPTOR &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "stale descriptor cannot commit a released slot");

    nonowner = tx;
    nonowner.direction = PACKET_POOL_DIRECTION_RX;
    CHECK(packet_pool_release(
              &pool, &nonowner) == PACKET_POOL_WRONG_OWNER &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "release-by-nonowner is rejected without mutation");

    CHECK(packet_pool_release(
              &pool, &tx) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT, 0U, 0U),
          "second active packet releases exactly once");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "reserve/commit/transfer/release performs no allocation");
}

static void test_exhaustion_and_reserved_rollback(void)
{
    struct packet_pool_slot_descriptor descriptors[TEST_COUNT];
    struct packet_pool_slot_descriptor extra;
    struct packet_pool_slot_descriptor current;
    uint32_t slot;

    CHECK(init_pool(),
          "exhaustion fixture initializes pool");
    for (slot = 0U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_RX,
                  &descriptors[slot]) == PACKET_POOL_OK &&
                  descriptor_is(
                      &descriptors[slot], slot, 1U,
                      PACKET_POOL_OWNER_RESERVED,
                      PACKET_POOL_DIRECTION_RX) &&
                  packet_pool_free_count(&pool) ==
                      TEST_COUNT - slot - 1U,
              "pool reserve fills exact free slot in order");
    }
    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_TX,
              &extra) == PACKET_POOL_EXHAUSTED &&
              counts_are(0U, TEST_COUNT, 0U),
          "exhausted reserve leaves all ownership unchanged");

    CHECK(packet_pool_release(
              &pool, &descriptors[0]) == PACKET_POOL_OK &&
              counts_are(1U, TEST_COUNT - 1U, 0U),
          "reserved slot can roll back before commit");
    CHECK(packet_pool_slot(
              &pool, 0U, &current) == PACKET_POOL_OK &&
              descriptor_is(
                  &current, 0U, 2U,
                  PACKET_POOL_OWNER_FREE,
                  PACKET_POOL_DIRECTION_NONE),
          "reserved rollback publishes free/none and advances generation");
    CHECK(packet_pool_release(
              &pool, &descriptors[0]) == PACKET_POOL_DOUBLE_FREE &&
              counts_are(1U, TEST_COUNT - 1U, 0U),
          "reserved rollback double release is rejected");

    for (slot = 1U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_release(
                  &pool, &descriptors[slot]) == PACKET_POOL_OK,
              "remaining reserved slots roll back once");
    }
    CHECK(counts_are(TEST_COUNT, 0U, 0U),
          "reserved rollback cycle leaks no ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exhaustion and rollback performs no allocation");
}

static void test_stale_release_by_nonowner_and_full_cycle(void)
{
    struct packet_pool_slot_descriptor rx[TEST_COUNT];
    struct packet_pool_slot_descriptor stale;
    struct packet_pool_slot_descriptor reacquired;
    struct packet_pool_slot_descriptor nonowner;
    struct packet_pool_slot_descriptor current;
    uint32_t slot;

    CHECK(init_pool(),
          "full-cycle fixture initializes pool");
    for (slot = 0U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_RX,
                  &rx[slot]) == PACKET_POOL_OK &&
                  packet_pool_commit(
                      &pool, &rx[slot]) == PACKET_POOL_OK,
              "full-cycle fixture reserves and commits every RX slot");
    }
    for (slot = 0U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_transfer(
                  &pool, &rx[slot],
                  PACKET_POOL_DIRECTION_TX) == PACKET_POOL_OK,
              "full-cycle fixture transfers every packet to TX");
    }
    for (slot = 0U; slot < TEST_COUNT; ++slot) {
        CHECK(packet_pool_release(
                  &pool, &rx[slot]) == PACKET_POOL_OK,
              "full-cycle fixture releases every TX packet once");
    }
    CHECK(counts_are(TEST_COUNT, 0U, 0U) &&
              packet_pool_slot(
                  &pool, 0U, &current) == PACKET_POOL_OK &&
              current.generation == 2U &&
              current.owner == PACKET_POOL_OWNER_FREE &&
              current.direction == PACKET_POOL_DIRECTION_NONE,
          "full RX/TX release cycle returns exact initial counts");

    stale = rx[0];
    CHECK(packet_pool_reserve(
              &pool, PACKET_POOL_DIRECTION_RX,
              &reacquired) == PACKET_POOL_OK &&
              descriptor_is(
                  &reacquired, 0U, 2U,
                  PACKET_POOL_OWNER_RESERVED,
                  PACKET_POOL_DIRECTION_RX),
          "released slot reacquires with a new generation");
    CHECK(packet_pool_release(
              &pool, &stale) == PACKET_POOL_STALE_DESCRIPTOR &&
              counts_are(TEST_COUNT - 1U, 1U, 0U),
          "stale descriptor cannot release a reacquired slot");
    CHECK(packet_pool_commit(
              &pool, &reacquired) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "current descriptor commits reacquired slot");

    nonowner = reacquired;
    nonowner.owner = PACKET_POOL_OWNER_ACTIVE;
    nonowner.direction = PACKET_POOL_DIRECTION_TX;
    CHECK(packet_pool_release(
              &pool, &nonowner) == PACKET_POOL_WRONG_OWNER &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "wrong-direction nonowner cannot release active packet");
    CHECK(packet_pool_transfer(
              &pool, &nonowner,
              PACKET_POOL_DIRECTION_RX) ==
              PACKET_POOL_WRONG_OWNER &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "wrong-direction nonowner cannot transfer active packet");
    CHECK(packet_pool_release(
              &pool, &reacquired) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT, 0U, 0U),
          "current active packet releases once");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stale/nonowner/full-cycle path performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_unpublished_behavior();
    test_reserve_commit_transfer_release();
    test_exhaustion_and_reserved_rollback();
    test_stale_release_by_nonowner_and_full_cycle();

    if (failures == 0) {
        printf("Packet acquire/release lifecycle host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_PACKET_LIFECYCLE_HOST_TEST"
#endif
