#ifndef HFOC_LOOP_BUDGET_H
#define HFOC_LOOP_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-479 (СРОЧНО-28, round 17): the pure half of the loop turn's phase
 * attribution.
 *
 * The owner's 2026-09-25 hardware run (`/srv/hfoc-example/queue/hw8-log-all.txt`)
 * read one loop turn of `input_us=1213704` inside an otherwise healthy 10 ms
 * loop (`turn_us=9998`, `wake_hz=100`, 99.7% halted).  The existing phase
 * budget said "the input phase ate 1.2 s" but not *which* input work did:
 * the driver service and doorbell, the keyboard's decoded events or the
 * pointer's movement and drawing.  The turn's worst phase was likewise not
 * published - `iter_max_us=3159521` named the boot's worst turn, not what
 * spent it.
 *
 * This module is the arithmetic only, so `make test-loop-budget-host` drives
 * every shape without a kernel:
 *
 *   * `struct loop_budget_snapshot` is one completed turn's accounted spans in
 *     microseconds, exactly as `log_production_loop_phases()` has them;
 *   * `loop_budget_dominant()` answers which single span spent the most, with
 *     ties resolved in a fixed order (console first), so the published
 *     `worst_phase=` is deterministic;
 *   * `loop_budget_is_long_turn()` bounds what counts as a stall turn; the
 *     published `over50=` counts how many the window held.
 *
 * The caller keeps the accumulators and the window; this module only names.
 */

enum loop_budget_phase {
    LOOP_BUDGET_CONSOLE = 0,
    LOOP_BUDGET_XHCI,
    LOOP_BUDGET_INPUT,
    LOOP_BUDGET_NETWORK,
    LOOP_BUDGET_INPUT_XHCI,
    LOOP_BUDGET_INPUT_KEYBOARD,
    LOOP_BUDGET_INPUT_POINTER,
    /* ITEM-486 (round 23): the named parts of the former `other` remainder. */
    LOOP_BUDGET_SERVICES,
    LOOP_BUDGET_JOURNAL,
    LOOP_BUDGET_STORAGE,
    LOOP_BUDGET_OTHER,
    LOOP_BUDGET_PHASE_COUNT
};

/* One completed turn's accounted spans, microseconds. */
struct loop_budget_snapshot {
    /* The console's own measured paint+scroll. */
    uint64_t console_us;
    /* The top-level driver span: the service pass and the diagnostics. */
    uint64_t xhci_us;
    /* The top-level input span: keyboard plus pointer event handling. */
    uint64_t input_us;
    uint64_t net_us;
    /* The three sub-spans inside the driver and input spans. */
    uint64_t in_xhci_us;
    uint64_t in_kbd_us;
    uint64_t in_ptr_us;
    /*
     * ITEM-486 (round 23): the named parts of the `other_us` remainder, measured
     * outside the spans above - services and application state, the journal
     * records and their disk flush, and storage/queued writes.
     */
    uint64_t svc_us;
    uint64_t journal_us;
    uint64_t store_us;
    /* The turn's own measured work, the upper bound of every span above. */
    uint64_t work_us;
};

/*
 * A turn whose full period is strictly longer than this many microseconds is a
 * stall turn.  The healthy loop is 10 ms (`turn_us=9998` in the owner's run),
 * so 50 ms is five missed ticks: rare enough not to count ordinary jitter and
 * short enough to catch the 1.2-3.2 s stalls the round-17 evidence names.
 */
#define LOOP_BUDGET_LONG_TURN_US 50000U

/*
 * The span that spent the most of this turn, or LOOP_BUDGET_OTHER when the
 * remainder does.  Ties keep the earlier probe in the fixed order console,
 * keyboard, pointer, driver service, driver remainder, network, remainder.
 * A NULL snapshot answers OTHER rather than reading through.
 */
enum loop_budget_phase loop_budget_dominant(
    const struct loop_budget_snapshot *snapshot);

/* A stable one-word name for the record (`console`, `in_kbd`, `in_ptr`,
   `in_xhci`, `xhci`, `net`, `other`); never NULL. */
const char *loop_budget_phase_name(enum loop_budget_phase phase);

/* True when `period_us` is a stall turn (strictly longer than the bound). */
bool loop_budget_is_long_turn(uint64_t period_us);

/*
 * MOUSE-DIAG (owner task 2026-09-27): whether a completed turn is a new boot
 * maximum worth one `[ITER-MAX]` record.
 *
 * `iter_max_us` was published as a bare boot maximum - `3259516` in the
 * 2026-09-27 hardware run - with no phase and no clock, so a reader could not
 * tell a real 3.26-second turn from a measurement artifact, and the run's own
 * journal had already lost the window it happened in.  The record is written
 * once per new maximum and only past the long-turn budget, so its cost is
 * bounded by the number of new maxima, not by the loop's turn rate.  The rule
 * is pure so the host proof pins both boundaries.
 */
bool loop_budget_iter_max_should_publish(uint64_t period_us,
                                         uint64_t previous_max_us);

/*
 * MOUSE-DIAG step 3 (owner task 2026-09-27): is the phase accounting
 * irreconcilable with an independent raw measurement of the same window?
 *
 * `hw14` published `journal_us=2 829 709` and `xhci_us=3 458 581` in windows
 * whose ISR tick counter advanced exactly 100 ticks and whose cumulative cycle
 * clock advanced one second, so those spans are an accounting artifact rather
 * than elapsed time.  The rule keeps the existing metrics untouched and only
 * compares their sum with a raw anchor: a mismatch is `sum_phase_us` exceeding
 * `raw_elapsed_us` by more than `slack_us`.  The slack exists because a turn
 * that straddles the window boundary folds its whole span into the window, so
 * a correct accounting may overshoot by up to one turn; the caller passes the
 * pacing period.
 */
bool loop_budget_accounting_mismatch(uint64_t sum_phase_us,
                                     uint64_t raw_elapsed_us,
                                     uint64_t slack_us);

#endif
