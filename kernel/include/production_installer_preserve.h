#ifndef HFOC_PRODUCTION_INSTALLER_PRESERVE_H
#define HFOC_PRODUCTION_INSTALLER_PRESERVE_H

#include <stdbool.h>
#include <stddef.h>

#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "installer_boot_menu.h"
#include "installer_gpt_preserve.h"
#include "production_installer.h"

/*
 * ITEM-396 production install path that preserves the existing system.
 *
 * `production_installer` plans and executes the appliance's own layout
 * (ITEM-391).  These entry points are the same controller for the install that
 * happens on a disk somebody else already uses:
 *
 *   1. `production_installer_plan_preserve()` runs after
 *      `production_installer_plan_layout()` in
 *      `PRODUCTION_INSTALLER_MODE_DUAL_BOOT`.  It turns the discovered systems
 *      and the loader inventory of the shared ESP into the boot menu the
 *      appliance will write, records which systems stay selectable, and stores
 *      the summary the preview publishes.  It touches no device.
 *   2. `production_installer_publish_preserve_record()` publishes the
 *      machine-readable plan - preserved entries, added roles, the free region
 *      used, the menu entries and the Limine configuration itself - inside one
 *      serial sequence, before the first write.
 *   3. `production_installer_execute_layout_preserving()` writes the partition
 *      table through `installer_gpt_preserve_write()`, which refuses everything
 *      it cannot preserve before it writes a byte.
 *   4. the installed boot set is staged into the reused ESP by the ITEM-397
 *      boot stage (`production_installer_boot_stage_run()` with the reused
 *      range), which is the only writer of those files: it binds the volume as
 *      it is, never formats it, preserves the two paths the set takes over under
 *      documented names, and refuses a volume without room for the set from a
 *      read-only free-cluster pre-flight before its first write.
 *
 * The two tables (`installer_gpt_preserve_table` and the menu) are caller-owned
 * storage: they are about 12 KiB and 2 KiB, so the install flow owns them as
 * statics and passes them in, and this module never keeps them on the stack.
 * ITEM-394, which runs this on an ordinary boot, calls the four entry points in
 * that order and owns exactly those two objects.
 */

#define PRODUCTION_INSTALLER_PRESERVE_VERSION 1U

/*
 * `device_fn` hands the menu scan the block device of a discovered disk id, or
 * NULL when this run has no device for it.  The scan only reads through it - the
 * acceptance host's Debian disk is read for its loader and never written - and
 * `context` is the caller's.
 */
enum production_installer_status production_installer_plan_preserve(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    installer_boot_menu_device_fn device_fn,
    void *device_context,
    const struct installer_boot_menu_entry_source *appliance,
    const struct installer_boot_menu_entry_source *recovery,
    struct installer_boot_menu *menu,
    struct installer_boot_menu_inventory *inventory);

/*
 * Publish the plan that will be written.  The table is checked against the plan
 * first (`installer_gpt_preserve_check()`, read-only), so the record states the
 * preserved entries, the added roles and the free region the writer will use,
 * and a plan that cannot be executed is refused here - before the first write -
 * rather than after it.
 */
enum production_installer_status
production_installer_publish_preserve_record(
    struct production_installer *installer,
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table,
    const struct installer_boot_menu *menu,
    const struct installer_boot_menu_inventory *inventory);

enum production_installer_status
production_installer_execute_layout_preserving(
    struct production_installer *installer,
    struct block_device *device,
    struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table);

/*
 * The preserved ESP's range and the reuse request the writer needs, taken from
 * the contracted summary the controller planned.  A summary without a reused ESP
 * role yields `present == false`.
 */
enum production_installer_status production_installer_preserve_reuse(
    const struct production_installer *installer,
    struct installer_gpt_preserve_reuse *reuse);

/* The ESP the discovery found on the target disk, ready to be bound. */
enum production_installer_status production_installer_preserve_esp(
    const struct production_installer *installer,
    const struct installer_discovery_report *report,
    uint64_t *first_lba,
    uint64_t *length_lba);

#endif
