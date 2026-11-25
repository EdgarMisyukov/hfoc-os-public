#ifndef HFOC_APPLIANCE_UPDATE_STORE_H
#define HFOC_APPLIANCE_UPDATE_STORE_H

#include "appliance_update_record.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-376 production binding for the durable update record.
 *
 * The record module deliberately owns no block device: it takes bounded copy
 * read/write/flush callbacks.  This adapter binds those callbacks to two
 * consecutive 512-byte blocks of a real block device, so ordinary boot can
 * place the record in a state-layout store while the record itself stays
 * host-testable and device-agnostic.
 *
 * The binding is strict: the device must be valid, its block size must be the
 * record block size and the range must fit inside the device.  A store that
 * cannot be bound never produces a record, so an unavailable region fails
 * closed instead of silently writing somewhere else.
 */

#define APPLIANCE_UPDATE_STORE_VERSION 1U
#define APPLIANCE_UPDATE_STORE_BLOCKS \
    APPLIANCE_UPDATE_RECORD_COPY_COUNT

enum appliance_update_store_status {
    APPLIANCE_UPDATE_STORE_OK = 0,
    APPLIANCE_UPDATE_STORE_NULL = 1,
    APPLIANCE_UPDATE_STORE_BAD_DEVICE = 2,
    APPLIANCE_UPDATE_STORE_BAD_RANGE = 3,
    APPLIANCE_UPDATE_STORE_BAD_STATE = 4,
};

struct appliance_update_store {
    uint32_t version;
    bool bound;
    struct block_device *device;
    uint64_t block_offset;
    /* Caller-owned callback table; the store never uses hidden state. */
    struct appliance_update_record_io io;
};

enum appliance_update_store_status appliance_update_store_bind(
    struct appliance_update_store *store,
    struct block_device *device,
    uint64_t block_offset);
/* The record callbacks bound to this store; NULL when it is not bound. */
const struct appliance_update_record_io *appliance_update_store_record_io(
    struct appliance_update_store *store);
bool appliance_update_store_is_bound(
    const struct appliance_update_store *store);
const char *appliance_update_store_status_string(
    enum appliance_update_store_status status);

#endif
