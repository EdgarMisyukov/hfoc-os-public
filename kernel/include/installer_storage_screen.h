#ifndef HFOC_INSTALLER_STORAGE_SCREEN_H
#define HFOC_INSTALLER_STORAGE_SCREEN_H

#include "installer_hardware_discovery.h"
#include "appliance_language.h"
#include "appliance_storage_report.h"
#include "installer_storage.h"

/*
 * ITEM-489: the storage screen on the machine.
 *
 * The model in `installer_storage.c` is a value machine and is proven on the
 * host.  This header is the half that needs a machine: it draws the screen
 * through the display abstraction (or prints it, when there is no framebuffer),
 * reads the operator's keys, and publishes the reading.  It partitions nothing,
 * formats nothing and writes nothing - the record it publishes says which disk
 * was chosen, and that is all it does.
 *
 * ITEM-490: the operator entry of the install medium makes this screen
 * mandatory.  `INSTALLER_STORAGE_NO_DEADLINE` waits for a person instead of
 * expiring, the chosen disk is handed back to the caller that owns the write,
 * and a session the operator left without choosing returns nothing at all.
 */

/* Wait for an operator: the mandatory screen, with no end. */
#define INSTALLER_STORAGE_NO_DEADLINE UINT32_C(0)

/*
 * Show the screen and let the operator decide, for at most `timeout_ms`, or
 * without a deadline when that is `INSTALLER_STORAGE_NO_DEADLINE`.
 *
 * ITEM-495: the decision is the two-valued answer the caller needs, not a bare
 * boolean.  `INSTALLER_STORAGE_DECISION_CONFIRMED` means the operator chose a
 * disk; `chosen` is filled with that disk's own row - the name, identity,
 * capacity, transport and table the operator saw - so a caller that writes a
 * disk takes its target from here and from nowhere else.
 * `INSTALLER_STORAGE_DECISION_NONE` means nothing was accepted: the operator
 * left the screen, no disk could be chosen, or a bounded session expired.
 *
 * ITEM-514: choosing the disk is no longer the confirmation.  The disk is
 * named here and the confirmation moved to the summary page - the whole
 * install plan, not one line about the disk - so a preserving install never
 * has to read "everything on this disk will be deleted" about a disk whose
 * partitions will stay.  The destructive page still exists, on the erase
 * scheme that actually destroys.
 */
enum installer_storage_decision installer_storage_session_choose(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    struct installer_storage_disk *chosen,
    enum appliance_language language);
/*
 * ITEM-500: the same mandatory screen, with one caller-supplied line printed
 * above the disks - the reason the operator is being asked ("the disk this
 * medium declared is not on this machine").  A NULL or empty note is exactly
 * `installer_storage_session_choose()`; the note is borrowed, never copied.
 */
enum installer_storage_decision installer_storage_session_choose_note(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    struct installer_storage_disk *chosen,
    const char *note,
    enum appliance_language language);
/*
 * ITEM-489: the bounded session, for a caller that must not wait.
 */
enum installer_storage_decision installer_storage_session(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    enum appliance_language language);
/*
 * ITEM-501: the second mandatory screen - the partitions of the chosen disk
 * that may be given up.  It opens only when the caller knows the disk carries at
 * least one releasable partition; `INSTALLER_STORAGE_DECISION_CONFIRMED` means
 * the operator read the warning page for one partition and accepted it, and
 * `released_index` is that partition's discovery index.  `NONE` means the
 * operator chose to keep every partition - the free-space path - or that a
 * bounded session expired; nothing is given up and nothing is written either
 * way.
 */
enum installer_storage_decision installer_storage_session_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t timeout_ms,
    uint32_t *released_index,
    uint64_t required_lba,
    enum appliance_language language);
/* The reading of the last session, for a caller that wants to publish it again. */
void installer_storage_publish(const struct installer_discovery_report *report);
/*
 * ITEM-490: the reading again, with the disk the operator chose selected - the
 * record an operator entry publishes after the screen, so the disk that is about
 * to be written is named on the disk the machine came up on.
 */
void installer_storage_publish_choice(
    const struct installer_discovery_report *report,
    uint32_t disk_id);
/*
 * ITEM-500: the page a refused operator entry shows instead of booting on in
 * silence.  It names the refusal (`reason` is the stable token the records
 * carry), says the medium cannot install on this machine, and waits for a key -
 * with a bounded wait, so a machine nobody is standing at still boots.  It
 * writes nothing and decides nothing; the refusal has already been decided and
 * published.
 *
 * ITEM-518: `observed` is the probe's own report, printed between the refusal
 * and the way out, so `NO_DISK` stops being a code the person cannot act on.
 * NULL renders the "not probed" line instead of inventing zeroes.
 *
 * ITEM-541: `evidence` carries the medium verdict and the journal's own status
 * plus the bounded tail of its RAM ring - the two facts a machine without
 * serial cannot otherwise report.  NULL renders the "no evidence" tokens; the
 * page itself is built by `installer_storage_refusal_lines()`.
 */
void installer_storage_refusal_show(
    const char *reason,
    const struct appliance_storage_report *observed,
    const struct installer_refusal_evidence *evidence,
    enum appliance_language language);

/*
 * ITEM-514: the scheme screen - the list between the disk and the write.
 *
 * `declared` is the scheme the medium declared (NONE when it declared none);
 * the cursor starts on the declared scheme, or on the disk's own suggestion
 * when there is no declaration.  `required_lba` is the footprint the
 * preserving layout needs and `minimum_media_lba` the smallest disk the clean
 * layout fits on; `contents_unknown` says the partition table could not be
 * read, so the erase page names the unknown contents instead of looking at a
 * blank disk.  `CONFIRMED` fills `chosen` with the scheme the operator
 * selected; for MANUAL the caller opens the partition screen next.
 * `NONE` means the operator left the schemes, so the caller writes nothing.
 */
enum installer_storage_decision installer_storage_session_scheme(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    uint64_t required_lba,
    uint64_t minimum_media_lba,
    bool contents_unknown,
    uint32_t timeout_ms,
    enum installer_storage_scheme *chosen,
    enum appliance_language language);
/*
 * ITEM-514: the summary page - the last ENTER before the write.
 *
 * It renders the plan the installer's own planner builds for this scheme and
 * this released partition, publishes the reading, and answers `CONFIRMED` only
 * when the operator accepts it.  A plan the contract refuses renders the named
 * refusal and answers `NONE`; nothing is ever confirmed that could not be
 * planned.
 */
enum installer_storage_decision installer_storage_session_summary(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    enum installer_storage_scheme scheme,
    uint32_t released_index,
    uint32_t timeout_ms,
    enum appliance_language language);

#endif
