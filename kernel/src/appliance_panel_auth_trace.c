/*
 * ITEM-617: the bounded panel-login trace and its one renderer.
 *
 * The browser sign-in runs the stored account KDF synchronously inside the
 * serving poll, so the only measurement the production path can make without
 * changing the KDF is the wall time around the credential adapter.  This
 * record carries that time and the result, never the submitted material.  It
 * is read-only at snapshot time: composing `network`, Status, `diag save` or
 * the SSH diag stream never changes the stored attempt.
 */

#include "appliance_panel_auth_trace.h"

#include <string.h>

static enum appliance_panel_auth_trace_state panel_auth_state =
    APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED;
static struct appliance_panel_auth_trace panel_auth_last;
static uint32_t panel_auth_attempts;
static uint32_t panel_auth_progress_last_ms;
static uint32_t panel_auth_progress_lines;
static appliance_panel_auth_trace_sink panel_auth_sink;

void appliance_panel_auth_trace_set_sink(appliance_panel_auth_trace_sink sink)
{
    panel_auth_sink = sink;
}

static uint32_t panel_auth_add_saturating(uint32_t value, uint32_t add)
{
    return value > UINT32_MAX - add ? UINT32_MAX : value + add;
}

static void panel_auth_emit(void)
{
    char record[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];
    const struct appliance_panel_auth_trace *trace =
        panel_auth_state == APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED
            ? NULL
            : &panel_auth_last;

    if (panel_auth_sink == NULL) {
        return;
    }
    if (appliance_panel_auth_trace_render(panel_auth_state, trace, record,
                                          sizeof(record)) != 0U) {
        panel_auth_sink(record);
    }
}

void appliance_panel_auth_trace_begin(uint32_t now_ms)
{
    ++panel_auth_attempts;
    if (panel_auth_attempts == 0U) {
        panel_auth_attempts = 1U;
    }
    memset(&panel_auth_last, 0, sizeof(panel_auth_last));
    panel_auth_last.attempt = panel_auth_attempts;
    panel_auth_last.started_ms = now_ms;
    panel_auth_progress_last_ms = now_ms;
    panel_auth_progress_lines = 0U;
    panel_auth_state = APPLIANCE_PANEL_AUTH_IN_PROGRESS;
    panel_auth_emit();
}

void appliance_panel_auth_trace_commit(
    enum appliance_panel_auth_trace_result result, uint32_t now_ms)
{
    if (panel_auth_state != APPLIANCE_PANEL_AUTH_IN_PROGRESS) {
        return;
    }
    panel_auth_last.duration_ms = now_ms - panel_auth_last.started_ms;
    panel_auth_last.result = result;
    panel_auth_state = APPLIANCE_PANEL_AUTH_COMPLETED;
    panel_auth_emit();
}

void appliance_panel_auth_trace_slice(uint32_t kdf_us, uint32_t wait_us,
                                      uint32_t rounds_done,
                                      uint32_t rounds_total)
{
    if (panel_auth_state != APPLIANCE_PANEL_AUTH_IN_PROGRESS ||
        rounds_total == 0U || rounds_done > rounds_total) {
        return;
    }
    panel_auth_last.kdf_us =
        panel_auth_add_saturating(panel_auth_last.kdf_us, kdf_us);
    panel_auth_last.wait_us =
        panel_auth_add_saturating(panel_auth_last.wait_us, wait_us);
    if (panel_auth_last.slices != UINT32_MAX) {
        ++panel_auth_last.slices;
    }
    if (wait_us > panel_auth_last.max_gap_us) {
        panel_auth_last.max_gap_us = wait_us;
    }
    panel_auth_last.rounds_done = rounds_done;
    panel_auth_last.rounds_total = rounds_total;
}

static const char *panel_auth_result_text(
    enum appliance_panel_auth_trace_result result)
{
    switch (result) {
    case APPLIANCE_PANEL_AUTH_RESULT_OK:
        return "OK";
    case APPLIANCE_PANEL_AUTH_RESULT_DENIED:
        return "DENIED";
    case APPLIANCE_PANEL_AUTH_RESULT_THROTTLED:
        return "THROTTLED";
    case APPLIANCE_PANEL_AUTH_RESULT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PANEL_AUTH_RESULT_CANCELLED:
        return "CANCELLED";
    default:
        return "UNAVAILABLE";
    }
}

struct panel_auth_line {
    char *record;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void panel_auth_line_text(struct panel_auth_line *line,
                                 const char *text)
{
    if (line->overflow || text == NULL) {
        return;
    }
    while (*text != '\0') {
        if (line->length + 1U >= line->capacity) {
            line->overflow = true;
            return;
        }
        line->record[line->length++] = *text++;
    }
    line->record[line->length] = '\0';
}

static void panel_auth_line_u32(struct panel_auth_line *line, uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        panel_auth_line_text(line, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        char text[2];

        text[0] = digits[--count];
        text[1] = '\0';
        panel_auth_line_text(line, text);
    }
}

static void panel_auth_line_pair(struct panel_auth_line *line,
                                 const char *name, uint32_t value)
{
    panel_auth_line_text(line, " ");
    panel_auth_line_text(line, name);
    panel_auth_line_text(line, "=");
    panel_auth_line_u32(line, value);
}

size_t appliance_panel_auth_trace_render_progress(
    uint32_t iterations_done, uint32_t iterations_total, char *record,
    size_t capacity)
{
    struct panel_auth_line line;
    uint32_t percent = 0U;

    if (record == NULL || capacity == 0U ||
        iterations_total == 0U ||
        iterations_done > iterations_total) {
        if (record != NULL && capacity != 0U) {
            record[0] = '\0';
        }
        return 0U;
    }
    percent = (uint32_t)(((uint64_t)iterations_done * 100U) /
                         iterations_total);
    record[0] = '\0';
    line.record = record;
    line.capacity = capacity;
    line.length = 0U;
    line.overflow = false;
    panel_auth_line_text(&line, "[PANEL-AUTH] v=1");
    panel_auth_line_text(&line, " available=0 state=IN_PROGRESS");
    panel_auth_line_pair(&line, "attempt", panel_auth_last.attempt);
    panel_auth_line_pair(&line, "iterations_done", iterations_done);
    panel_auth_line_pair(&line, "iterations_total", iterations_total);
    panel_auth_line_pair(&line, "progress_percent", percent);
    if (line.overflow) {
        record[0] = '\0';
        return 0U;
    }
    return line.length;
}

void appliance_panel_auth_trace_progress(uint32_t now_ms,
                                         uint32_t iterations_done,
                                         uint32_t iterations_total)
{
    char record[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];

    if (panel_auth_sink == NULL ||
        panel_auth_state != APPLIANCE_PANEL_AUTH_IN_PROGRESS ||
        iterations_total == 0U ||
        iterations_done == 0U ||
        iterations_done >= iterations_total) {
        return;
    }
    /*
     * One line per second, sixteen lines per attempt: enough for a bounded
     * proof to observe real progress while the KDF is pending, and far below
     * the cost of logging every slice.
     */
    if (panel_auth_progress_lines >= 16U ||
        (uint32_t)(now_ms - panel_auth_progress_last_ms) < 1000U) {
        return;
    }
    if (appliance_panel_auth_trace_render_progress(
            iterations_done, iterations_total, record,
            sizeof(record)) == 0U) {
        return;
    }
    panel_auth_progress_last_ms = now_ms;
    ++panel_auth_progress_lines;
    panel_auth_sink(record);
}

size_t appliance_panel_auth_trace_render(
    enum appliance_panel_auth_trace_state state,
    const struct appliance_panel_auth_trace *trace, char *record,
    size_t capacity)
{
    struct panel_auth_line line;

    if (record == NULL || capacity == 0U) {
        return 0U;
    }
    record[0] = '\0';
    line.record = record;
    line.capacity = capacity;
    line.length = 0U;
    line.overflow = false;
    panel_auth_line_text(&line, "[PANEL-AUTH] v=1");
    if (state == APPLIANCE_PANEL_AUTH_COMPLETED && trace != NULL) {
        panel_auth_line_text(&line, " available=1 state=COMPLETED");
        panel_auth_line_pair(&line, "attempt", trace->attempt);
        panel_auth_line_text(&line, " result=");
        panel_auth_line_text(&line, panel_auth_result_text(trace->result));
        panel_auth_line_pair(&line, "started_ms", trace->started_ms);
        panel_auth_line_pair(&line, "duration_ms", trace->duration_ms);
        panel_auth_line_pair(&line, "kdf_us", trace->kdf_us);
        panel_auth_line_pair(&line, "wait_us", trace->wait_us);
        panel_auth_line_pair(&line, "slices", trace->slices);
        panel_auth_line_pair(&line, "max_gap_us", trace->max_gap_us);
        panel_auth_line_pair(&line, "rounds_done", trace->rounds_done);
        panel_auth_line_pair(&line, "rounds_total", trace->rounds_total);
    } else if (state == APPLIANCE_PANEL_AUTH_IN_PROGRESS) {
        panel_auth_line_text(&line, " available=0 state=IN_PROGRESS");
        if (trace != NULL) {
            panel_auth_line_pair(&line, "attempt", trace->attempt);
            panel_auth_line_pair(&line, "started_ms", trace->started_ms);
        }
    } else {
        panel_auth_line_text(&line, " available=0 state=NOT_ATTEMPTED");
    }
    if (line.overflow) {
        record[0] = '\0';
        return 0U;
    }
    return line.length;
}

void appliance_panel_auth_trace_status_report(
    void (*sink)(void *context, const char *line), void *context)
{
    char record[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];

    const struct appliance_panel_auth_trace *trace =
        panel_auth_state == APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED
            ? NULL
            : &panel_auth_last;

    if (sink == NULL) {
        return;
    }
    if (appliance_panel_auth_trace_render(panel_auth_state, trace, record,
                                          sizeof(record)) == 0U) {
        return;
    }
    sink(context, record);
}
