#ifndef HFOC_APPLIANCE_PANEL_AUTH_TRACE_H
#define HFOC_APPLIANCE_PANEL_AUTH_TRACE_H

/*
 * ITEM-617: the one panel-login attempt, kept as bounded scalar data.
 *
 * The reproduced Toshiba incident showed a ~21 s gap in every network reply
 * that matches the account KDF inside the browser sign-in path.  The panel
 * path had no record of its own, so a snapshot taken after the page stopped
 * loading could not name the blocking stage or its duration.  This module
 * stores only the attempt number, the start tick, the measured duration and a
 * result word; it never receives a login, password, verifier, token, cookie,
 * KDF context or key.
 *
 * A new attempt marks the record IN_PROGRESS before the credential check, so a
 * snapshot taken while the KDF is still running cannot present the previous
 * success.  Only a finished attempt becomes COMPLETED, and a failure replaces
 * the previous success with its own result.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX 320U

enum appliance_panel_auth_trace_state {
    APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED = 0,
    APPLIANCE_PANEL_AUTH_IN_PROGRESS,
    APPLIANCE_PANEL_AUTH_COMPLETED
};

enum appliance_panel_auth_trace_result {
    APPLIANCE_PANEL_AUTH_RESULT_OK = 0,
    APPLIANCE_PANEL_AUTH_RESULT_DENIED,
    APPLIANCE_PANEL_AUTH_RESULT_THROTTLED,
    APPLIANCE_PANEL_AUTH_RESULT_UNAVAILABLE,
    APPLIANCE_PANEL_AUTH_RESULT_CANCELLED
};

struct appliance_panel_auth_trace {
    uint32_t attempt;
    uint32_t started_ms;
    uint32_t duration_ms;
    enum appliance_panel_auth_trace_result result;
    /*
     * ITEM-652: separate the derivation cost from the wait between slices and
     * keep the bounded progress totals, so a snapshot can attribute a slow
     * sign-in instead of carrying only one wall duration.  These are scalar
     * counters; no submitted or derived secret material reaches this record.
     */
    uint32_t kdf_us;
    uint32_t wait_us;
    uint32_t slices;
    uint32_t max_gap_us;
    uint32_t rounds_done;
    uint32_t rounds_total;
};

/*
 * ITEM-619 follow-up: the proof needs the production job's start and finish on
 * the serial stream, in order with the traffic it served.  The sink receives
 * the same bounded record `appliance_panel_auth_trace_render()` produces -
 * attempt, start, duration and result only, never a login, secret or token.
 * With no sink registered (host tests, early boot) emission is a no-op.
 */
typedef void (*appliance_panel_auth_trace_sink)(const char *line);

void appliance_panel_auth_trace_set_sink(appliance_panel_auth_trace_sink sink);
void appliance_panel_auth_trace_begin(uint32_t now_ms);
/*
 * ITEM-619: the pending proof needs measured progress, not only a start and a
 * finish.  The serving poll reports the production job's round count; the
 * module emits at most one bounded line per second and at most sixteen per
 * attempt, so the serial stream shows the KDF advancing while other requests
 * are answered without flooding the console.  It never receives password,
 * salt, verifier or key material.
 */
void appliance_panel_auth_trace_progress(uint32_t now_ms,
                                         uint32_t iterations_done,
                                         uint32_t iterations_total);
/*
 * ITEM-652: record one measured production slice.  `kdf_us` is the time spent
 * inside the credential step, `wait_us` the time between the previous slice's
 * end and this slice's start, and the round counters are the job's own
 * bounded totals.  Accumulation saturates instead of wrapping and is ignored
 * outside an in-progress attempt or for an impossible round pair.
 */
void appliance_panel_auth_trace_slice(uint32_t kdf_us, uint32_t wait_us,
                                      uint32_t rounds_done,
                                      uint32_t rounds_total);
void appliance_panel_auth_trace_commit(enum appliance_panel_auth_trace_result result,
                                       uint32_t now_ms);

/* One bounded record, or an empty buffer when the capacity cannot hold every
   field.  `state` is explicit so a caller can render the stored state without
   a second copy. */
size_t appliance_panel_auth_trace_render(
    enum appliance_panel_auth_trace_state state,
    const struct appliance_panel_auth_trace *trace, char *record,
    size_t capacity);

/* The bounded pending-progress record, or an empty buffer on overflow. */
size_t appliance_panel_auth_trace_render_progress(
    uint32_t iterations_done, uint32_t iterations_total, char *record,
    size_t capacity);

/* The shared status-report record used by `network`/Status/`diag save`/SSH. */
void appliance_panel_auth_trace_status_report(
    void (*sink)(void *context, const char *line), void *context);

#endif /* HFOC_APPLIANCE_PANEL_AUTH_TRACE_H */
