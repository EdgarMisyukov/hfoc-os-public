#include "appliance_production_update_boot.h"

#ifdef HFOC_PRODUCTION_SERVICES

#include "appliance_persistent_storage_namespace.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "appliance_update_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct appliance_production_update_boot {
    bool initialized;
    bool available;
    enum appliance_update_lifecycle_status last_status;
    struct appliance_state_block_device partition;
    struct appliance_update_store store;
    struct appliance_update_lifecycle lifecycle;
};

static struct appliance_production_update_boot
    appliance_production_update_boot_state;

/*
 * The frozen baseline has no production candidate payload region, so the slot
 * readback is fail-closed: a pending record is never handed over.  A real
 * candidate digest check belongs to the payload-staging work, and until then
 * this module never claims a candidate was verified.
 */
static bool appliance_production_update_boot_slot_intact(
    void *context,
    const struct appliance_update_record_entry *entry)
{
    (void)context;
    (void)entry;
    return false;
}

bool appliance_production_update_boot_init(
    struct appliance_state_loader *loader)
{
    struct appliance_production_update_boot *boot =
        &appliance_production_update_boot_state;
    const struct appliance_state_layout_entry *entry = NULL;
    const struct appliance_update_record_io *record_io;
    struct appliance_update_lifecycle_io io;
    struct appliance_update_record_entry known_good;

    if (boot->initialized) {
        return boot->available;
    }
    boot->initialized = true;
    boot->last_status = APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;

    if (loader == NULL || !loader->initialized || !loader->ready) {
        return false;
    }
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    /*
     * A sealed volume without an ordinary-boot key provider is never written
     * through the plaintext path, so the durable record is unavailable
     * instead of silently unencrypted.
     */
    if (loader->encryption_sealed) {
        boot->last_status = APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
        return false;
    }
#endif
    if (appliance_state_layout_find(
            &loader->layout,
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
            APPLIANCE_STATE_STORE_ID_RECOVERY_METADATA, &entry) !=
            APPLIANCE_STATE_LAYOUT_OK ||
        entry == NULL) {
        return false;
    }
    /*
     * Block 0 of the store is the data-region header, so the two record
     * copies live in the blocks after it.
     */
    if (appliance_state_block_device_bind(
            &loader->partition.device, entry, &boot->partition) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    if (appliance_update_store_bind(
            &boot->store,
            (struct block_device *)appliance_state_block_device_view(
                &boot->partition),
            APPLIANCE_PRODUCTION_UPDATE_BOOT_RECORD_BLOCK_OFFSET) !=
        APPLIANCE_UPDATE_STORE_OK) {
        return false;
    }
    record_io = appliance_update_store_record_io(&boot->store);
    if (record_io == NULL) {
        return false;
    }

    io.context = boot;
    io.record = *record_io;
    io.slot_intact = appliance_production_update_boot_slot_intact;

    memset(&known_good, 0, sizeof(known_good));
    known_good.slot_id = APPLIANCE_PRODUCTION_UPDATE_BOOT_SLOT_ID;
    known_good.generation = APPLIANCE_PRODUCTION_UPDATE_BOOT_VERSION;

    boot->last_status = appliance_update_lifecycle_init(
        &boot->lifecycle,
        APPLIANCE_PRODUCTION_UPDATE_BOOT_VERSION,
        &known_good,
        &io);
    if (boot->last_status != APPLIANCE_UPDATE_LIFECYCLE_OK) {
        return false;
    }
    boot->last_status = appliance_update_lifecycle_resolve(
        &boot->lifecycle, &io);
    boot->available =
        boot->last_status == APPLIANCE_UPDATE_LIFECYCLE_OK;
    return boot->available;
}

const struct appliance_update_lifecycle *
appliance_production_update_boot_lifecycle(void)
{
    struct appliance_production_update_boot *boot =
        &appliance_production_update_boot_state;

    return boot->initialized ? &boot->lifecycle : NULL;
}

bool appliance_production_update_boot_available(void)
{
    return appliance_production_update_boot_state.initialized &&
           appliance_production_update_boot_state.available;
}

enum appliance_update_lifecycle_status
appliance_production_update_boot_last_status(void)
{
    return appliance_production_update_boot_state.last_status;
}

#else

typedef int appliance_production_update_boot_translation_unit_guard;

#endif
