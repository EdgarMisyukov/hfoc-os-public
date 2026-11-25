#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_STATE_BLOCK_DEVICE_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 256U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_RANGE_OFFSET 10U
#define TEST_RANGE_COUNT 32U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

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

static void init_fixture(struct fixture_device *fixture)
{
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    for (index = 0U; index < sizeof(fixture->storage); ++index) {
        fixture->storage[index] = (unsigned char)(index & 0xFFU);
    }
}

static struct block_device make_device(struct fixture_device *fixture)
{
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

static void test_bind_and_io(void)
{
    struct fixture_device fixture;
    struct block_device parent = make_device(&fixture);
    struct appliance_state_block_device sub;
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char expected[TEST_BLOCK_SIZE];
    struct appliance_state_layout_entry entry;

    init_fixture(&fixture);
    CHECK(appliance_state_block_device_bind_range(
              NULL, TEST_RANGE_OFFSET, TEST_RANGE_COUNT, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_NULL,
          "bind range rejects null parent");
    CHECK(appliance_state_block_device_bind_range(
              &parent, TEST_RANGE_OFFSET, TEST_RANGE_COUNT, NULL) ==
              APPLIANCE_STATE_BLOCK_DEVICE_NULL,
          "bind range rejects null subdevice");
    CHECK(appliance_state_block_device_bind(
              &parent, NULL, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_NULL,
          "bind entry rejects null entry");

    CHECK(appliance_state_block_device_bind_range(
              &parent, TEST_RANGE_OFFSET, TEST_RANGE_COUNT, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK &&
              sub.block_offset == TEST_RANGE_OFFSET &&
              sub.block_count == TEST_RANGE_COUNT &&
              sub.device.block_size == APPLIANCE_STATE_BLOCK_SIZE &&
              sub.device.block_count == TEST_RANGE_COUNT,
          "valid range binds exact bounded view");
    CHECK(appliance_state_block_device_view(&sub) != NULL &&
              block_device_validate(
                  appliance_state_block_device_view(&sub)) ==
                  BLOCK_DEVICE_OK,
          "bound view validates as block device");

    memset(buffer, 0, sizeof(buffer));
    CHECK(appliance_state_block_device_read(
              &sub, 0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_OK &&
              fixture.read_calls == 1U,
          "bounded read reaches physical parent");
    memcpy(expected,
           fixture.storage +
               (size_t)(TEST_RANGE_OFFSET * TEST_BLOCK_SIZE),
           sizeof(expected));
    CHECK(memcmp(buffer, expected, sizeof(expected)) == 0,
          "bounded read returns exact physical range");

    memset(buffer, 0x5AU, sizeof(buffer));
    CHECK(appliance_state_block_device_write(
              &sub, TEST_BLOCK_SIZE, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK &&
              memcmp(fixture.storage +
                         (size_t)((TEST_RANGE_OFFSET + 1U) *
                                  TEST_BLOCK_SIZE),
                     buffer, sizeof(buffer)) == 0,
          "bounded write mutates only selected parent range");

    CHECK(appliance_state_block_device_flush(&sub) == BLOCK_DEVICE_OK &&
              fixture.flush_calls == 1U,
          "bounded flush propagates to physical parent");

    memset(&entry, 0, sizeof(entry));
    entry.namespace_id =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG;
    entry.store_id = APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME;
    entry.block_offset = TEST_RANGE_OFFSET;
    entry.block_count = TEST_RANGE_COUNT;
    entry.format_version = APPLIANCE_STATE_LAYOUT_ENTRY_VERSION;
    memset(&sub, 0, sizeof(sub));
    CHECK(appliance_state_block_device_bind(
              &parent, &entry, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "layout entry binds to subdevice");
}

static void test_bounds_and_failures(void)
{
    struct fixture_device fixture;
    struct block_device parent = make_device(&fixture);
    struct appliance_state_block_device sub;
    unsigned char buffer[TEST_BLOCK_SIZE];
    struct appliance_state_layout_entry entry;

    init_fixture(&fixture);
    CHECK(appliance_state_block_device_bind_range(
              &parent, TEST_BLOCK_COUNT, 1U, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE,
          "range at parent end is out of range");
    CHECK(appliance_state_block_device_bind_range(
              &parent, TEST_BLOCK_COUNT - 1U, 2U, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE,
          "range crossing parent end is out of range");
    CHECK(appliance_state_block_device_bind_range(
              &parent, 0U, 0U, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE,
          "zero-length range is rejected");

    CHECK(appliance_state_block_device_bind_range(
              &parent, TEST_RANGE_OFFSET, TEST_RANGE_COUNT, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "valid range binds for bounds test");
    memset(buffer, 0xA5U, sizeof(buffer));
    CHECK(appliance_state_block_device_read(
              &sub, sub.device.block_count * APPLIANCE_STATE_BLOCK_SIZE,
              buffer, sizeof(buffer)) == BLOCK_DEVICE_OUT_OF_RANGE &&
              fixture.read_calls == 0U &&
              buffer[0] == 0xA5U,
          "out-of-range read is rejected without parent I/O");
    CHECK(appliance_state_block_device_write(
              &sub, sub.device.block_count * APPLIANCE_STATE_BLOCK_SIZE,
              buffer, sizeof(buffer)) == BLOCK_DEVICE_OUT_OF_RANGE &&
              fixture.write_calls == 0U,
          "out-of-range write is rejected without parent mutation");

    fixture.fail_read = true;
    CHECK(appliance_state_block_device_read(
              &sub, 0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              !fixture.fail_read,
          "parent read failure propagates");
    fixture.fail_write = true;
    CHECK(appliance_state_block_device_write(
              &sub, 0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              !fixture.fail_write,
          "parent write failure propagates");
    fixture.fail_flush = true;
    CHECK(appliance_state_block_device_flush(&sub) == BLOCK_DEVICE_IO_ERROR &&
              !fixture.fail_flush,
          "parent flush failure propagates");

    parent.block_size = 1024U;
    CHECK(appliance_state_block_device_bind_range(
              &parent, 0U, 1U, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_UNSUPPORTED_BLOCK_SIZE,
          "non-512 parent is rejected");
    parent.block_size = TEST_BLOCK_SIZE;
    parent.block_count = 0U;
    CHECK(appliance_state_block_device_bind_range(
              &parent, 0U, 1U, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE,
          "invalid parent is rejected");

    memset(&entry, 0, sizeof(entry));
    entry.namespace_id =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG;
    entry.store_id = APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME;
    entry.block_offset = 0U;
    entry.block_count = 1U;
    entry.format_version = APPLIANCE_STATE_LAYOUT_ENTRY_VERSION;
    CHECK(appliance_state_block_device_bind(
              &parent, &entry, &sub) ==
              APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE,
          "invalid layout parent bind is rejected");

    CHECK(appliance_state_block_device_status_string(
              APPLIANCE_STATE_BLOCK_DEVICE_OK) != NULL &&
              strcmp(appliance_state_block_device_status_string(
                         APPLIANCE_STATE_BLOCK_DEVICE_OK),
                     "APPLIANCE_STATE_BLOCK_DEVICE_OK") == 0,
          "subdevice status string is exact");
    CHECK(appliance_state_block_device_status_string(
              (enum appliance_state_block_device_status)99U) == NULL,
          "subdevice rejects undefined status string");
}

int main(void)
{
    test_bind_and_io();
    test_bounds_and_failures();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance state block device host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance state block device host tests: ALL OK");
    return 0;
}

#endif
