/*
 * ITEM-617: the panel-login trace is bounded, honest about pending work, and
 * never carries a credential.  This drives the renderer and the stored state
 * through the same functions the production composition calls.
 */
#include "appliance_panel_auth_trace.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: ");                                        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

struct sink_line {
    char text[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];
    int calls;
};

static void sink_capture(void *context, const char *line)
{
    struct sink_line *sink = context;

    ++sink->calls;
    if (line != NULL) {
        (void)snprintf(sink->text, sizeof(sink->text), "%s", line);
    }
}

/* ITEM-619 follow-up: the production sink is what the serial proof reads. */
static char serial_line[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];
static int serial_calls;

static void serial_capture(const char *line)
{
    ++serial_calls;
    if (line != NULL) {
        (void)snprintf(serial_line, sizeof(serial_line), "%s", line);
    }
}

int main(void)
{
    char record[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];
    struct appliance_panel_auth_trace trace;
    struct sink_line sink;

    memset(&trace, 0, sizeof(trace));
    CHECK(appliance_panel_auth_trace_render(
              APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED, NULL, record,
              sizeof(record)) == strlen(record),
          "the NOT_ATTEMPTED render returned a wrong length");
    CHECK(strcmp(record, "[PANEL-AUTH] v=1 available=0 state=NOT_ATTEMPTED") ==
              0,
          "the cold record is %s", record);

    trace.attempt = 1U;
    trace.started_ms = 1000U;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_IN_PROGRESS,
                                            &trace, record, sizeof(record)) ==
              strlen(record),
          "the IN_PROGRESS render returned a wrong length");
    CHECK(strstr(record, "available=0 state=IN_PROGRESS") != NULL,
          "the pending record is %s", record);
    CHECK(strstr(record, " attempt=1") != NULL,
          "the pending record lost its attempt: %s", record);
    CHECK(strstr(record, " duration_ms=") == NULL,
          "the pending record claimed a finished duration: %s", record);

    trace.duration_ms = 21000U;
    trace.result = APPLIANCE_PANEL_AUTH_RESULT_OK;
    trace.kdf_us = 1700U;
    trace.wait_us = 300U;
    trace.slices = 3U;
    trace.max_gap_us = 200U;
    trace.rounds_done = 200000U;
    trace.rounds_total = 200000U;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, sizeof(record)) ==
              strlen(record),
          "the COMPLETED render returned a wrong length");
    CHECK(strstr(record, "available=1 state=COMPLETED") != NULL,
          "the completed record is %s", record);
    CHECK(strstr(record, "attempt=1") != NULL &&
              strstr(record, "result=OK") != NULL &&
              strstr(record, "started_ms=1000") != NULL &&
              strstr(record, "duration_ms=21000") != NULL &&
              strstr(record, "kdf_us=1700") != NULL &&
              strstr(record, "wait_us=300") != NULL &&
              strstr(record, "slices=3") != NULL &&
              strstr(record, "max_gap_us=200") != NULL &&
              strstr(record, "rounds_done=200000") != NULL &&
              strstr(record, "rounds_total=200000") != NULL,
          "the completed record lost a field: %s", record);

    trace.result = APPLIANCE_PANEL_AUTH_RESULT_DENIED;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, sizeof(record)) !=
              0U &&
              strstr(record, "result=DENIED") != NULL,
          "the denied record is %s", record);
    trace.result = APPLIANCE_PANEL_AUTH_RESULT_THROTTLED;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, sizeof(record)) !=
              0U &&
              strstr(record, "result=THROTTLED") != NULL,
          "the throttled record is %s", record);
    trace.result = APPLIANCE_PANEL_AUTH_RESULT_UNAVAILABLE;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, sizeof(record)) !=
              0U &&
              strstr(record, "result=UNAVAILABLE") != NULL,
          "the unavailable record is %s", record);
    trace.result = APPLIANCE_PANEL_AUTH_RESULT_CANCELLED;
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, sizeof(record)) !=
              0U &&
              strstr(record, "result=CANCELLED") != NULL,
          "the cancelled record is %s", record);

    /* A capacity that cannot hold the record is refused whole, never cut. */
    record[0] = 'x';
    CHECK(appliance_panel_auth_trace_render(APPLIANCE_PANEL_AUTH_COMPLETED,
                                            &trace, record, 8U) == 0U,
          "a short buffer accepted a truncated record");
    CHECK(record[0] == '\0', "a refused render left text in the buffer");

    /* The stored state follows begin/commit and replaces the old success. */
    appliance_panel_auth_trace_begin(1000U);
    memset(&sink, 0, sizeof(sink));
    appliance_panel_auth_trace_status_report(sink_capture, &sink);
    CHECK(sink.calls == 1, "the pending status emitted %d lines", sink.calls);
    CHECK(strstr(sink.text, "state=IN_PROGRESS") != NULL &&
              strstr(sink.text, "attempt=1") != NULL,
          "the stored pending record is %s", sink.text);

    appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK, 22000U);
    memset(&sink, 0, sizeof(sink));
    appliance_panel_auth_trace_status_report(sink_capture, &sink);
    CHECK(strstr(sink.text, "state=COMPLETED") != NULL &&
              strstr(sink.text, "result=OK") != NULL &&
              strstr(sink.text, "duration_ms=21000") != NULL,
          "the stored success record is %s", sink.text);

    appliance_panel_auth_trace_begin(30000U);
    memset(&sink, 0, sizeof(sink));
    appliance_panel_auth_trace_status_report(sink_capture, &sink);
    CHECK(strstr(sink.text, "state=IN_PROGRESS") != NULL &&
              strstr(sink.text, "attempt=2") != NULL,
          "a second attempt reuses the previous result: %s", sink.text);
    CHECK(strstr(sink.text, "result=") == NULL,
          "the pending second attempt reports a result: %s", sink.text);

    appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_DENIED,
                                      31000U);
    memset(&sink, 0, sizeof(sink));
    appliance_panel_auth_trace_status_report(sink_capture, &sink);
    CHECK(strstr(sink.text, "state=COMPLETED") != NULL &&
              strstr(sink.text, "result=DENIED") != NULL &&
              strstr(sink.text, "duration_ms=1000") != NULL,
          "the stored denial record is %s", sink.text);

    /* The registered sink sees the start and the measured finish in order. */
    appliance_panel_auth_trace_set_sink(serial_capture);
    serial_calls = 0;
    serial_line[0] = '\0';
    appliance_panel_auth_trace_begin(40000U);
    CHECK(serial_calls == 1, "the serial sink saw %d start lines", serial_calls);
    CHECK(strstr(serial_line, "state=IN_PROGRESS") != NULL &&
              strstr(serial_line, "attempt=3") != NULL &&
              strstr(serial_line, " started_ms=40000") != NULL,
          "the serial start line is %s", serial_line);
    appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK, 41000U);
    CHECK(serial_calls == 2, "the serial sink saw %d lines", serial_calls);
    CHECK(strstr(serial_line, "state=COMPLETED") != NULL &&
              strstr(serial_line, "result=OK") != NULL &&
              strstr(serial_line, "duration_ms=1000") != NULL,
          "the serial finish line is %s", serial_line);
    /* ITEM-619: pending progress is bounded, ordered and carries no secret. */
    {
        int base;
        int index;

        appliance_panel_auth_trace_begin(45000U);
        base = serial_calls;
        CHECK(base >= 1, "the serial sink saw %d start lines", base);
        serial_line[0] = '\0';
        appliance_panel_auth_trace_progress(45500U, 10000U, 200000U);
        CHECK(serial_calls == base,
              "a sub-second slice emitted a progress line too early");
        appliance_panel_auth_trace_progress(46000U, 10000U, 200000U);
        CHECK(serial_calls == base + 1,
              "the first second emitted %d progress lines",
              serial_calls - base);
        CHECK(strstr(serial_line, "state=IN_PROGRESS") != NULL &&
                  strstr(serial_line, "iterations_done=10000") != NULL &&
                  strstr(serial_line, "iterations_total=200000") != NULL &&
                  strstr(serial_line, "progress_percent=5") != NULL,
              "the first progress line is %s", serial_line);
        CHECK(strstr(serial_line, "result=") == NULL &&
                  strstr(serial_line, "duration_ms=") == NULL,
              "the progress line claims a finished result: %s", serial_line);
        appliance_panel_auth_trace_progress(46500U, 20000U, 200000U);
        CHECK(serial_calls == base + 1,
              "a slice inside the same second emitted a duplicate line");
        appliance_panel_auth_trace_progress(47000U, 20000U, 200000U);
        CHECK(serial_calls == base + 2,
              "the next second emitted %d progress lines",
              serial_calls - base);
        appliance_panel_auth_trace_progress(48000U, 200001U, 200000U);
        appliance_panel_auth_trace_progress(49000U, 0U, 200000U);
        appliance_panel_auth_trace_progress(50000U, 1000U, 0U);
        CHECK(serial_calls == base + 2,
              "an out-of-range round count emitted a progress line");
        for (index = 0; index < 20; ++index) {
            appliance_panel_auth_trace_progress(
                51000U + (uint32_t)index * 1000U,
                30000U + (uint32_t)index * 100U, 200000U);
        }
        CHECK(serial_calls == base + 16,
              "the per-attempt progress cap emitted %d lines",
              serial_calls - base);
        appliance_panel_auth_trace_commit(
            APPLIANCE_PANEL_AUTH_RESULT_OK, 65000U);
        CHECK(serial_calls == base + 17,
              "the serial sink saw %d finish lines", serial_calls - base);
        CHECK(strstr(serial_line, "state=COMPLETED") != NULL &&
                  strstr(serial_line, "duration_ms=20000") != NULL,
              "the finish after progress is %s", serial_line);
    }
    {
        char progress[APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX];

        CHECK(appliance_panel_auth_trace_render_progress(
                  50000U, 200000U, progress, sizeof(progress)) ==
                  strlen(progress),
              "the progress render returned a wrong length");
        CHECK(strstr(progress, "iterations_done=50000") != NULL &&
                  strstr(progress, "progress_percent=25") != NULL,
              "the direct progress render is %s", progress);
        CHECK(appliance_panel_auth_trace_render_progress(
                  1U, 0U, progress, sizeof(progress)) == 0U &&
                  progress[0] == '\0',
              "a zero total accepted a progress record");
    }
    /* ITEM-652: measured slices accumulate and keep the worst wait. */
    {
        appliance_panel_auth_trace_begin(70000U);
        appliance_panel_auth_trace_slice(1000U, 0U, 64U, 200000U);
        appliance_panel_auth_trace_slice(2000U, 150U, 128U, 200000U);
        appliance_panel_auth_trace_slice(500U, 90U, 192U, 200000U);
        appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK,
                                          71000U);
        memset(&sink, 0, sizeof(sink));
        appliance_panel_auth_trace_status_report(sink_capture, &sink);
        CHECK(strstr(sink.text, "kdf_us=3500") != NULL &&
                  strstr(sink.text, "wait_us=240") != NULL &&
                  strstr(sink.text, "slices=3") != NULL &&
                  strstr(sink.text, "max_gap_us=150") != NULL &&
                  strstr(sink.text, "rounds_done=192") != NULL &&
                  strstr(sink.text, "rounds_total=200000") != NULL,
              "the accumulated slice record is %s", sink.text);
    }
    /* Impossible round pairs and pre-attempt slices are ignored whole. */
    {
        appliance_panel_auth_trace_begin(72000U);
        appliance_panel_auth_trace_slice(100U, 100U, 101U, 100U);
        appliance_panel_auth_trace_slice(100U, 100U, 1U, 0U);
        appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK,
                                          73000U);
        memset(&sink, 0, sizeof(sink));
        appliance_panel_auth_trace_status_report(sink_capture, &sink);
        CHECK(strstr(sink.text, "kdf_us=0") != NULL &&
                  strstr(sink.text, "wait_us=0") != NULL &&
                  strstr(sink.text, "slices=0") != NULL &&
                  strstr(sink.text, "max_gap_us=0") != NULL,
              "an invalid slice was counted: %s", sink.text);
    }
    /* Saturation is explicit; a long slow job cannot wrap the counters. */
    {
        appliance_panel_auth_trace_begin(74000U);
        appliance_panel_auth_trace_slice(UINT32_MAX - 10U, UINT32_MAX - 20U,
                                         1U, 2U);
        appliance_panel_auth_trace_slice(100U, 100U, 2U, 2U);
        appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK,
                                          75000U);
        memset(&sink, 0, sizeof(sink));
        appliance_panel_auth_trace_status_report(sink_capture, &sink);
        CHECK(strstr(sink.text, "kdf_us=4294967295") != NULL &&
                  strstr(sink.text, "wait_us=4294967295") != NULL &&
                  strstr(sink.text, "max_gap_us=4294967275") != NULL,
              "the saturated record is %s", sink.text);
    }

    /* Removing the sink returns to a no-op, as early boot and host tests need. */
    {
        const int lines_before_removal = serial_calls;

        appliance_panel_auth_trace_set_sink(NULL);
        appliance_panel_auth_trace_begin(50000U);
        CHECK(serial_calls == lines_before_removal,
              "a removed sink still emitted %d lines", serial_calls);
    }

    if (failures != 0) {
        fprintf(stderr, "panel auth trace host: %d failure(s)\n", failures);
        return 1;
    }
    printf("panel auth trace host: ALL OK\n");
    return 0;
}
