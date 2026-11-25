#ifndef HFOC_APPLIANCE_PRODUCTION_UPDATE_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_UPDATE_BOOT_H

#include "appliance_state_loader.h"
#include "appliance_update_lifecycle.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-376 ordinary-boot update lifecycle glue.
 *
 * Ordinary boot owns the durable update record: this module binds the record
 * store to the unused RECOVERY_METADATA state-layout store and resolves the
 * boot decision from it, so an update that was interrupted or whose candidate
 * cannot be trusted still boots the known-good slot.
 *
 * Honest limits of the frozen baseline, all published rather than hidden:
 *
 *   - a sealed state volume without an ordinary-boot key provider is not
 *     written through the plaintext path, so the lifecycle publishes
 *     UNAVAILABLE instead of a fabricated record;
 *   - there is no production candidate payload region yet, so the slot
 *     readback is fail-closed: a pending record is never handed over and
 *     rolls back to the known-good slot;
 *   - the known-good slot digest is a declared placeholder because the
 *     baseline has no production payload slot to digest, and it is never
 *     consumed as evidence.
 */

#define APPLIANCE_PRODUCTION_UPDATE_BOOT_VERSION 1U
#define APPLIANCE_PRODUCTION_UPDATE_BOOT_RECORD_BLOCK_OFFSET 1U
#define APPLIANCE_PRODUCTION_UPDATE_BOOT_SLOT_ID 1U

bool appliance_production_update_boot_init(
    struct appliance_state_loader *loader);
const struct appliance_update_lifecycle *
appliance_production_update_boot_lifecycle(void);
/* Was the durable record actually bound, or is the domain unavailable? */
bool appliance_production_update_boot_available(void);
enum appliance_update_lifecycle_status
appliance_production_update_boot_last_status(void);

#endif
