#include "appliance_state_block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum block_device_status appliance_state_block_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct appliance_state_block_device *subdevice =
        (struct appliance_state_block_device *)device->context;
    uint64_t byte_count;

    if (subdevice == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    byte_count = subdevice->block_count * APPLIANCE_STATE_BLOCK_SIZE;
    if (offset >= byte_count ||
        length > byte_count - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_read(
        &subdevice->parent,
        subdevice->block_offset * APPLIANCE_STATE_BLOCK_SIZE + offset,
        buffer, length);
}

static enum block_device_status appliance_state_block_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct appliance_state_block_device *subdevice =
        (struct appliance_state_block_device *)device->context;
    uint64_t byte_count;

    if (subdevice == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    byte_count = subdevice->block_count * APPLIANCE_STATE_BLOCK_SIZE;
    if (offset >= byte_count ||
        length > byte_count - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_write(
        &subdevice->parent,
        subdevice->block_offset * APPLIANCE_STATE_BLOCK_SIZE + offset,
        buffer, length);
}

static enum block_device_status appliance_state_block_flush(
    struct block_device *device)
{
    struct appliance_state_block_device *subdevice =
        (struct appliance_state_block_device *)device->context;

    if (subdevice == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    return block_device_flush(&subdevice->parent);
}

enum appliance_state_block_device_status
appliance_state_block_device_bind_range(
    struct block_device *parent,
    uint64_t block_offset,
    uint64_t block_count,
    struct appliance_state_block_device *subdevice)
{
    enum block_device_status status;

    if (parent == NULL || subdevice == NULL) {
        return APPLIANCE_STATE_BLOCK_DEVICE_NULL;
    }
    status = block_device_validate(parent);
    if (status != BLOCK_DEVICE_OK) {
        return APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE;
    }
    if (parent->block_size != APPLIANCE_STATE_BLOCK_SIZE) {
        return APPLIANCE_STATE_BLOCK_DEVICE_UNSUPPORTED_BLOCK_SIZE;
    }
    if (block_count == 0U) {
        return APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE;
    }
    if (block_offset > parent->block_count ||
        block_count > parent->block_count - block_offset) {
        return APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE;
    }

    memset(subdevice, 0, sizeof(*subdevice));
    subdevice->parent = *parent;
    subdevice->block_offset = block_offset;
    subdevice->block_count = block_count;
    subdevice->device.block_size = APPLIANCE_STATE_BLOCK_SIZE;
    subdevice->device.block_count = block_count;
    subdevice->device.context = subdevice;
    subdevice->device.read_fn = appliance_state_block_read;
    subdevice->device.write_fn = appliance_state_block_write;
    subdevice->device.flush_fn = appliance_state_block_flush;
    return APPLIANCE_STATE_BLOCK_DEVICE_OK;
}

enum appliance_state_block_device_status
appliance_state_block_device_bind(
    struct block_device *parent,
    const struct appliance_state_layout_entry *entry,
    struct appliance_state_block_device *subdevice)
{
    if (parent == NULL || entry == NULL || subdevice == NULL) {
        return APPLIANCE_STATE_BLOCK_DEVICE_NULL;
    }
    if (entry->namespace_id ==
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE ||
        entry->store_id == 0U ||
        entry->block_count == 0U ||
        entry->format_version != APPLIANCE_STATE_LAYOUT_ENTRY_VERSION) {
        return APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE;
    }
    return appliance_state_block_device_bind_range(
        parent, entry->block_offset, entry->block_count, subdevice);
}

const struct block_device *appliance_state_block_device_view(
    const struct appliance_state_block_device *subdevice)
{
    if (subdevice == NULL) {
        return NULL;
    }
    return &subdevice->device;
}

enum block_device_status appliance_state_block_device_read(
    struct appliance_state_block_device *subdevice,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    if (subdevice == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    return block_device_read(&subdevice->device, offset, buffer, length);
}

enum block_device_status appliance_state_block_device_write(
    struct appliance_state_block_device *subdevice,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    if (subdevice == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    return block_device_write(&subdevice->device, offset, buffer, length);
}

enum block_device_status appliance_state_block_device_flush(
    struct appliance_state_block_device *subdevice)
{
    if (subdevice == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    return block_device_flush(&subdevice->device);
}

const char *appliance_state_block_device_status_string(
    enum appliance_state_block_device_status status)
{
    switch (status) {
    case APPLIANCE_STATE_BLOCK_DEVICE_OK:
        return "APPLIANCE_STATE_BLOCK_DEVICE_OK";
    case APPLIANCE_STATE_BLOCK_DEVICE_NULL:
        return "APPLIANCE_STATE_BLOCK_DEVICE_NULL";
    case APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE:
        return "APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE";
    case APPLIANCE_STATE_BLOCK_DEVICE_UNSUPPORTED_BLOCK_SIZE:
        return "APPLIANCE_STATE_BLOCK_DEVICE_UNSUPPORTED_BLOCK_SIZE";
    case APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE:
        return "APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE";
    case APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE:
        return "APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE";
    default:
        return NULL;
    }
}
