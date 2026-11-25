#include "appliance_panel_gate.h"

/*
 * ITEM-516: one table, so the boot, the console and the proofs all read the
 * same bytes.  The literals are written whole - no `printf`, no concatenation
 * at run time - because the acceptance report greps them exactly.
 */
static const char appliance_panel_gate_boot_published[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG " gate=PUBLISHED source=POLICY";
static const char appliance_panel_gate_boot_closed[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG
    " gate=CLOSED reason=AWAITING_LOCAL_SECRET";
static const char appliance_panel_gate_pending[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG
    " gate=PENDING action=open confirmation_required";
static const char appliance_panel_gate_enabled[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG " gate=ENABLED source=LOCAL_SECRET";
static const char appliance_panel_gate_disabled[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG " gate=DISABLED source=CONSOLE";
static const char appliance_panel_gate_refused[] =
    APPLIANCE_PANEL_GATE_RECORD_TAG
    " gate=REFUSED reason=" APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED;

const char *appliance_panel_gate_record(enum appliance_panel_gate_event event)
{
    switch (event) {
    case APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED:
        return appliance_panel_gate_boot_published;
    case APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED:
        return appliance_panel_gate_boot_closed;
    case APPLIANCE_PANEL_GATE_EVENT_PENDING:
        return appliance_panel_gate_pending;
    case APPLIANCE_PANEL_GATE_EVENT_ENABLED:
        return appliance_panel_gate_enabled;
    case APPLIANCE_PANEL_GATE_EVENT_DISABLED:
        return appliance_panel_gate_disabled;
    case APPLIANCE_PANEL_GATE_EVENT_REFUSED:
        return appliance_panel_gate_refused;
    default:
        return NULL;
    }
}

const char *appliance_panel_gate_setting_record(bool open)
{
    return appliance_panel_gate_record(
        open ? APPLIANCE_PANEL_GATE_EVENT_ENABLED
             : APPLIANCE_PANEL_GATE_EVENT_DISABLED);
}

const char *appliance_panel_gate_event_string(
    enum appliance_panel_gate_event event)
{
    switch (event) {
    case APPLIANCE_PANEL_GATE_EVENT_NONE:
        return "NONE";
    case APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED:
        return "BOOT_PUBLISHED";
    case APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED:
        return "BOOT_CLOSED";
    case APPLIANCE_PANEL_GATE_EVENT_PENDING:
        return "PENDING";
    case APPLIANCE_PANEL_GATE_EVENT_ENABLED:
        return "ENABLED";
    case APPLIANCE_PANEL_GATE_EVENT_DISABLED:
        return "DISABLED";
    case APPLIANCE_PANEL_GATE_EVENT_REFUSED:
        return "REFUSED";
    default:
        return NULL;
    }
}
