#ifndef HFOC_APPLIANCE_LOOP_HEARTBEAT_H
#define HFOC_APPLIANCE_LOOP_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-428: the main loop says it is alive.
 *
 * The 2026-09-16 hardware run's boot log ended at `ticks=1000`, ten seconds
 * after the installed system booted, and "the machine is up but has nothing
 * new to report" looked exactly like "the machine stopped": every record in
 * the log was a state change, so a loop that had nothing to change published
 * nothing at all.
 *
 * This module owns the one record that is published whether or not anything
 * changed.  It is a pure interval decision - no clock of its own, no state
 * beyond the last interval it announced - so a host proof can pin the
 * cadence, and the loop renders the record it approves:
 *
 *   [PRODUCTION-LOOP] state=RUNNING ticks=<n> stage=<n> interval=<n>
 *
 * A stream of these records proves the loop reached the tick it names; the
 * first record after a silence is the one that says when the loop came back,
 * and a log whose last record is a heartbeat says the machine was alive at
 * that tick and that whatever stopped it stopped it inside the loop.
 */

/*
 * The interval is a compromise a boot log can live with: often enough that a
 * machine which is up is never silent for long, rare enough that the log's
 * fixed extent (511 slots on the appliance's own ESP) keeps hours of history
 * when the network has nothing to say.  The QEMU proof under TCG runs many
 * times slower than wall time, so it is also the interval that lets the proof
 * see the cadence at all.
 */
#define APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS 10000U

struct appliance_loop_heartbeat {
    bool started;
    uint32_t last_ms;
    uint32_t interval_index;
};

/*
 * True when this turn of the loop must publish the next heartbeat, in which
 * case `interval_index` is advanced by one.  The first call always publishes,
 * so the boot itself is on the record; every later call publishes once
 * APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS have passed on the monotonic
 * millisecond clock the loop is driven with.  A clock that does not advance -
 * the same clock every timer in the appliance is driven by - never publishes
 * a second heartbeat, which is itself the finding.
 */
bool appliance_loop_heartbeat_due(
    struct appliance_loop_heartbeat *heartbeat,
    uint32_t now_ms);

#endif
