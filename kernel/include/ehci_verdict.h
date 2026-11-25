#ifndef HFOC_EHCI_VERDICT_H
#define HFOC_EHCI_VERDICT_H

/*
 * ITEM-562: two controllers, two verdicts, one machine answer.
 *
 * The owner's laptop has two Intel EHCI functions (`00:1A.0` and `00:1D.0`), and
 * the hardware runs proved that a single global verdict is not a reading of the
 * machine: `ehci_status()`/`ehci_detail()` kept one pair that every controller
 * overwrote, so the last controller scanned answered for both, and the start
 * evidence kept one snapshot, so the controller that was shown changed between
 * runs (`00:1A.0` once, `00:1D.0` the next).  A reader could not tell "this
 * controller's walk stopped at a reset" from "the other controller never had a
 * device on a port".
 *
 * This module is the aggregation, kept pure so a host proof can run it with two
 * controllers in both scan orders:
 *
 *   * every controller's own status and detail are preserved (nothing is
 *     overwritten by a later controller, and a specific refusal is never
 *     replaced by a generic one);
 *   * the machine's answer is chosen by information, not by position: a bound
 *     medium wins, then the first controller that got as far as touching a
 *     device, then the first concrete refusal in scan order;
 *   * the per-controller list is rendered for the refusal record, so the `diag`
 *     line names both controllers instead of whichever came last.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ehci.h"
#include "ehci_walk.h"

/* One controller, as the scan saw it.  `status`/`detail` are that controller's
   own verdict; `outcome`/`failure` are the walk's first concrete refusal. */
struct ehci_verdict_entry {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    bool started;                 /* the register stage ran */
    enum ehci_status status;      /* this controller's verdict */
    const char *detail;           /* the string that verdict stands for */
    uint32_t ports;
    uint32_t connected;
    uint32_t attached;
    enum ehci_walk_outcome outcome;
    const char *failure;          /* the walk's first concrete refusal */
    const char *failure_detail;   /* the transport's text for it */
    bool bound;                   /* the guard accepted and bound its medium */
};

/* What the machine reports.  `index` is the entry the answer comes from, so the
   caller can name that controller instead of an anonymous aggregate. */
struct ehci_verdict_choice {
    int index;
    enum ehci_status status;
    const char *detail;
    bool storage_bound;
    uint32_t controllers;
};

/*
 * Fill one entry from a controller's own state and its walk report.  When the
 * walk ran and the controller has no specific status of its own, the outcome
 * supplies the status and the walk's first refusal supplies the detail, so a
 * port that failed at its reset is `PORT_NOT_ENABLED/ROOT_RESET` and not
 * `DESCRIPTOR_FAILED/NO_DEVICE`.
 */
void ehci_verdict_fill(struct ehci_verdict_entry *entry,
                       uint8_t bus,
                       uint8_t device,
                       uint8_t function,
                       bool started,
                       enum ehci_status status,
                       const char *detail,
                       uint32_t ports,
                       const struct ehci_walk_report *report,
                       bool bound);

/* Choose the machine's answer from `count` entries (scan order). */
void ehci_verdict_choose(const struct ehci_verdict_entry *entries,
                         uint32_t count,
                         struct ehci_verdict_choice *choice);

/*
 * The per-controller list as one bounded field, in scan order:
 * `00:1A.0=DESCRIPTOR_FAILED/DEVICE8;00:1D.0=PORT_NOT_ENABLED/ROOT_RESET`.
 * A controller that did not start says `NOT_STARTED` and keeps its own detail.
 * A field that does not fit the buffer is dropped whole, so a truncated list
 * never shows half a controller.  Returns the length written.
 */
size_t ehci_verdict_render(const struct ehci_verdict_entry *entries,
                           uint32_t count,
                           char *buffer,
                           size_t size);

/* The longest per-controller list the refusal record carries. */
#define EHCI_VERDICT_TEXT_MAX 320U

#endif
