#include "packet_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PACKET_BUFFER_HOST_TEST

#define TEST_CAPACITY 4U
#define TEST_PAYLOAD 64U
#define TEST_STORAGE_SIZE (TEST_CAPACITY * TEST_PAYLOAD)

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct packet_buffer_pool pool;
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
    return packet_buffer_pool_init(
               &pool, storage, sizeof(storage),
               TEST_CAPACITY, TEST_PAYLOAD) == PACKET_BUFFER_OK;
}

static unsigned int owner_count(enum packet_buffer_owner owner)
{
    unsigned int count = 0U;
    uint32_t slot;

    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        if ((enum packet_buffer_owner)pool.owners[slot] == owner) {
            ++count;
        }
    }
    return count;
}

static bool descriptor_matches(
    const struct packet_buffer_descriptor *descriptor,
    uint32_t slot,
    uint32_t generation)
{
    return descriptor != NULL &&
           descriptor->slot == slot &&
           descriptor->generation == generation;
}

static void test_status_strings(void)
{
    static const struct {
        enum packet_buffer_status status;
        const char *string;
    } statuses[] = {
        {PACKET_BUFFER_OK, "PACKET_BUFFER_OK"},
        {PACKET_BUFFER_NULL, "PACKET_BUFFER_NULL"},
        {PACKET_BUFFER_BAD_STATE, "PACKET_BUFFER_BAD_STATE"},
        {PACKET_BUFFER_BAD_CAPACITY, "PACKET_BUFFER_BAD_CAPACITY"},
        {PACKET_BUFFER_BAD_PAYLOAD, "PACKET_BUFFER_BAD_PAYLOAD"},
        {PACKET_BUFFER_BAD_STORAGE, "PACKET_BUFFER_BAD_STORAGE"},
        {PACKET_BUFFER_EXHAUSTED, "PACKET_BUFFER_EXHAUSTED"},
        {PACKET_BUFFER_BAD_DESCRIPTOR, "PACKET_BUFFER_BAD_DESCRIPTOR"},
        {PACKET_BUFFER_STALE_DESCRIPTOR,
         "PACKET_BUFFER_STALE_DESCRIPTOR"},
        {PACKET_BUFFER_WRONG_OWNER, "PACKET_BUFFER_WRONG_OWNER"},
        {PACKET_BUFFER_DOUBLE_FREE, "PACKET_BUFFER_DOUBLE_FREE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = packet_buffer_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "packet buffer status string is exact");
    }
    CHECK(packet_buffer_status_string(
              (enum packet_buffer_status)11U) == NULL,
          "packet buffer undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet buffer status handling performs no allocation");
}

static void test_pool_validation_and_lifecycle(void)
{
    struct packet_buffer_descriptor descriptor;
    unsigned char small_storage[TEST_PAYLOAD];

    reset_pool();
    CHECK(packet_buffer_pool_init(
              NULL, storage, sizeof(storage),
              TEST_CAPACITY, TEST_PAYLOAD) == PACKET_BUFFER_NULL,
          "pool init rejects null pool");
    CHECK(packet_buffer_pool_init(
              &pool, NULL, sizeof(storage),
              TEST_CAPACITY, TEST_PAYLOAD) == PACKET_BUFFER_NULL,
          "pool init rejects null storage");
    CHECK(packet_buffer_pool_init(
              &pool, storage, sizeof(storage),
              PACKET_BUFFER_CAPACITY_MIN - 1U,
              TEST_PAYLOAD) == PACKET_BUFFER_BAD_CAPACITY,
          "pool init rejects zero capacity");
    CHECK(packet_buffer_pool_init(
              &pool, storage, sizeof(storage),
              PACKET_BUFFER_CAPACITY_MAX + 1U,
              TEST_PAYLOAD) == PACKET_BUFFER_BAD_CAPACITY,
          "pool init rejects over-limit capacity");
    CHECK(packet_buffer_pool_init(
              &pool, storage, sizeof(storage),
              TEST_CAPACITY, PACKET_BUFFER_PAYLOAD_MIN - 1U) ==
              PACKET_BUFFER_BAD_PAYLOAD,
          "pool init rejects zero payload size");
    CHECK(packet_buffer_pool_init(
              &pool, storage, sizeof(storage),
              TEST_CAPACITY, PACKET_BUFFER_PAYLOAD_MAX + 1U) ==
              PACKET_BUFFER_BAD_PAYLOAD,
          "pool init rejects over-limit payload size");
    CHECK(packet_buffer_pool_init(
              &pool, storage, sizeof(storage) + 1U,
              TEST_CAPACITY, TEST_PAYLOAD) == PACKET_BUFFER_BAD_STORAGE,
          "pool init rejects mismatched storage size");
    CHECK(packet_buffer_pool_init(
              &pool, small_storage, sizeof(small_storage),
              1U, TEST_PAYLOAD) == PACKET_BUFFER_OK,
          "pool init accepts exact single-buffer storage");
    CHECK(packet_buffer_pool_init(
              &pool, small_storage, sizeof(small_storage),
              1U, TEST_PAYLOAD) == PACKET_BUFFER_BAD_STATE,
          "pool init rejects duplicate publication");

    reset_pool();
    CHECK(packet_buffer_free_count(NULL) == 0U &&
              packet_buffer_capacity(NULL) == 0U &&
              packet_buffer_data(NULL, &descriptor) == NULL,
          "null pool accessors return inactive state");
    CHECK(packet_buffer_acquire_rx(
              &pool, &descriptor) == PACKET_BUFFER_BAD_STATE,
          "unpublished pool rejects RX acquisition");
    CHECK(packet_buffer_acquire_tx(
              &pool, &descriptor) == PACKET_BUFFER_BAD_STATE,
          "unpublished pool rejects TX acquisition");
    CHECK(init_pool(),
          "valid pool init publishes bounded pool");
    CHECK(packet_buffer_free_count(&pool) == TEST_CAPACITY &&
              packet_buffer_capacity(&pool) == TEST_CAPACITY &&
              owner_count(PACKET_BUFFER_OWNER_FREE) == TEST_CAPACITY,
          "valid pool starts with every slot free");

    descriptor.slot = 0U;
    descriptor.generation = 1U;
    CHECK(packet_buffer_data(&pool, &descriptor) == NULL,
          "free slot data access is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "pool validation and lifecycle perform no allocation");
}

static void test_exhaustion_and_receive_failure(void)
{
    struct packet_buffer_descriptor descriptors[TEST_CAPACITY];
    struct packet_buffer_descriptor extra;
    uint32_t slot;

    CHECK(init_pool(),
          "exhaustion fixture initializes pool");
    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_acquire_rx(
                  &pool, &descriptors[slot]) == PACKET_BUFFER_OK &&
                  descriptor_matches(
                      &descriptors[slot], slot, 1U) &&
                  (enum packet_buffer_owner)pool.owners[slot] ==
                      PACKET_BUFFER_OWNER_RX &&
                  packet_buffer_data(&pool, &descriptors[slot]) ==
                      storage + (size_t)slot * TEST_PAYLOAD &&
                  packet_buffer_free_count(&pool) ==
                      TEST_CAPACITY - slot - 1U,
              "RX acquisition claims the exact free slot");
    }
    CHECK(packet_buffer_acquire_rx(
              &pool, &extra) == PACKET_BUFFER_EXHAUSTED &&
              packet_buffer_free_count(&pool) == 0U &&
              owner_count(PACKET_BUFFER_OWNER_RX) == TEST_CAPACITY &&
              owner_count(PACKET_BUFFER_OWNER_FREE) == 0U,
          "exhausted RX acquisition leaves all ownership unchanged");

    CHECK(packet_buffer_release(
              &pool, &descriptors[0]) == PACKET_BUFFER_OK &&
              packet_buffer_free_count(&pool) == 1U &&
              pool.owners[0] == PACKET_BUFFER_OWNER_FREE &&
              packet_buffer_data(&pool, &descriptors[0]) == NULL,
          "receive failure reclaim returns the RX buffer once");
    CHECK(packet_buffer_release(
              &pool, &descriptors[0]) == PACKET_BUFFER_DOUBLE_FREE &&
              packet_buffer_free_count(&pool) == 1U &&
              pool.owners[0] == PACKET_BUFFER_OWNER_FREE,
          "double receive-failure return is rejected without mutation");

    CHECK(packet_buffer_acquire_rx(
              &pool, &descriptors[0]) == PACKET_BUFFER_OK &&
              descriptor_matches(&descriptors[0], 0U, 2U) &&
              packet_buffer_free_count(&pool) == 0U,
          "reclaimed buffer is reacquired with a new generation");
    CHECK(packet_buffer_release(
              &pool, &descriptors[0]) == PACKET_BUFFER_OK &&
              packet_buffer_free_count(&pool) == 1U,
          "reacquired RX buffer releases exactly once");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exhaustion and receive failure perform no allocation");
}

static void test_handoff_and_transmit_failure(void)
{
    struct packet_buffer_descriptor rx;
    struct packet_buffer_descriptor tx;
    struct packet_buffer_descriptor stale;
    unsigned char *payload;

    CHECK(init_pool(),
          "handoff fixture initializes pool");
    CHECK(packet_buffer_acquire_rx(
              &pool, &rx) == PACKET_BUFFER_OK,
          "handoff fixture acquires RX buffer");
    payload = packet_buffer_data(&pool, &rx);
    CHECK(payload != NULL,
          "RX buffer exposes owned payload");
    payload[0] = 0x5A;
    CHECK(packet_buffer_handoff_tx(
              &pool, &rx) == PACKET_BUFFER_OK &&
              pool.owners[0] == PACKET_BUFFER_OWNER_TX &&
              packet_buffer_data(&pool, &rx) == payload &&
              payload[0] == 0x5A,
          "RX-to-TX handoff preserves exact buffer ownership and data");
    CHECK(packet_buffer_handoff_tx(
              &pool, &rx) == PACKET_BUFFER_WRONG_OWNER &&
              pool.owners[0] == PACKET_BUFFER_OWNER_TX,
          "second handoff is rejected without mutation");
    CHECK(packet_buffer_release(
              &pool, &rx) == PACKET_BUFFER_OK &&
              packet_buffer_free_count(&pool) == TEST_CAPACITY &&
              pool.owners[0] == PACKET_BUFFER_OWNER_FREE,
          "transmit failure reclaim returns the TX buffer once");
    CHECK(packet_buffer_release(
              &pool, &rx) == PACKET_BUFFER_DOUBLE_FREE &&
              packet_buffer_free_count(&pool) == TEST_CAPACITY,
          "double transmit-failure return is rejected without mutation");

    CHECK(packet_buffer_acquire_tx(
              &pool, &tx) == PACKET_BUFFER_OK &&
              descriptor_matches(&tx, 0U, 2U) &&
              pool.owners[0] == PACKET_BUFFER_OWNER_TX,
          "direct TX acquisition claims a free buffer");
    stale = tx;
    CHECK(packet_buffer_release(
              &pool, &tx) == PACKET_BUFFER_OK &&
              packet_buffer_free_count(&pool) == TEST_CAPACITY,
          "direct TX failure returns the buffer exactly once");
    CHECK(packet_buffer_release(
              &pool, &stale) == PACKET_BUFFER_DOUBLE_FREE,
          "stale TX return is rejected as double free");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "handoff and transmit failure perform no allocation");
}

static void test_stale_descriptor_and_no_leak_cycle(void)
{
    struct packet_buffer_descriptor rx[TEST_CAPACITY];
    struct packet_buffer_descriptor tx_slots[TEST_CAPACITY];
    struct packet_buffer_descriptor tx;
    struct packet_buffer_descriptor stale;
    uint32_t slot;

    CHECK(init_pool(),
          "no-leak fixture initializes pool");
    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_acquire_rx(
                  &pool, &rx[slot]) == PACKET_BUFFER_OK,
              "no-leak fixture fills the pool");
    }
    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_handoff_tx(
                  &pool, &rx[slot]) == PACKET_BUFFER_OK,
              "no-leak fixture hands every RX buffer to TX");
    }
    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_release(
                  &pool, &rx[slot]) == PACKET_BUFFER_OK,
              "transmit-failure reclaim returns every TX buffer once");
    }
    CHECK(packet_buffer_free_count(&pool) == TEST_CAPACITY &&
              owner_count(PACKET_BUFFER_OWNER_FREE) == TEST_CAPACITY &&
              owner_count(PACKET_BUFFER_OWNER_RX) == 0U &&
              owner_count(PACKET_BUFFER_OWNER_TX) == 0U,
          "full RX/TX failure cycle leaks no ownership");

    stale = rx[0];
    CHECK(packet_buffer_acquire_rx(
              &pool, &tx) == PACKET_BUFFER_OK &&
              descriptor_matches(&tx, 0U, 2U),
          "released slot reacquires with the next generation");
    CHECK(packet_buffer_release(
              &pool, &stale) == PACKET_BUFFER_STALE_DESCRIPTOR &&
              pool.owners[0] == PACKET_BUFFER_OWNER_RX,
          "stale descriptor cannot release a reacquired buffer");
    CHECK(packet_buffer_data(&pool, &stale) == NULL &&
              packet_buffer_data(&pool, &tx) != NULL,
          "data access rejects stale descriptor");
    CHECK(packet_buffer_release(
              &pool, &tx) == PACKET_BUFFER_OK &&
              packet_buffer_free_count(&pool) == TEST_CAPACITY,
          "current descriptor still releases the reacquired buffer once");

    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_acquire_tx(
                  &pool, &tx_slots[slot]) == PACKET_BUFFER_OK,
              "direct TX fill succeeds");
    }
    CHECK(packet_buffer_acquire_tx(
              &pool, &tx) == PACKET_BUFFER_EXHAUSTED,
          "direct TX pool exhaustion is rejected");
    for (slot = 0U; slot < TEST_CAPACITY; ++slot) {
        CHECK(packet_buffer_release(
                  &pool, &tx_slots[slot]) == PACKET_BUFFER_OK,
              "direct TX fill releases every buffer once");
    }
    CHECK(packet_buffer_free_count(&pool) == TEST_CAPACITY &&
              owner_count(PACKET_BUFFER_OWNER_FREE) == TEST_CAPACITY,
          "direct TX failure cycle leaks no ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stale and no-leak cycle perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_pool_validation_and_lifecycle();
    test_exhaustion_and_receive_failure();
    test_handoff_and_transmit_failure();
    test_stale_descriptor_and_no_leak_cycle();

    if (failures == 0) {
        printf("Packet buffer host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_PACKET_BUFFER_HOST_TEST"
#endif
