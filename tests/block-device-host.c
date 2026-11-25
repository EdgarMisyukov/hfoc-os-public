#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_BLOCK_DEVICE_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 2U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    bool fail_read;
    bool fail_write;
    bool fail_flush;
    size_t read_calls;
    size_t write_calls;
    size_t flush_calls;
};

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    if (fixture->fail_read) {
        fixture->fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    if (fixture->fail_write) {
        fixture->fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->flush_calls;
    if (fixture->fail_flush) {
        fixture->fail_flush = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static void init_fixture(struct fixture_device *fixture) {
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    for (index = 0U; index < TEST_BLOCK_SIZE; ++index) {
        fixture->storage[index] = 0x11U;
    }
    for (index = TEST_BLOCK_SIZE; index < TEST_CAPACITY; ++index) {
        fixture->storage[index] = 0x22U;
    }
}

static struct block_device make_device(struct fixture_device *fixture) {
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static bool all_bytes(const unsigned char *buffer,
                      size_t length,
                      unsigned char value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

static void test_validation_and_status_strings(void) {
    struct fixture_device fixture;
    struct block_device device;
    unsigned char buffer[TEST_BLOCK_SIZE];

    init_fixture(&fixture);
    device = make_device(&fixture);

    CHECK(block_device_validate(NULL) == BLOCK_DEVICE_NULL,
          "device validate rejects null device");
    CHECK(block_device_read(NULL, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_NULL,
          "device read rejects null device");
    CHECK(block_device_write(NULL, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_NULL,
          "device write rejects null device");
    CHECK(block_device_read(&device, 0U, NULL, sizeof(buffer)) ==
              BLOCK_DEVICE_NULL,
          "device read rejects null buffer");
    CHECK(block_device_write(&device, 0U, NULL, sizeof(buffer)) ==
              BLOCK_DEVICE_NULL,
          "device write rejects null buffer");
    CHECK(block_device_flush(NULL) == BLOCK_DEVICE_NULL,
          "device flush rejects null device");

    device.block_size = 0U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_SIZE,
          "zero block size is rejected");
    device.block_size = TEST_BLOCK_SIZE - 1U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_SIZE,
          "below-minimum block size is rejected");
    device.block_size = BLOCK_DEVICE_BLOCK_SIZE_MAX + 1U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_SIZE,
          "above-maximum block size is rejected");
    device.block_size = 768U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_SIZE,
          "non-power-of-two block size is rejected");
    device = make_device(&fixture);
    device.block_count = 0U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_COUNT,
          "zero block count is rejected");
    device = make_device(&fixture);
    device.block_count = BLOCK_DEVICE_BLOCK_COUNT_MAX + 1U;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_BLOCK_COUNT,
          "above-maximum block count is rejected");
    device = make_device(&fixture);
    device.read_fn = NULL;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_DEVICE,
          "missing read function is rejected");
    device = make_device(&fixture);
    device.write_fn = NULL;
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_DEVICE,
          "missing write function is rejected");
    CHECK(block_device_validate(&device) == BLOCK_DEVICE_BAD_DEVICE,
          "invalid device cannot be used for I/O");

    CHECK(block_device_status_string(BLOCK_DEVICE_OK) != NULL &&
              strcmp(block_device_status_string(BLOCK_DEVICE_OK),
                     "BLOCK_DEVICE_OK") == 0,
          "block device OK status string is exact");
    CHECK(block_device_status_string(BLOCK_DEVICE_IO_ERROR) != NULL &&
              strcmp(block_device_status_string(BLOCK_DEVICE_IO_ERROR),
                     "BLOCK_DEVICE_IO_ERROR") == 0,
          "block device I/O error status string is exact");
    CHECK(block_device_status_string(BLOCK_DEVICE_UNSUPPORTED) != NULL &&
              strcmp(block_device_status_string(BLOCK_DEVICE_UNSUPPORTED),
                     "BLOCK_DEVICE_UNSUPPORTED") == 0,
          "block device unsupported status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "block-device validation performs no allocation");
}

static void test_flush_semantics(void) {
    struct fixture_device fixture;
    struct block_device device;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(block_device_flush(&device) == BLOCK_DEVICE_OK &&
              fixture.flush_calls == 1U,
          "device flush invokes backend and returns success");

    device = make_device(&fixture);
    device.flush_fn = NULL;
    CHECK(block_device_flush(&device) == BLOCK_DEVICE_UNSUPPORTED &&
              fixture.flush_calls == 1U,
          "device flush without backend reports unsupported");

    fixture.fail_flush = true;
    CHECK(block_device_flush(&device) == BLOCK_DEVICE_UNSUPPORTED,
          "unsupported flush does not consume injected failure");

    device = make_device(&fixture);
    fixture.fail_flush = true;
    CHECK(block_device_flush(&device) == BLOCK_DEVICE_IO_ERROR &&
              fixture.flush_calls == 2U && !fixture.fail_flush,
          "injected flush failure maps to stable I/O error");
    CHECK(block_device_flush(&device) == BLOCK_DEVICE_OK &&
              fixture.flush_calls == 3U,
          "flush succeeds after injected failure");
}

static void test_alignment_and_range_never_invoke_device(void) {
    struct fixture_device fixture;
    struct block_device device;
    unsigned char buffer[TEST_BLOCK_SIZE];

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(buffer, 0xA5, sizeof(buffer));

    CHECK(block_device_read(&device, 256U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_UNALIGNED &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "unaligned offset is rejected without read operation");
    CHECK(block_device_write(&device, 256U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_UNALIGNED &&
              fixture.write_calls == 0U,
          "unaligned write offset is rejected without write operation");
    CHECK(block_device_read(&device, 0U, buffer, TEST_BLOCK_SIZE - 1U) ==
              BLOCK_DEVICE_UNALIGNED &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "unaligned length is rejected without read operation");
    CHECK(block_device_read(&device, 0U, buffer, 0U) ==
              BLOCK_DEVICE_INVALID_LENGTH &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "zero length is rejected without read operation");
    CHECK(block_device_read(&device, TEST_CAPACITY, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OUT_OF_RANGE &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "offset at device end is rejected without read operation");
    CHECK(block_device_read(&device, TEST_BLOCK_SIZE, buffer,
                            TEST_CAPACITY) == BLOCK_DEVICE_OUT_OF_RANGE &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "length beyond device end is rejected without read operation");
    CHECK(block_device_read(&device, TEST_BLOCK_SIZE, buffer,
                            TEST_BLOCK_SIZE + 1U) ==
              BLOCK_DEVICE_UNALIGNED &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "unaligned oversized length is rejected before range check");
    CHECK(block_device_read(&device, UINT64_MAX - (TEST_BLOCK_SIZE - 1U),
                            buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OUT_OF_RANGE &&
              fixture.read_calls == 0U && all_bytes(buffer, sizeof(buffer),
                                                    0xA5U),
          "aligned maximum offset is rejected without arithmetic overflow");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "alignment and range rejection performs no allocation");
}

static void test_valid_read_and_write(void) {
    struct fixture_device fixture;
    struct block_device device;
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char write_data[TEST_BLOCK_SIZE];

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(buffer, 0, sizeof(buffer));
    memset(write_data, 0x33, sizeof(write_data));

    CHECK(block_device_read(&device, TEST_BLOCK_SIZE, buffer,
                            sizeof(buffer)) == BLOCK_DEVICE_OK &&
              fixture.read_calls == 1U && all_bytes(buffer, sizeof(buffer),
                                                    0x22U),
          "block-aligned in-range read publishes exact block");
    CHECK(block_device_write(&device, 0U, write_data, sizeof(write_data)) ==
              BLOCK_DEVICE_OK &&
              fixture.write_calls == 1U &&
              all_bytes(fixture.storage, sizeof(write_data), 0x33U),
          "block-aligned in-range write publishes exact block");
    CHECK(block_device_read(&device, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK &&
              all_bytes(buffer, sizeof(buffer), 0x33U),
          "published write is observable through read");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid block I/O performs no allocation");
}

static void test_injected_failures_leave_state_unchanged(void) {
    struct fixture_device fixture;
    struct block_device device;
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char write_data[TEST_BLOCK_SIZE];
    unsigned char original[TEST_BLOCK_SIZE];

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(buffer, 0xA5, sizeof(buffer));
    memset(write_data, 0x44, sizeof(write_data));
    memcpy(original, fixture.storage, sizeof(original));

    fixture.fail_read = true;
    CHECK(block_device_read(&device, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_IO_ERROR &&
              fixture.read_calls == 1U && !fixture.fail_read &&
              all_bytes(buffer, sizeof(buffer), 0xA5U),
          "injected read failure leaves destination unchanged");
    CHECK(block_device_read(&device, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK &&
              all_bytes(buffer, sizeof(buffer), 0x11U),
          "read succeeds explicitly after injected failure");

    fixture.fail_write = true;
    CHECK(block_device_write(&device, 0U, write_data, sizeof(write_data)) ==
              BLOCK_DEVICE_IO_ERROR &&
              fixture.write_calls == 1U && !fixture.fail_write &&
              memcmp(fixture.storage, original, sizeof(original)) == 0,
          "injected write failure leaves device storage unchanged");
    CHECK(block_device_write(&device, 0U, write_data, sizeof(write_data)) ==
              BLOCK_DEVICE_OK &&
              all_bytes(fixture.storage, sizeof(write_data), 0x44U),
          "write succeeds explicitly after injected failure");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "injected failure handling performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_flush_semantics();
    test_alignment_and_range_never_invoke_device();
    test_valid_read_and_write();
    test_injected_failures_leave_state_unchanged();

    if (failures != 0) {
        fprintf(stderr, "Block-device host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Block-device host tests: ALL OK");
    return 0;
}

#endif
