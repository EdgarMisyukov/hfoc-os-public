#include "block_device.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_DATA_REGION_HOST_TEST

#define TEST_BLOCK_SIZE DATA_REGION_HEADER_BLOCK_SIZE
#define TEST_BLOCK_COUNT 8U
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
    size_t read_calls;
    size_t write_calls;
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
    memcpy(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void init_fixture(struct fixture_device *fixture) {
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    for (index = 0U; index < TEST_CAPACITY; ++index) {
        fixture->storage[index] = 0xA5U;
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
    return device;
}

static void store_le64(unsigned char *bytes, uint64_t value) {
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void install_header(struct fixture_device *fixture,
                           bool valid_magic,
                           uint8_t version,
                           bool reserved_middle,
                           bool reserved_tail,
                           uint64_t block_offset,
                           uint64_t block_count)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memset(fixture->storage, 0, TEST_BLOCK_SIZE);
    if (valid_magic) {
        memcpy(fixture->storage, magic, sizeof(magic));
    }
    fixture->storage[8] = version;
    if (reserved_middle) {
        fixture->storage[9] = 0x11U;
    }
    store_le64(fixture->storage + 16U, block_offset);
    store_le64(fixture->storage + 24U, block_count);
    if (reserved_tail) {
        fixture->storage[32U] = 0x33U;
    }
}

static bool region_is_sentinel(const struct data_region *region) {
    return region->block_offset == UINT64_C(0xAAAAAAAAAAAAAAAA) &&
           region->block_count == UINT64_C(0xBBBBBBBBBBBBBBBB);
}

static void set_region_sentinel(struct data_region *region) {
    region->block_offset = UINT64_C(0xAAAAAAAAAAAAAAAA);
    region->block_count = UINT64_C(0xBBBBBBBBBBBBBBBB);
}

static void test_null_and_invalid_device(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct data_region region;

    init_fixture(&fixture);
    device = make_device(&fixture);
    set_region_sentinel(&region);

    CHECK(data_region_discover(NULL, &region) == DATA_REGION_NULL &&
              region_is_sentinel(&region),
          "discover rejects null device");
    CHECK(data_region_discover(&device, NULL) == DATA_REGION_NULL,
          "discover rejects null region");

    device.read_fn = NULL;
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_BAD_DEVICE &&
              fixture.read_calls == 0U && region_is_sentinel(&region),
          "invalid device is rejected without header read");

    device = make_device(&fixture);
    device.block_count = 0U;
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_BAD_DEVICE &&
              fixture.read_calls == 0U && region_is_sentinel(&region),
          "zero block count is rejected without header read");

    device = make_device(&fixture);
    device.block_size = TEST_BLOCK_SIZE * 2U;
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_UNSUPPORTED_BLOCK_SIZE &&
              fixture.read_calls == 0U && region_is_sentinel(&region),
          "unsupported block size is rejected without header read");

    CHECK(data_region_status_string(DATA_REGION_OK) != NULL &&
              strcmp(data_region_status_string(DATA_REGION_OK),
                     "DATA_REGION_OK") == 0,
          "data-region OK status string is exact");
    CHECK(data_region_status_string(DATA_REGION_IO_ERROR) != NULL &&
              strcmp(data_region_status_string(DATA_REGION_IO_ERROR),
                     "DATA_REGION_IO_ERROR") == 0,
          "data-region I/O error status string is exact");
    CHECK(data_region_status_string(DATA_REGION_INVALID_LAYOUT) != NULL &&
              strcmp(data_region_status_string(DATA_REGION_INVALID_LAYOUT),
                     "DATA_REGION_INVALID_LAYOUT") == 0,
          "data-region invalid layout status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "null and invalid device handling performs no allocation");
}

static void test_valid_region_is_published(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct data_region region;

    init_fixture(&fixture);
    device = make_device(&fixture);
    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   2U, 4U);
    set_region_sentinel(&region);

    CHECK(data_region_discover(&device, &region) == DATA_REGION_OK &&
              fixture.read_calls == 1U &&
              region.block_offset == 2U && region.block_count == 4U,
          "valid fixed layout publishes exact data region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   1U, 1U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_OK &&
              region.block_offset == 1U && region.block_count == 1U,
          "single-block data region publishes exact bounds");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid discovery performs no allocation");
}

static void test_malformed_layout_never_publishes(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct data_region region;

    init_fixture(&fixture);
    device = make_device(&fixture);

    install_header(&fixture, false, DATA_REGION_FORMAT_VERSION, false, false,
                   2U, 4U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_BAD_MAGIC &&
              region_is_sentinel(&region),
          "wrong magic never publishes a region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION + 1U,
                   false, false, 2U, 4U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_UNSUPPORTED_VERSION &&
              region_is_sentinel(&region),
          "unsupported version never publishes a region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, true, false,
                   2U, 4U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_RESERVED_NONZERO &&
              region_is_sentinel(&region),
          "nonzero middle reserved bytes never publish a region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, true,
                   2U, 4U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_RESERVED_NONZERO &&
              region_is_sentinel(&region),
          "nonzero tail reserved bytes never publish a region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   0U, 4U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_INVALID_LAYOUT && region_is_sentinel(&region),
          "region overlapping header block never publishes");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   1U, 0U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_INVALID_LAYOUT && region_is_sentinel(&region),
          "zero block count never publishes a region");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   TEST_BLOCK_COUNT, 1U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_INVALID_LAYOUT && region_is_sentinel(&region),
          "region starting at device end never publishes");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   TEST_BLOCK_COUNT - 1U, 2U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_INVALID_LAYOUT && region_is_sentinel(&region),
          "region extending beyond device end never publishes");

    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   UINT64_MAX, 1U);
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) ==
              DATA_REGION_INVALID_LAYOUT && region_is_sentinel(&region),
          "maximum block offset never publishes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed layout handling performs no allocation");
}

static void test_injected_read_failure_never_publishes(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct data_region region;

    init_fixture(&fixture);
    device = make_device(&fixture);
    install_header(&fixture, true, DATA_REGION_FORMAT_VERSION, false, false,
                   2U, 4U);

    fixture.fail_read = true;
    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_IO_ERROR &&
              fixture.read_calls == 1U && !fixture.fail_read &&
              region_is_sentinel(&region),
          "injected header read failure never publishes a region");

    set_region_sentinel(&region);
    CHECK(data_region_discover(&device, &region) == DATA_REGION_OK &&
              region.block_offset == 2U && region.block_count == 4U,
          "discovery succeeds after injected failure");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "injected failure handling performs no allocation");
}

int main(void) {
    test_null_and_invalid_device();
    test_valid_region_is_published();
    test_malformed_layout_never_publishes();
    test_injected_read_failure_never_publishes();

    if (failures != 0) {
        fprintf(stderr, "Data-region host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Data-region host tests: ALL OK");
    return 0;
}

#endif
