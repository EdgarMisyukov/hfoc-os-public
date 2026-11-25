#ifndef HFOC_APPLIANCE_PANEL_GATE_H
#define HFOC_APPLIANCE_PANEL_GATE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-516: the management gate's own records.
 *
 * The owner's decision of 2026-10-10 (variant "b") lets an operator save the
 * decision "the panel is available after boot without a local login".  The
 * setting lives in the system record (`v1:security.panel.after-boot=open`);
 * this module owns the *records* the appliance speaks about it, in one place,
 * so the boot, the local console and the acceptance report cannot drift:
 *
 *   [PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY
 *       the boot read `open` and published the stored credential itself; the
 *       panel answers over the network without a local login.
 *   [PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET
 *       the default: the listener stays withheld until a local login proves
 *       the secret, exactly as ITEM-401 has always behaved.
 *   [PRODUCTION-SECURITY] gate=PENDING action=open confirmation_required
 *       the local console accepted `gate open` and waits for the explicit
 *       confirmation; nothing durable is written yet.
 *   [PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET
 *       the console wrote `open` after a successful local login.
 *   [PRODUCTION-SECURITY] gate=DISABLED source=CONSOLE
 *       the console wrote the default `closed`.
 *   [PRODUCTION-SECURITY] gate=REFUSED reason=LOCAL_SECRET_REQUIRED
 *       `gate open` without a proven local secret; nothing was written.
 *
 * The acceptance report reads the `gate=...` line, so the exact bytes are
 * part of the contract and are pinned by a host proof.
 */
#define APPLIANCE_PANEL_GATE_VERSION 1U
#define APPLIANCE_PANEL_GATE_RECORD_TAG "[PRODUCTION-SECURITY]"
/*
 * The refusal a panel attempt to open the gate answers with.  It is stable
 * because an operator and a script both read it: opening is local-only, and
 * the panel can only ever close.
 */
#define APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED \
    "GATE_LOCAL_SECRET_REQUIRED"
/* The values the durable record may hold. */
#define APPLIANCE_PANEL_GATE_VALUE_OPEN "open"
#define APPLIANCE_PANEL_GATE_VALUE_CLOSED "closed"

enum appliance_panel_gate_event {
    APPLIANCE_PANEL_GATE_EVENT_NONE = 0,
    /* The boot published the stored credential because the record said open. */
    APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED = 1,
    /* The boot withheld the listener: the default, awaiting a local secret. */
    APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED = 2,
    /* The local console accepted `gate open` and waits for confirmation. */
    APPLIANCE_PANEL_GATE_EVENT_PENDING = 3,
    /* The local console wrote `open` after a proven local secret. */
    APPLIANCE_PANEL_GATE_EVENT_ENABLED = 4,
    /* The console (or the panel) wrote the default `closed`. */
    APPLIANCE_PANEL_GATE_EVENT_DISABLED = 5,
    /* `gate open` without a proven local secret: nothing was written. */
    APPLIANCE_PANEL_GATE_EVENT_REFUSED = 6,
};

/* The one record of an event, or NULL for an event the module does not name. */
const char *appliance_panel_gate_record(enum appliance_panel_gate_event event);
/*
 * The record of a setting change that carries the `open` flag rather than the
 * event: `ENABLED` for open and `DISABLED` for closed.  It exists so the boot
 * and the console never spell the two records themselves.
 */
const char *appliance_panel_gate_setting_record(bool open);
const char *appliance_panel_gate_event_string(
    enum appliance_panel_gate_event event);

#endif
