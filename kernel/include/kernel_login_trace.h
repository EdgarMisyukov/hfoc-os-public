#ifndef HFOC_KERNEL_LOGIN_TRACE_H
#define HFOC_KERNEL_LOGIN_TRACE_H

/*
 * ITEM-614: one finished production login attempt, kept for the snapshot.
 *
 * ITEM-611 published `[LOGIN-TRACE]` on the serial stream only.  `diag save`
 * does not copy that stream: it composes a fresh report, so the owner's next
 * hardware export would still carry no latency record.  This module keeps the
 * last finished attempt as bounded scalar data, renders it through one
 * function, and exposes the same renderer to the serial/journal publisher and
 * to `kernel_production_status_report()` -> `network`/Status/`diag save`/SSH
 * `hfoc diag`.
 *
 * The module never receives or stores a password, seed, verifier, token, key
 * or cryptographic context.  It only copies the stage result, the timing
 * cycles and the operation/KDF counters the production login already measured.
 *
 * A new attempt marks the snapshot IN_PROGRESS before it runs, so a report
 * taken while the password is still being checked cannot present the previous
 * success as this attempt's result.  Only a finished attempt becomes
 * `available=1`; a failure replaces the previous success with its own `ok=0`
 * and failure stage.  With no attempt at all the record is an honest
 * `available=0 state=NOT_ATTEMPTED`, never fabricated zeros or an `ok=1`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_host_seed.h"

/* Worst case for every field at its full width plus the build identity; the
   renderer refuses rather than truncating a field when a caller gives it less. */
#define KERNEL_LOGIN_TRACE_LINE_MAX 768U

enum kernel_login_trace_phase {
    KERNEL_LOGIN_TRACE_NOT_ATTEMPTED = 0,
    KERNEL_LOGIN_TRACE_IN_PROGRESS,
    KERNEL_LOGIN_TRACE_COMPLETED
};

struct kernel_login_trace {
    bool ok;
    uint32_t stage;
    uint32_t account_iterations;
    uint64_t account_cycles;
    uint64_t seed_cycles;
    uint64_t publish_cycles;
    uint64_t entropy_cycles;
    uint64_t total_cycles;
    struct appliance_host_seed_accounting seed;
};

/* The production login opens one attempt after its context guard and commits
   the finished result on every exit path that measured something. */
void kernel_login_trace_begin(void);
void kernel_login_trace_commit(const struct kernel_login_trace *trace);

/* One fragment sink.  A fragment is a field name ending in `=` or its value,
   never a secret, so the console's journal mirror can break between fields
   instead of cutting a number.  Returning false stops the render. */
typedef bool (*kernel_login_trace_sink_fn)(void *context,
                                           const char *fragment);

/* Render an explicit state; `trace` is read only for COMPLETED. */
bool kernel_login_trace_emit(enum kernel_login_trace_phase phase,
                             const struct kernel_login_trace *trace,
                             uint32_t attempt,
                             kernel_login_trace_sink_fn sink, void *context);

/* Render the module's stored state through the same fragments. */
bool kernel_login_trace_emit_state(kernel_login_trace_sink_fn sink,
                                   void *context);

/* The stored state as one bounded line.  Returns the length, or 0 when the
   caller's capacity cannot hold every field (the buffer is then empty). */
size_t kernel_login_trace_snapshot_render(char *record, size_t capacity);

/* ITEM-614: the shared status-report record.  It is the same stored state the
   serial/journal publisher emits, so the two cannot disagree. */
void kernel_login_trace_status_report(void (*sink)(void *context,
                                                   const char *line),
                                      void *context);

#endif /* HFOC_KERNEL_LOGIN_TRACE_H */
