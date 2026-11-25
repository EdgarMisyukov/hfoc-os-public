#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DATA_REGION_VERSION_OFFSET 8U
#define DATA_REGION_RESERVED_MIDDLE_OFFSET 9U
#define DATA_REGION_RESERVED_MIDDLE_SIZE 7U
#define DATA_REGION_BLOCK_OFFSET_OFFSET 16U
#define DATA_REGION_BLOCK_COUNT_OFFSET 24U
#define DATA_REGION_RESERVED_TAIL_OFFSET 32U
#define DATA_REGION_RESERVED_TAIL_SIZE 32U

static const unsigned char data_region_magic[DATA_REGION_MAGIC_SIZE] =
    DATA_REGION_MAGIC_BYTES;

static uint64_t data_region_load_le64(const unsigned char *bytes) {
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool data_region_zero_range(const unsigned char *bytes,
                                   size_t offset,
                                   size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[offset + index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool data_region_magic_valid(const unsigned char *header) {
    size_t index;

    for (index = 0U; index < DATA_REGION_MAGIC_SIZE; ++index) {
        if (header[index] != data_region_magic[index]) {
            return false;
        }
    }
    return true;
}

enum data_region_status data_region_discover(
    struct block_device *device,
    struct data_region *region)
{
    unsigned char header[DATA_REGION_HEADER_BLOCK_SIZE];
    enum block_device_status io_status;
    uint64_t block_offset;
    uint64_t block_count;

    if (device == NULL || region == NULL) {
        return DATA_REGION_NULL;
    }
    io_status = block_device_validate(device);
    if (io_status != BLOCK_DEVICE_OK) {
        return DATA_REGION_BAD_DEVICE;
    }
    if (device->block_size != DATA_REGION_HEADER_BLOCK_SIZE) {
        return DATA_REGION_UNSUPPORTED_BLOCK_SIZE;
    }
    io_status = block_device_read(device, 0U, header, sizeof(header));
    if (io_status != BLOCK_DEVICE_OK) {
        return DATA_REGION_IO_ERROR;
    }
    if (!data_region_magic_valid(header)) {
        return DATA_REGION_BAD_MAGIC;
    }
    if (header[DATA_REGION_VERSION_OFFSET] != DATA_REGION_FORMAT_VERSION) {
        return DATA_REGION_UNSUPPORTED_VERSION;
    }
    if (!data_region_zero_range(
            header, DATA_REGION_RESERVED_MIDDLE_OFFSET,
            DATA_REGION_RESERVED_MIDDLE_SIZE) ||
        !data_region_zero_range(
            header, DATA_REGION_RESERVED_TAIL_OFFSET,
            DATA_REGION_RESERVED_TAIL_SIZE)) {
        return DATA_REGION_RESERVED_NONZERO;
    }

    block_offset = data_region_load_le64(
        header + DATA_REGION_BLOCK_OFFSET_OFFSET);
    block_count = data_region_load_le64(
        header + DATA_REGION_BLOCK_COUNT_OFFSET);

    if (block_offset < 1U || block_count < 1U ||
        block_offset > device->block_count ||
        block_count > device->block_count - block_offset) {
        return DATA_REGION_INVALID_LAYOUT;
    }

    region->block_offset = block_offset;
    region->block_count = block_count;
    return DATA_REGION_OK;
}

const char *data_region_status_string(enum data_region_status status) {
    switch (status) {
    case DATA_REGION_OK:
        return "DATA_REGION_OK";
    case DATA_REGION_NULL:
        return "DATA_REGION_NULL";
    case DATA_REGION_BAD_DEVICE:
        return "DATA_REGION_BAD_DEVICE";
    case DATA_REGION_UNSUPPORTED_BLOCK_SIZE:
        return "DATA_REGION_UNSUPPORTED_BLOCK_SIZE";
    case DATA_REGION_IO_ERROR:
        return "DATA_REGION_IO_ERROR";
    case DATA_REGION_BAD_MAGIC:
        return "DATA_REGION_BAD_MAGIC";
    case DATA_REGION_UNSUPPORTED_VERSION:
        return "DATA_REGION_UNSUPPORTED_VERSION";
    case DATA_REGION_RESERVED_NONZERO:
        return "DATA_REGION_RESERVED_NONZERO";
    case DATA_REGION_INVALID_LAYOUT:
        return "DATA_REGION_INVALID_LAYOUT";
    default:
        return "DATA_REGION_UNKNOWN";
    }
}
