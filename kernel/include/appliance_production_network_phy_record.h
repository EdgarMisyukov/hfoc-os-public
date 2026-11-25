#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_H

/*
 * ITEM-590: one composition for the `[NET-PHY]` record, and one record for it.
 *
 * ITEM-589 gave the record its content - the state, the XID, the access method
 * and the first error - so a Realtek part whose PHY was skipped or refused
 * names itself instead of leaving `link=DOWN` as the only evidence.  The record
 * was still composed twice (the boot journal's emitter in kernel.c and the
 * diagnostic report's composer), and it was still written *after* the
 * `[PRODUCTION-NET]` record's terminator: the console's journal mirror ignores
 * fragments that arrive after a record ended - by design, since they belong to
 * no record - so the serial stream carried `[NET-PHY]` and the boot journal on
 * the medium carried nothing.  That is exactly what the owner's Toshiba ran
 * into: 512 slots of boot log, not one `[NET-PHY]` line in them.
 *
 * This module is the answer to both halves:
 *
 *   * `appliance_production_network_phy_record_line()` composes the one line,
 *     so the boot journal and the exported report cannot drift apart;
 *   * `appliance_production_network_phy_record_publish()` opens the record,
 *     writes the line and its terminator, and ends it - so the record is its
 *     own record and no caller can append it to a finished one.
 *
 * `appliance_production_network_phy_info_from_driver()` derives the published
 * answer from the driver's own state (`struct rtl8168_chip`,
 * `struct rtl8168_phy_state`), which is what makes the three states reachable
 * from a host proof's RTL8168 model: the record's content and the record's
 * presence in the journal are provable without the laptop.
 */

#include "appliance_production_network_boot.h"
#include "rtl8168.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The composed line's bound: the longest form is the base fields plus the
   register block, well under one journal slot's text. */
#define APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX 384U

/* Where the serial copy of the record goes: `serial_write()` in the kernel, a
   capture in a host proof. */
typedef void (*appliance_production_network_phy_serial_fn)(const char *text);

/*
 * The product's answer for a Realtek part, from the driver's own state.  A part
 * whose PHY was never configured is SKIPPED with the reason that decided it; a
 * part whose configuration was attempted is CONFIGURED or REFUSED by the
 * result and the state's own validity.  `first_error` is NULL while no PHY
 * error was seen, so the record says `NONE` and not the name of success.
 */
void appliance_production_network_phy_info_from_driver(
    const struct rtl8168_chip *chip,
    const struct rtl8168_phy_state *phy,
    const struct rtl8168_cold_boot_report *boot,
    uint32_t link_attempts,
    struct appliance_production_network_phy_info *info);

/*
 * The record's text without its terminator.  Returns zero when there is nothing
 * to print (a backend with no Realtek PHY answer); `buffer` is then left empty.
 * A line that would not fit the caller's buffer is refused whole rather than
 * published cut.  The register fields are written only for a CONFIGURED part: a
 * skipped or refused PHY never answers for registers it was not asked.
 */
size_t appliance_production_network_phy_record_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);

/*
 * Emit the record as its own journal record: one serial sequence, the
 * journal-only funnel opened and ended around the line and its terminator.
 * Returns false when there was nothing to print.  The caller must have ended
 * the record this one follows.
 */
bool appliance_production_network_phy_record_publish(
    const struct appliance_production_network_phy_info *info,
    appliance_production_network_phy_serial_fn serial);

/*
 * ITEM-592: the cold start's evidence, as its own bounded records:
 *
 *   [NET-PHY-BOOT]    the confirmed profile, what each stage did, the first
 *                     failure and the firmware dependency that was skipped;
 *   [NET-PHY-GIG]     the 1000BASE-T advertisement (register 9) and the
 *                     partner's (register 10), each with its read verdict;
 *   [NET-PHY-SAMPLES] the raw PHYstatus before and now, the initial BMCR, the
 *                     successive BMSR readings and whether link/AN were seen.
 *
 * `..._boot_line()`, `..._gigabit_line()` and `..._sample_line()` compose one
 * line each (zero when there is nothing to print); `..._detail_publish()`
 * emits them as records of their own, after the `[NET-PHY]` record.
 */
size_t appliance_production_network_phy_boot_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
size_t appliance_production_network_phy_gigabit_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
size_t appliance_production_network_phy_sample_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
size_t appliance_production_network_phy_failure_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
/* Preceding completed page write and raw verification payload. Present only
 * for the first failed page verification; completion is not bank acceptance. */
size_t appliance_production_network_phy_page_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
size_t appliance_production_network_phy_select_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size);
void appliance_production_network_phy_detail_publish(
    const struct appliance_production_network_phy_info *info,
    appliance_production_network_phy_serial_fn serial);

#endif
