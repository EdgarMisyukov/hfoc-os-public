/*
 * ITEM-486 (round 24): the mouse event path names the furthest stage it
 * reached.
 *
 * The owner's hardware runs end with the pointer working and then silent while
 * `mouse_armed=16` and the endpoint Running.  input_path.c is linked into this
 * binary, so the classifier the kernel publishes is the one exercised here, and
 * every outcome the contract names is pinned from a window that reaches it:
 * NO_COMPLETION, NO_REPORT, NO_LOCAL_INPUT, QUEUE_BACKLOG, WATCHDOG_REARM,
 * WATCHDOG_GAVE_UP, POINTER_PROCESSING and FRAMEBUFFER_RENDER.
 */
#include "input_path.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static unsigned int checks;

#define CHECK(condition, message) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static bool outcome_is(const struct input_path_window *window,
                       enum input_path_outcome expected,
                       const char *name)
{
    const enum input_path_outcome actual = input_path_classify(window);

    CHECK(actual == expected, name);
    CHECK(strcmp(input_path_outcome_name(actual), name) == 0,
          "the outcome has its stable record name");
    return actual == expected;
}

static void test_armed_but_no_completion(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.framebuffer_flushes = 3U;
    outcome_is(&window, INPUT_PATH_NO_COMPLETION,
               "NO_COMPLETION");
}

static void test_completion_without_report(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.completions = 4U;
    outcome_is(&window, INPUT_PATH_NO_REPORT, "NO_REPORT");
}

static void test_report_without_local_event(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.completions = 4U;
    window.reports = 2U;
    outcome_is(&window, INPUT_PATH_NO_LOCAL_INPUT, "NO_LOCAL_INPUT");
}

static void test_local_event_without_draw(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.completions = 4U;
    window.reports = 2U;
    window.local_events = 2U;
    outcome_is(&window, INPUT_PATH_QUEUE_BACKLOG, "QUEUE_BACKLOG");
}

static void test_watchdog_rearm_without_report(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.watchdog_rearms = 1U;
    outcome_is(&window, INPUT_PATH_WATCHDOG_REARM, "WATCHDOG_REARM");
}

static void test_watchdog_gave_up_wins(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.completions = 4U;
    window.reports = 2U;
    window.local_events = 2U;
    window.pointer_draws = 1U;
    window.watchdog_rearms = 3U;
    window.watchdog_gave_up = true;
    outcome_is(&window, INPUT_PATH_WATCHDOG_GAVE_UP, "WATCHDOG_GAVE_UP");
}

static void test_pointer_processing(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.mouse_armed = true;
    window.completions = 4U;
    window.reports = 4U;
    window.local_events = 3U;
    window.pointer_draws = 2U;
    window.framebuffer_flushes = 1U;
    outcome_is(&window, INPUT_PATH_POINTER_PROCESSING,
               "POINTER_PROCESSING");
}

static void test_framebuffer_render_without_pointer(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    window.framebuffer_flushes = 2U;
    outcome_is(&window, INPUT_PATH_FRAMEBUFFER_RENDER,
               "FRAMEBUFFER_RENDER");
}

static void test_quiet_window_is_ok(void)
{
    struct input_path_window window;

    memset(&window, 0, sizeof(window));
    outcome_is(&window, INPUT_PATH_OK, "OK");
    CHECK(input_path_classify(NULL) == INPUT_PATH_OK,
          "a missing window is not a failure");
}

int main(void)
{
    test_armed_but_no_completion();
    test_completion_without_report();
    test_report_without_local_event();
    test_local_event_without_draw();
    test_watchdog_rearm_without_report();
    test_watchdog_gave_up_wins();
    test_pointer_processing();
    test_framebuffer_render_without_pointer();
    test_quiet_window_is_ok();

    if (failures != 0) {
        fprintf(stderr, "input path host: %u checks, %d failures\n",
                checks, failures);
        return 1;
    }
    printf("input path host: %u checks, 0 failures\n", checks);
    return 0;
}
