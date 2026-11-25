#include "appliance_update_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-376 production binding for the durable update record.  See the header.
 */

static bool appliance_update_store_copy_read(
    void *context,
    uint32_t copy,
    uint8_t *block,
    size_t length)
{
    struct appliance_update_store *store =
        (struct appliance_update_store *)context;

    if (store == NULL || !store->bound || copy >= APPLIANCE_UPDATE_STORE_BLOCKS ||
        length != APPLIANCE_UPDATE_RECORD_BLOCK_SIZE) {
        return false;
    }
    return block_device_read(
               store->device,
               (store->block_offset + (uint64_t)copy) *
                   (uint64_t)store->device->block_size,
               block,
               length) == BLOCK_DEVICE_OK;
}

static bool appliance_update_store_copy_write(
    void *context,
    uint32_t copy,
    const uint8_t *block,
    size_t length)
{
    struct appliance_update_store *store =
        (struct appliance_update_store *)context;

    if (store == NULL || !store->bound || copy >= APPLIANCE_UPDATE_STORE_BLOCKS ||
        length != APPLIANCE_UPDATE_RECORD_BLOCK_SIZE) {
        return false;
    }
    return block_device_write(
               store->device,
               (store->block_offset + (uint64_t)copy) *
                   (uint64_t)store->device->block_size,
               block,
               length) == BLOCK_DEVICE_OK;
}

static bool appliance_update_store_flush(void *context)
{
    struct appliance_update_store *store =
        (struct appliance_update_store *)context;

    if (store == NULL || !store->bound) {
        return false;
    }
    return block_device_flush(store->device) == BLOCK_DEVICE_OK;
}

const struct appliance_update_record_io *appliance_update_store_record_io(
    struct appliance_update_store *store)
{
    if (!appliance_update_store_is_bound(store)) {
        return NULL;
    }
    memset(&store->io, 0, sizeof(store->io));
    store->io.context = store;
    store->io.read = appliance_update_store_copy_read;
    store->io.write = appliance_update_store_copy_write;
    store->io.flush = appliance_update_store_flush;
    return &store->io;
}

bool appliance_update_store_is_bound(
    const struct appliance_update_store *store)
{
    return store != NULL && store->bound &&
           store->version == APPLIANCE_UPDATE_STORE_VERSION;
}

enum appliance_update_store_status appliance_update_store_bind(
    struct appliance_update_store *store,
    struct block_device *device,
    uint64_t block_offset)
{
    if (store == NULL || device == NULL) {
        return APPLIANCE_UPDATE_STORE_NULL;
    }
    /*
     * A failed (re)bind invalidates the store: a stale binding must never
     * keep answering record operations after the caller asked for another
     * range.
     */
    memset(store, 0, sizeof(*store));
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != APPLIANCE_UPDATE_RECORD_BLOCK_SIZE) {
        return APPLIANCE_UPDATE_STORE_BAD_DEVICE;
    }
    if (block_offset > device->block_count ||
        device->block_count - block_offset <
            APPLIANCE_UPDATE_STORE_BLOCKS) {
        return APPLIANCE_UPDATE_STORE_BAD_RANGE;
    }

    store->version = APPLIANCE_UPDATE_STORE_VERSION;
    store->device = device;
    store->block_offset = block_offset;
    store->bound = true;
    return APPLIANCE_UPDATE_STORE_OK;
}

const char *appliance_update_store_status_string(
    enum appliance_update_store_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_STORE_OK:
        return "OK";
    case APPLIANCE_UPDATE_STORE_NULL:
        return "NULL";
    case APPLIANCE_UPDATE_STORE_BAD_DEVICE:
        return "BAD_DEVICE";
    case APPLIANCE_UPDATE_STORE_BAD_RANGE:
        return "BAD_RANGE";
    case APPLIANCE_UPDATE_STORE_BAD_STATE:
        return "BAD_STATE";
    default:
        return NULL;
    }
}
