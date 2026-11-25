#ifndef HFOC_PRODUCTION_INSTALLER_LAYOUT_H
#define HFOC_PRODUCTION_INSTALLER_LAYOUT_H

#include <stdbool.h>

#include "production_installer.h"

/*
 * ITEM-391 production publication of the installed footprint.
 *
 * The controller stores the contracted layout summary when it plans the
 * install (`production_installer_plan_layout()`).  This entry point publishes
 * that same summary as a bounded single-line marker record, so the hardware
 * acceptance `INSTALLER` stage and the runbook can quote the installed
 * footprint instead of guessing it.
 *
 * The record is assembled in one buffer and written inside one
 * serial_sequence_begin()/serial_sequence_end() pair, so an interrupt-context
 * trace cannot split a marker line.
 */
enum production_installer_status
production_installer_publish_layout_record(
    const struct production_installer *installer);

#endif
