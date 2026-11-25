#include "input_path.h"

#include <stddef.h>

/*
 * ITEM-486 (round 24): see input_path.h for the model.  The cases are ordered
 * from the driver's own state outward: a watchdog that gave up is named before a
 * completion that never came, a completion without a decoded report before a
 * report without a local event, and so on.  The function is pure so the host
 * proof can drive every branch.
 */
enum input_path_outcome input_path_classify(
    const struct input_path_window *window)
{
    if (window == NULL) {
        return INPUT_PATH_OK;
    }
    if (window->watchdog_gave_up) {
        return INPUT_PATH_WATCHDOG_GAVE_UP;
    }
    if (window->watchdog_rearms != 0U && window->reports == 0U) {
        /* The recovery ran and the window still held no report. */
        return INPUT_PATH_WATCHDOG_REARM;
    }
    if (window->mouse_armed && window->completions == 0U &&
        window->reports == 0U) {
        return INPUT_PATH_NO_COMPLETION;
    }
    if (window->mouse_armed && window->reports == 0U) {
        return INPUT_PATH_NO_REPORT;
    }
    if (window->reports != 0U && window->local_events == 0U) {
        return INPUT_PATH_NO_LOCAL_INPUT;
    }
    if (window->local_events != 0U && window->pointer_draws == 0U) {
        return INPUT_PATH_QUEUE_BACKLOG;
    }
    if (window->pointer_draws != 0U) {
        return INPUT_PATH_POINTER_PROCESSING;
    }
    if (window->framebuffer_flushes != 0U) {
        return INPUT_PATH_FRAMEBUFFER_RENDER;
    }
    return INPUT_PATH_OK;
}

const char *input_path_outcome_name(enum input_path_outcome outcome)
{
    switch (outcome) {
    case INPUT_PATH_NO_COMPLETION:
        return "NO_COMPLETION";
    case INPUT_PATH_NO_REPORT:
        return "NO_REPORT";
    case INPUT_PATH_NO_LOCAL_INPUT:
        return "NO_LOCAL_INPUT";
    case INPUT_PATH_QUEUE_BACKLOG:
        return "QUEUE_BACKLOG";
    case INPUT_PATH_WATCHDOG_REARM:
        return "WATCHDOG_REARM";
    case INPUT_PATH_WATCHDOG_GAVE_UP:
        return "WATCHDOG_GAVE_UP";
    case INPUT_PATH_POINTER_PROCESSING:
        return "POINTER_PROCESSING";
    case INPUT_PATH_FRAMEBUFFER_RENDER:
        return "FRAMEBUFFER_RENDER";
    case INPUT_PATH_OK:
    default:
        return "OK";
    }
}
