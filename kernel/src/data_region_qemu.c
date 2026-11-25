#ifdef HFOC_DATA_REGION_QEMU_TEST

#include "block_device.h"
#include "data_region.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DATA_REGION_QEMU_BLOCK_COUNT 8U
#define DATA_REGION_QEMU_CAPACITY \
    (DATA_REGION_HEADER_BLOCK_SIZE * DATA_REGION_QEMU_BLOCK_COUNT)

static unsigned char data_region_qemu_storage[DATA_REGION_QEMU_CAPACITY];
static bool data_region_qemu_fail_read;
static size_t data_region_qemu_read_calls;

static void data_region_qemu_failure(const char *message)
    __attribute__((noreturn));

static void data_region_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[DATA-REGION] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void data_region_qemu_require(bool condition, const char *message) {
    if (!condition) {
        data_region_qemu_failure(message);
    }
}

static void data_region_qemu_store_le64(unsigned char *bytes,
                                        uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void data_region_qemu_install_header(
    bool valid_magic,
    uint8_t version,
    bool reserved_middle,
    bool reserved_tail,
    uint64_t block_offset,
    uint64_t block_count)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memset(data_region_qemu_storage, 0, DATA_REGION_HEADER_BLOCK_SIZE);
    if (valid_magic) {
        memcpy(data_region_qemu_storage, magic, sizeof(magic));
    }
    data_region_qemu_storage[8] = version;
    if (reserved_middle) {
        data_region_qemu_storage[9] = 0x11U;
    }
    data_region_qemu_store_le64(
        data_region_qemu_storage + 16U, block_offset);
    data_region_qemu_store_le64(
        data_region_qemu_storage + 24U, block_count);
    if (reserved_tail) {
        data_region_qemu_storage[32U] = 0x33U;
    }
}

static enum block_device_status data_region_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    ++data_region_qemu_read_calls;
    if (data_region_qemu_fail_read) {
        data_region_qemu_fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, data_region_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status data_region_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    memcpy(data_region_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void data_region_qemu_set_region_sentinel(struct data_region *region) {
    region->block_offset = UINT64_C(0xAAAAAAAAAAAAAAAA);
    region->block_count = UINT64_C(0xBBBBBBBBBBBBBBBB);
}

static bool data_region_qemu_region_is_sentinel(
    const struct data_region *region)
{
    return region->block_offset == UINT64_C(0xAAAAAAAAAAAAAAAA) &&
           region->block_count == UINT64_C(0xBBBBBBBBBBBBBBBB);
}

void data_region_qemu_test_init(void)
{
    struct block_device device;
    struct data_region region;
    enum data_region_status status;
    size_t index;

    for (index = 0U; index < DATA_REGION_QEMU_CAPACITY; ++index) {
        data_region_qemu_storage[index] = 0xA5U;
    }
    memset(&device, 0, sizeof(device));
    device.block_size = DATA_REGION_HEADER_BLOCK_SIZE;
    device.block_count = DATA_REGION_QEMU_BLOCK_COUNT;
    device.read_fn = data_region_qemu_read;
    device.write_fn = data_region_qemu_write;

    serial_write("[DATA-REGION] proof init\n");

    data_region_qemu_install_header(true, DATA_REGION_FORMAT_VERSION,
                                    false, false, 2U, 4U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_OK &&
            data_region_qemu_read_calls == 1U &&
            region.block_offset == 2U && region.block_count == 4U,
        "valid region publication");
    serial_write("[DATA-REGION] valid region published\n");

    data_region_qemu_install_header(false, DATA_REGION_FORMAT_VERSION,
                                    false, false, 2U, 4U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_BAD_MAGIC &&
            data_region_qemu_region_is_sentinel(&region),
        "malformed magic rejection");
    serial_write("[DATA-REGION] malformed magic rejected\n");

    data_region_qemu_install_header(
        true, (uint8_t)(DATA_REGION_FORMAT_VERSION + 1U), false, false,
        2U, 4U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_UNSUPPORTED_VERSION &&
            data_region_qemu_region_is_sentinel(&region),
        "unsupported version rejection");
    serial_write("[DATA-REGION] unsupported version rejected\n");

    data_region_qemu_install_header(true, DATA_REGION_FORMAT_VERSION,
                                    true, true, 2U, 4U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_RESERVED_NONZERO &&
            data_region_qemu_region_is_sentinel(&region),
        "reserved byte rejection");
    serial_write("[DATA-REGION] reserved bytes rejected\n");

    data_region_qemu_install_header(true, DATA_REGION_FORMAT_VERSION,
                                    false, false, 0U, 4U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_INVALID_LAYOUT &&
            data_region_qemu_region_is_sentinel(&region),
        "overlapping layout rejection");
    serial_write("[DATA-REGION] overlapping layout rejected\n");

    data_region_qemu_install_header(
        true, DATA_REGION_FORMAT_VERSION, false, false,
        DATA_REGION_QEMU_BLOCK_COUNT - 1U, 2U);
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_INVALID_LAYOUT &&
            data_region_qemu_region_is_sentinel(&region),
        "out-of-range layout rejection");
    serial_write("[DATA-REGION] out-of-range layout rejected\n");

    data_region_qemu_install_header(true, DATA_REGION_FORMAT_VERSION,
                                    false, false, 2U, 4U);
    data_region_qemu_fail_read = true;
    data_region_qemu_set_region_sentinel(&region);
    status = data_region_discover(&device, &region);
    data_region_qemu_require(
        status == DATA_REGION_IO_ERROR &&
            data_region_qemu_read_calls == 7U &&
            !data_region_qemu_fail_read &&
            data_region_qemu_region_is_sentinel(&region),
        "injected read failure rejection");
    serial_write("[DATA-REGION] injected read failure rejected\n");

    serial_write("[DATA-REGION] clean shutdown\n");
    serial_write("[DATA-REGION] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_data_region_qemu_translation_unit_marker;
#endif
