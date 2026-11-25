#ifndef HFOC_APPLIANCE_STATE_BLOCK_DEVICE_H
#define HFOC_APPLIANCE_STATE_BLOCK_DEVICE_H

#include "appliance_state_layout.h"
#include "block_device.h"

#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_STATE_BLOCK_SIZE 512U

enum appliance_state_block_device_status {
    APPLIANCE_STATE_BLOCK_DEVICE_OK = 0,
    APPLIANCE_STATE_BLOCK_DEVICE_NULL = 1,
    APPLIANCE_STATE_BLOCK_DEVICE_BAD_DEVICE = 2,
    APPLIANCE_STATE_BLOCK_DEVICE_UNSUPPORTED_BLOCK_SIZE = 3,
    APPLIANCE_STATE_BLOCK_DEVICE_BAD_RANGE = 4,
    APPLIANCE_STATE_BLOCK_DEVICE_OUT_OF_RANGE = 5,
};

struct appliance_state_block_device {
    struct block_device parent;
    uint64_t block_offset;
    uint64_t block_count;
    struct block_device device;
};

enum appliance_state_block_device_status
appliance_state_block_device_bind_range(
    struct block_device *parent,
    uint64_t block_offset,
    uint64_t block_count,
    struct appliance_state_block_device *subdevice);
enum appliance_state_block_device_status
appliance_state_block_device_bind(
    struct block_device *parent,
    const struct appliance_state_layout_entry *entry,
    struct appliance_state_block_device *subdevice);
const struct block_device *appliance_state_block_device_view(
    const struct appliance_state_block_device *subdevice);
enum block_device_status appliance_state_block_device_read(
    struct appliance_state_block_device *subdevice,
    uint64_t offset,
    void *buffer,
    size_t length);
enum block_device_status appliance_state_block_device_write(
    struct appliance_state_block_device *subdevice,
    uint64_t offset,
    const void *buffer,
    size_t length);
enum block_device_status appliance_state_block_device_flush(
    struct appliance_state_block_device *subdevice);
const char *appliance_state_block_device_status_string(
    enum appliance_state_block_device_status status);

#endif
