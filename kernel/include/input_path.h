#ifndef HFOC_INPUT_PATH_H
#define HFOC_INPUT_PATH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-486 (round 24): the mouse event path's own named outcome.
 *
 * The owner's hardware runs end with a pointer that worked and then stopped:
 * `mouse_armed=16`, the endpoint Running, every completion code SUCCESS, and no
 * report for the rest of the run.  The counters exist, but a reader is left to
 * decide from a dozen numbers where the event was lost.  This module names the
 * furthest stage the window is known to have reached:
 *
 *   mouse physical report -> xHCI completion -> decoded report ->
 *   local input -> pointer processing -> framebuffer render
 *
 * `input_path_classify()` is pure and takes only the window's deltas, so the
 * host proof can pin every reachable outcome without a controller; the kernel
 * fills it from the xHCI counters and the loop's pointer window and publishes
 * `mouse_path=<name>` on the input record.  The outcome is diagnostic, not a
 * verdict: `OK` means no stage stalled, and the other names say where the
 * window's evidence stops.
 *
 * `WATCHDOG_GAVE_UP` is the case the motivating run needs: the stalled-endpoint
 * watchdog published one recovery and then, because a fresh arm does not clear
 * the reported flag, never tries again while the endpoint stays armed - the
 * state the 2026-09-26 `hw11` journal showed as `mouse_watchdog=3` followed by
 * silence to shutdown.
 */
enum input_path_outcome {
    INPUT_PATH_OK = 0,
    INPUT_PATH_NO_COMPLETION,
    INPUT_PATH_NO_REPORT,
    INPUT_PATH_NO_LOCAL_INPUT,
    INPUT_PATH_QUEUE_BACKLOG,
    INPUT_PATH_WATCHDOG_REARM,
    INPUT_PATH_WATCHDOG_GAVE_UP,
    INPUT_PATH_POINTER_PROCESSING,
    INPUT_PATH_FRAMEBUFFER_RENDER
};

struct input_path_window {
    /* The pointer is bound and holds at least one armed transfer. */
    bool mouse_armed;
    /* Physical completions the driver re-armed in the window. */
    uint32_t completions;
    /* Reports the driver decoded from those completions in the window. */
    uint32_t reports;
    /* Pointer events the loop took out of local input in the window. */
    uint32_t local_events;
    /* Pointer draws the loop performed in the window. */
    uint32_t pointer_draws;
    /* Watchdog recoveries published in the window. */
    uint32_t watchdog_rearms;
    /* The watchdog published for the current silence and will not try again. */
    bool watchdog_gave_up;
    /* Framebuffer flushes the window performed. */
    uint32_t framebuffer_flushes;
};

enum input_path_outcome input_path_classify(
    const struct input_path_window *window);
const char *input_path_outcome_name(enum input_path_outcome outcome);

#endif
