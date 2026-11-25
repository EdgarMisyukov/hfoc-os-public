#include "packet_slice.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PACKET_SLICE_HOST_TEST

#define TEST_COUNT 2U
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

static bool init_active_slot(
    struct packet_pool_slot_descriptor *descriptor)
{
    reset_pool();
    return packet_pool_init(
               &pool, storage, sizeof(storage),
               TEST_COUNT, TEST_CAPACITY,
               TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_OK &&
           packet_pool_reserve(
               &pool, PACKET_POOL_DIRECTION_RX,
               descriptor) == PACKET_POOL_OK &&
           packet_pool_commit(
               &pool, descriptor) == PACKET_POOL_OK;
}

static bool init_slice(
    struct packet_slice *slice,
    struct packet_pool_slot_descriptor *descriptor,
    uint32_t length)
{
    return init_active_slot(descriptor) &&
           packet_slice_init(
               slice, &pool, descriptor, length) == PACKET_SLICE_OK;
}

static bool slice_is(
    const struct packet_slice *slice,
    uint32_t start,
    uint32_t length,
    uint32_t cursor)
{
    return packet_slice_is_valid(slice) &&
           packet_slice_start(slice) == start &&
           packet_slice_length(slice) == length &&
           packet_slice_cursor(slice) == cursor;
}

static void test_status_strings(void)
{
    static const struct {
        enum packet_slice_status status;
        const char *string;
    } statuses[] = {
        {PACKET_SLICE_OK, "PACKET_SLICE_OK"},
        {PACKET_SLICE_NULL, "PACKET_SLICE_NULL"},
        {PACKET_SLICE_BAD_STATE, "PACKET_SLICE_BAD_STATE"},
        {PACKET_SLICE_BAD_POOL, "PACKET_SLICE_BAD_POOL"},
        {PACKET_SLICE_STALE_DESCRIPTOR,
         "PACKET_SLICE_STALE_DESCRIPTOR"},
        {PACKET_SLICE_WRONG_OWNER, "PACKET_SLICE_WRONG_OWNER"},
        {PACKET_SLICE_BAD_LENGTH, "PACKET_SLICE_BAD_LENGTH"},
        {PACKET_SLICE_BAD_RANGE, "PACKET_SLICE_BAD_RANGE"},
        {PACKET_SLICE_BAD_CURSOR, "PACKET_SLICE_BAD_CURSOR"},
        {PACKET_SLICE_UNDERFLOW, "PACKET_SLICE_UNDERFLOW"},
        {PACKET_SLICE_OVERFLOW, "PACKET_SLICE_OVERFLOW"},
        {PACKET_SLICE_OVERLAP, "PACKET_SLICE_OVERLAP"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = packet_slice_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "packet slice status string is exact");
    }
    CHECK(packet_slice_status_string(
              (enum packet_slice_status)12U) == NULL,
          "packet slice undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "packet slice status handling performs no allocation");
}

static void test_null_and_invalid_init(void)
{
    struct packet_pool_slot_descriptor free_slot;
    struct packet_pool_slot_descriptor reserved;
    struct packet_pool_slot_descriptor active;
    struct packet_slice slice;
    struct packet_slice stale_slice;

    reset_pool();
    CHECK(packet_slice_init(
              NULL, &pool, &active, 0U) == PACKET_SLICE_NULL,
          "init rejects null slice");
    CHECK(packet_slice_init(
              &slice, NULL, &active, 0U) == PACKET_SLICE_NULL,
          "init rejects null pool");
    CHECK(packet_slice_init(
              &slice, &pool, NULL, 0U) == PACKET_SLICE_NULL,
          "init rejects null slot descriptor");
    CHECK(packet_slice_init(
              &slice, &pool, &active, 0U) == PACKET_SLICE_BAD_POOL &&
              !packet_slice_is_valid(&slice),
          "init rejects unpublished pool");

    CHECK(init_active_slot(&active),
          "active slot fixture initializes");
    CHECK(packet_pool_slot(
              &pool, 1U, &free_slot) == PACKET_POOL_OK &&
              free_slot.owner == PACKET_POOL_OWNER_FREE &&
              packet_slice_init(
                  &slice, &pool, &free_slot, 0U) ==
                  PACKET_SLICE_WRONG_OWNER,
          "init rejects a free slot");

    reset_pool();
    CHECK(packet_pool_init(
              &pool, storage, sizeof(storage),
              TEST_COUNT, TEST_CAPACITY,
              TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_OK &&
              packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_RX,
                  &reserved) == PACKET_POOL_OK &&
              packet_slice_init(
                  &slice, &pool, &reserved, 0U) ==
                  PACKET_SLICE_WRONG_OWNER,
          "init rejects a reserved slot");

    CHECK(init_active_slot(&active),
          "stale init fixture initializes active slot");
    CHECK(packet_slice_init(
              &slice, &pool, &active, 0U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 0U, 0U),
          "zero-length active slice initializes at headroom");
    CHECK(packet_pool_release(
              &pool, &active) == PACKET_POOL_OK &&
              packet_slice_init(
                  &stale_slice, &pool, &active, 0U) ==
                  PACKET_SLICE_STALE_DESCRIPTOR,
          "init rejects a stale descriptor after release");

    CHECK(init_active_slot(&active),
          "length-limit fixture initializes active slot");
    CHECK(packet_slice_init(
              &slice, &pool, &active, TEST_LENGTH + 1U) ==
              PACKET_SLICE_BAD_LENGTH,
          "init rejects initial length above pool maximum");
    CHECK(packet_slice_init(
              &slice, &pool, &active, TEST_LENGTH) ==
              PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, TEST_LENGTH, 0U),
          "init accepts exact pool maximum length");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "null and invalid init performs no allocation");
}

static void test_append_read_write_seek(void)
{
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    char buffer[32];

    CHECK(init_slice(&slice, &descriptor, 0U),
          "append fixture initializes empty active slice");
    CHECK(packet_slice_append(
              &slice, NULL, 0U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 0U, 0U),
          "zero-length append is a no-op");
    CHECK(packet_slice_append(
              &slice, "hello", 5U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 5U, 0U),
          "append extends slice and leaves cursor at start");
    CHECK(packet_slice_append(
              &slice, " world", 6U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 11U, 0U),
          "second append extends exact end");
    CHECK(packet_slice_seek(
              &slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_read(
                  &slice, buffer, 5U) == PACKET_SLICE_OK &&
              memcmp(buffer, "hello", 5U) == 0 &&
              packet_slice_cursor(&slice) == 5U,
          "seek and read copy exact prefix");
    CHECK(packet_slice_seek(
              &slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_write(
                  &slice, "HELLO", 5U) == PACKET_SLICE_OK &&
              packet_slice_seek(
                  &slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_read(
                  &slice, buffer, 11U) == PACKET_SLICE_OK &&
              memcmp(buffer, "HELLO world", 11U) == 0,
          "write cursor overwrites in place");

    CHECK(packet_slice_seek(
              &slice, 11U) == PACKET_SLICE_OK &&
              packet_slice_cursor(&slice) == 11U,
          "seek accepts cursor at slice end");
    CHECK(packet_slice_seek(
              &slice, 12U) == PACKET_SLICE_BAD_CURSOR &&
              packet_slice_cursor(&slice) == 11U,
          "seek rejects cursor beyond end without mutation");

    CHECK(packet_slice_seek(
              &slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_read(
                  &slice, NULL, 0U) == PACKET_SLICE_OK &&
              packet_slice_cursor(&slice) == 0U,
          "zero-length read is a no-op");
    CHECK(packet_slice_write(
              &slice, NULL, 0U) == PACKET_SLICE_OK &&
              packet_slice_cursor(&slice) == 0U,
          "zero-length write is a no-op");
    CHECK(packet_slice_read(
              &slice, buffer, 12U) == PACKET_SLICE_UNDERFLOW &&
              packet_slice_cursor(&slice) == 0U,
          "read beyond end returns underflow without cursor change");
    CHECK(packet_slice_write(
              &slice, "overflow!!!!!", 12U) == PACKET_SLICE_UNDERFLOW &&
              packet_slice_cursor(&slice) == 0U,
          "write beyond end returns underflow without cursor change");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "append/read/write/seek performs no allocation");
}

static void test_prepend_and_trim(void)
{
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    char buffer[32];

    CHECK(init_slice(&slice, &descriptor, 4U),
          "prepend fixture initializes four-byte active slice");
    CHECK(packet_slice_write(
              &slice, "body", 4U) == PACKET_SLICE_OK &&
              packet_slice_seek(
                  &slice, 0U) == PACKET_SLICE_OK,
          "prepend fixture writes initial payload");
    CHECK(packet_slice_prepend(
              &slice, NULL, 0U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 4U, 0U),
          "zero-length prepend is a no-op");
    CHECK(packet_slice_prepend(
              &slice, "ip", 2U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM - 2U, 6U, 0U),
          "prepend writes before start and keeps zero cursor at first byte");
    CHECK(packet_slice_read(
              &slice, buffer, 6U) == PACKET_SLICE_OK &&
              memcmp(buffer, "ipbody", 6U) == 0,
          "prepend preserves original payload order");

    CHECK(packet_slice_seek(
              &slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_prepend(
                  &slice, "x", 15U) == PACKET_SLICE_OVERFLOW &&
              slice_is(&slice, TEST_HEADROOM - 2U, 6U, 0U),
          "prepend beyond available headroom returns overflow");

    CHECK(packet_slice_append(
              &slice, "X", 1U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM - 2U, 7U, 0U),
          "append after prepend extends slice end");
    CHECK(packet_slice_trim(
              &slice, 0U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM - 2U, 7U, 0U),
          "zero-length trim is a no-op");
    CHECK(packet_slice_trim(
              &slice, 2U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM - 2U, 5U, 0U),
          "trim removes bytes from slice end");
    CHECK(packet_slice_read(
              &slice, buffer, 5U) == PACKET_SLICE_OK &&
              memcmp(buffer, "ipbod", 5U) == 0,
          "trimmed slice exposes exact remaining bytes");
    CHECK(packet_slice_trim(
              &slice, 6U) == PACKET_SLICE_UNDERFLOW &&
              slice_is(&slice, TEST_HEADROOM - 2U, 5U, 5U),
          "trim beyond end returns underflow without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "prepend/trim performs no allocation");
}

static void test_overlap_rejection(void)
{
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    unsigned char *slot_base;
    char buffer[4] = "ext";

    CHECK(init_slice(&slice, &descriptor, 4U),
          "overlap fixture initializes active slice");
    CHECK(packet_slice_write(
              &slice, "abcd", 4U) == PACKET_SLICE_OK &&
              packet_slice_seek(
                  &slice, 0U) == PACKET_SLICE_OK,
          "overlap fixture writes initial payload");
    slot_base = storage +
        (size_t)descriptor.slot * (TEST_HEADROOM + TEST_CAPACITY);

    CHECK(packet_slice_read(
              &slice, slot_base + TEST_HEADROOM, 1U) ==
              PACKET_SLICE_OVERLAP &&
              packet_slice_cursor(&slice) == 0U,
          "read rejects destination overlapping packet storage");
    CHECK(packet_slice_write(
              &slice, slot_base + TEST_HEADROOM, 1U) ==
              PACKET_SLICE_OVERLAP &&
              packet_slice_cursor(&slice) == 0U,
          "write rejects source overlapping packet storage");
    CHECK(packet_slice_append(
              &slice, slot_base + TEST_HEADROOM, 1U) ==
              PACKET_SLICE_OVERLAP &&
              slice_is(&slice, TEST_HEADROOM, 4U, 0U),
          "append rejects source overlapping packet storage");
    CHECK(packet_slice_prepend(
              &slice, slot_base + TEST_HEADROOM - 1U, 1U) ==
              PACKET_SLICE_OVERLAP &&
              slice_is(&slice, TEST_HEADROOM, 4U, 0U),
          "prepend rejects source overlapping packet storage");
    CHECK(packet_slice_append(
              &slice, buffer, 3U) == PACKET_SLICE_OK &&
              slice_is(&slice, TEST_HEADROOM, 7U, 0U),
          "external append buffer remains usable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "overlap rejection performs no allocation");
}

static void test_stale_and_wrong_owner(void)
{
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    char buffer[4];

    CHECK(init_slice(&slice, &descriptor, 4U),
          "stale fixture initializes active slice");
    CHECK(packet_pool_release(
              &pool, &descriptor) == PACKET_POOL_OK &&
              !packet_slice_is_valid(&slice) &&
              packet_slice_length(&slice) == 0U &&
              packet_slice_start(&slice) == 0U &&
              packet_slice_cursor(&slice) == 0U,
          "released slot invalidates slice accessors");
    CHECK(packet_slice_append(
              &slice, "x", 1U) == PACKET_SLICE_STALE_DESCRIPTOR &&
              packet_slice_prepend(
                  &slice, "x", 1U) ==
                  PACKET_SLICE_STALE_DESCRIPTOR &&
              packet_slice_trim(
                  &slice, 1U) == PACKET_SLICE_STALE_DESCRIPTOR &&
              packet_slice_read(
                  &slice, buffer, 1U) ==
                  PACKET_SLICE_STALE_DESCRIPTOR &&
              packet_slice_write(
                  &slice, "x", 1U) ==
                  PACKET_SLICE_STALE_DESCRIPTOR &&
              packet_slice_seek(
                  &slice, 0U) == PACKET_SLICE_STALE_DESCRIPTOR,
          "every stale slice operation fails closed");

    CHECK(init_active_slot(&descriptor),
          "wrong-owner fixture initializes active slot");
    CHECK(packet_slice_init(
              &slice, &pool, &descriptor, 4U) == PACKET_SLICE_OK &&
              packet_pool_transfer(
                  &pool, &descriptor,
                  PACKET_POOL_DIRECTION_TX) == PACKET_POOL_OK &&
              !packet_slice_is_valid(&slice),
          "direction transfer invalidates prior slice ownership view");
    CHECK(packet_slice_read(
              &slice, buffer, 1U) == PACKET_SLICE_WRONG_OWNER &&
              packet_slice_append(
                  &slice, "x", 1U) == PACKET_SLICE_WRONG_OWNER &&
              packet_slice_seek(
                  &slice, 0U) == PACKET_SLICE_WRONG_OWNER,
          "wrong-owner slice operations fail closed");

    CHECK(packet_slice_append(
              NULL, "x", 1U) == PACKET_SLICE_NULL &&
              packet_slice_prepend(
                  NULL, "x", 1U) == PACKET_SLICE_NULL &&
              packet_slice_trim(
                  NULL, 1U) == PACKET_SLICE_NULL &&
              packet_slice_read(
                  NULL, buffer, 1U) == PACKET_SLICE_NULL &&
              packet_slice_write(
                  NULL, "x", 1U) == PACKET_SLICE_NULL &&
              packet_slice_seek(
                  NULL, 0U) == PACKET_SLICE_NULL,
          "null slice operations are rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stale/wrong-owner path performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_invalid_init();
    test_append_read_write_seek();
    test_prepend_and_trim();
    test_overlap_rejection();
    test_stale_and_wrong_owner();

    if (failures == 0) {
        printf("Packet slice and cursor host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_PACKET_SLICE_HOST_TEST"
#endif
