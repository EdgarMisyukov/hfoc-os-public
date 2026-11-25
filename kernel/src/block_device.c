#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t block_device_byte_size(
    const struct block_device *device)
{
    return (uint64_t)device->block_size * device->block_count;
}

enum block_device_status block_device_validate(
    const struct block_device *device)
{
    if (device == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    if (device->read_fn == NULL || device->write_fn == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (device->block_size < BLOCK_DEVICE_BLOCK_SIZE_MIN ||
        device->block_size > BLOCK_DEVICE_BLOCK_SIZE_MAX ||
        (device->block_size & (device->block_size - 1U)) != 0U) {
        return BLOCK_DEVICE_BAD_BLOCK_SIZE;
    }
    if (device->block_count < 1U ||
        device->block_count > BLOCK_DEVICE_BLOCK_COUNT_MAX) {
        return BLOCK_DEVICE_BAD_BLOCK_COUNT;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status block_device_request_validate(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    enum block_device_status status;

    if (device == NULL || buffer == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    status = block_device_validate(device);
    if (status != BLOCK_DEVICE_OK) {
        return status;
    }
    const uint64_t byte_size = block_device_byte_size(device);
    if (length == 0U) {
        return BLOCK_DEVICE_INVALID_LENGTH;
    }
    if ((offset % device->block_size) != 0U ||
        (length % device->block_size) != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    if (offset >= byte_size || length > byte_size - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return BLOCK_DEVICE_OK;
}

enum block_device_status block_device_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    const enum block_device_status status =
        block_device_request_validate(device, offset, buffer, length);

    if (status != BLOCK_DEVICE_OK) {
        return status;
    }
    return device->read_fn(device, offset, buffer, length);
}

enum block_device_status block_device_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const enum block_device_status status =
        block_device_request_validate(device, offset, buffer, length);

    if (status != BLOCK_DEVICE_OK) {
        return status;
    }
    return device->write_fn(device, offset, buffer, length);
}

enum block_device_status block_device_flush(
    struct block_device *device)
{
    const enum block_device_status status =
        block_device_validate(device);

    if (status != BLOCK_DEVICE_OK) {
        return status;
    }
    if (device->flush_fn == NULL) {
        return BLOCK_DEVICE_UNSUPPORTED;
    }
    return device->flush_fn(device);
}

const char *block_device_status_string(enum block_device_status status) {
    switch (status) {
    case BLOCK_DEVICE_OK:
        return "BLOCK_DEVICE_OK";
    case BLOCK_DEVICE_NULL:
        return "BLOCK_DEVICE_NULL";
    case BLOCK_DEVICE_BAD_DEVICE:
        return "BLOCK_DEVICE_BAD_DEVICE";
    case BLOCK_DEVICE_BAD_BLOCK_SIZE:
        return "BLOCK_DEVICE_BAD_BLOCK_SIZE";
    case BLOCK_DEVICE_BAD_BLOCK_COUNT:
        return "BLOCK_DEVICE_BAD_BLOCK_COUNT";
    case BLOCK_DEVICE_UNALIGNED:
        return "BLOCK_DEVICE_UNALIGNED";
    case BLOCK_DEVICE_INVALID_LENGTH:
        return "BLOCK_DEVICE_INVALID_LENGTH";
    case BLOCK_DEVICE_OUT_OF_RANGE:
        return "BLOCK_DEVICE_OUT_OF_RANGE";
    case BLOCK_DEVICE_IO_ERROR:
        return "BLOCK_DEVICE_IO_ERROR";
    case BLOCK_DEVICE_UNSUPPORTED:
        return "BLOCK_DEVICE_UNSUPPORTED";
    default:
        return "BLOCK_DEVICE_UNKNOWN";
    }
}
