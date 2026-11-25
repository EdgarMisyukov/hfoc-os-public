#ifndef HFOC_INSTALLER_STORAGE_SUMMARY_H
#define HFOC_INSTALLER_STORAGE_SUMMARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"
#include "installer_hardware_discovery.h"
#include "installer_layout_contract.h"
#include "installer_storage.h"

/*
 * ITEM-514: the summary before the write.
 *
 * The scheme screen names what the operator wants; this builds the plan that
 * will actually be written and renders it for the last ENTER.  The plan is not
 * a second opinion: the summary calls the same `installer_layout_contract_*`
 * entry points the installer's own `production_installer_plan_layout()` calls
 * with the same report, disk, scheme and released partition, so what the
 * operator reads is the contracted layout the writer executes - roles, sizes,
 * the reused EFI system partition, the reclaimed partition and the space left.
 *
 * The module reads and renders; it writes nothing and reaches no device.  A
 * plan the contract refuses is a named refusal on the same page, never a
 * silently different scheme.
 */
enum installer_storage_summary_status {
    /* The contracted plan was built; the layout below is the plan. */
    INSTALLER_STORAGE_SUMMARY_OK = 0,
    /* The scheme or the released index cannot be planned at all. */
    INSTALLER_STORAGE_SUMMARY_INVALID = 1,
    /* The contract planner refused; `contract_status` names why. */
    INSTALLER_STORAGE_SUMMARY_REFUSED = 2,
};

struct installer_storage_summary {
    uint32_t disk_id;
    enum installer_storage_scheme scheme;
    /* The medium's declaration, so the page and the record name both sources. */
    enum installer_storage_scheme declared;
    /* The partition a MANUAL install gives up; zero for every other scheme. */
    uint32_t released_index;
    enum installer_storage_summary_status status;
    enum installer_layout_contract_status contract_status;
    /* Systems the discovery found on the machine; what the menu keeps. */
    uint32_t existing_system_count;
    struct installer_layout_contract_summary layout;
};

/*
 * Build the summary for one scheme on one disk.  A null report, an unknown
 * disk, NONE, or MANUAL without a released partition is INVALID; a planner
 * refusal is REFUSED with the contract status; otherwise the contracted layout
 * is filled in and the status is OK.
 */
void installer_storage_summary_build(
    struct installer_storage_summary *summary,
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    enum installer_storage_scheme scheme,
    uint32_t released_index);
/*
 * The page the operator confirms: scheme, disk, what is deleted, what is
 * created (one line per contracted role, with its size), the EFI system
 * partition (reused or created), the systems the menu keeps, the entry this
 * install adds, the space left and the fact that nothing is written yet.  A
 * refused plan renders the refusal and its contract status instead of a plan.
 */
size_t installer_storage_summary_lines(
    const struct installer_storage_summary *summary,
    const struct installer_discovery_report *report,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/* The reading of the summary page, for the record, `writes=0`. */
size_t installer_storage_summary_record(
    const struct installer_storage_summary *summary,
    char *buffer,
    size_t size);
const char *installer_storage_summary_status_name(
    enum installer_storage_summary_status status);

#endif
