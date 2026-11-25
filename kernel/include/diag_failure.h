#ifndef HFOC_DIAG_FAILURE_H
#define HFOC_DIAG_FAILURE_H

/*
 * ITEM-558: the refusal record, kept apart from the attempt, and its two
 * renderings.
 *
 * The hardware run of ITEM-557 printed `DIAG FAILED reason=NO_MEDIUM
 * step=MEDIUM_PROBE` and a detail that named the walk's stage
 * (`ehci=DESCRIPTOR_FAILED/DEVICE8`) but not the transfer that failed - the
 * result the controller reported, the descriptor token, the bytes received, the
 * registers.  The snapshot existed, but only as the serial record
 * `[EHCI-FAIL]`, and on that machine the journal and the boot log were both
 * unbound, so nothing that survives the screen carried it.  The report builder
 * did not include it either.
 *
 * This module is the durable half: a bounded record of the last refusal with
 * the stage, the primary cause and the full detail, written once and never
 * cleared by the next attempt's cleanup.  The console prints its two renderings
 * - a short line that fits one 80-column row and the detail - and the `diag`
 * command prints them again on demand, so a machine whose only output is the
 * framebuffer still hands the operator the reason.
 *
 * Pure by construction: no device, no console, no journal.  The host proof
 * drives it with the walk's own failure text and calls the cleanup, which is
 * how the two properties below are proven rather than asserted:
 *   * a cleanup does not erase the record;
 *   * the short line stays within the row the operator reads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ehci_verdict.h"

/* One row of the 80-column screen, the bound the short line is built to. */
#define DIAG_FAILURE_ROW_MAX 80U
#define DIAG_FAILURE_CODE_MAX 24U
/* The short code that names what actually failed (the transfer result, for a
   transport refusal); empty when the refusal has no such code. */
#define DIAG_FAILURE_PRIMARY_MAX 24U
#define DIAG_FAILURE_DETAIL_MAX 2304U
/* ITEM-615: the commit and storage receipts as their own bounded fields.  The
   detail line already appends them, but a shared status record has to carry
   them without cutting the record that names the stage and primary cause. */
#define DIAG_FAILURE_COMMIT_MAX 96U
#define DIAG_FAILURE_STORAGE_MAX 192U
/* One shared status record: `[DIAG-FAIL]` plus every field above fits the
   diagnostic slot (440 bytes) without a continuation record. */
#define DIAG_FAILURE_STATUS_MAX 440U
/* ITEM-561: the start evidence the refusal carries beside the transfer, so a
   `diag save` that never reached a medium still hands `diag` the PCI words,
   the handoff and the words at stop, reset and start. */
#define DIAG_FAILURE_START_MAX 704U
/* ITEM-562: the per-controller verdict list, so the refusal names both Intel
   EHCI functions instead of whichever was scanned last. */
#define DIAG_FAILURE_CONTROLLERS_MAX EHCI_VERDICT_TEXT_MAX

struct diag_failure {
    bool recorded; /* a save refused at least once this boot */
    char reason[DIAG_FAILURE_CODE_MAX];
    char step[DIAG_FAILURE_CODE_MAX];
    char primary[DIAG_FAILURE_PRIMARY_MAX];
    char detail[DIAG_FAILURE_DETAIL_MAX];
    /* ITEM-615: the receipts the shared status record publishes beside the
       stage and primary cause; NOT_ATTEMPTED until the commit point or the
       bound device actually answered. */
    char commit[DIAG_FAILURE_COMMIT_MAX];
    char storage[DIAG_FAILURE_STORAGE_MAX];
};

/* The attempt's cleanup: clears the attempt's own state and deliberately leaves
   the record alone, so the next `diag save` cannot erase the answer the
   operator is about to be asked about. */
void diag_failure_attempt_reset(struct diag_failure *failure);

/*
 * Record one refusal.  Every field is copied and bounded; a field that does not
 * fit is cut at its bound rather than dropped.  `primary` may be NULL or empty;
 * `primary_detail` (the controller's snapshot, for instance) may be NULL, and
 * so may `start_detail` (the controller's start evidence, ITEM-561) and
 * `controllers` (the per-controller verdict list, ITEM-562).
 */
/*
 * ITEM-571: append the commit point's receipt and the bound device's last
 * failed command to the detail `diag` prints.  Both are RAM snapshots: a
 * machine whose journal medium is what failed still prints them, and neither
 * depends on the journal being writable.
 */
void diag_failure_set_evidence(struct diag_failure *failure,
                               const char *commit,
                               const char *storage);

void diag_failure_set(struct diag_failure *failure,
                      const char *reason,
                      const char *step,
                      const char *primary,
                      const char *ehci_state,
                      const char *ehci_detail,
                      const char *medium,
                      const char *medium_reason,
                      const char *journal_state,
                      const char *journal_reason,
                      const char *primary_detail,
                      const char *start_detail,
                      const char *controllers);

/*
 * The short line the operator reads first: `DIAG FAILED reason=<r> step=<s>`
 * and, when the refusal has one, ` primary=<p>`.  Returns the line's length and
 * never writes more than `size` bytes; a longer combination is cut only between
 * whole fields, so a half field is never shown.
 */
size_t diag_failure_short_line(const struct diag_failure *failure,
                               char *buffer,
                               size_t size);

/* The same rendering under another prefix - the `diag` command prints the
   recorded refusal as `DIAG LAST …`, so the operator can tell the answer to
   the question just asked from the line `diag save` printed. */
size_t diag_failure_short_line_prefixed(const struct diag_failure *failure,
                                        const char *prefix,
                                        char *buffer,
                                        size_t size);

/* The detail line's text, without the `DIAG DETAIL ` prefix. */
size_t diag_failure_detail_line(const struct diag_failure *failure,
                                char *buffer,
                                size_t size);

/*
 * ITEM-615: the retained refusal as one bounded record for the shared
 * composition (`network`, Status, `diag save`, SSH `hfoc diag`), so a later
 * export or remote read names the first failure instead of losing it with the
 * 64-record boot ring.  Read-only: it never resets the attempt or the record.
 * Before any refusal the record is `state=NO_FAILURE reason=NOT_ATTEMPTED`,
 * never a fabricated failure or a stale one.
 */
void diag_failure_status_report(const struct diag_failure *failure,
                                void (*sink)(void *context, const char *line),
                                void *context);

#endif
