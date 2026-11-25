/*
 * ITEM-614: the bounded login trace snapshot and its one renderer.
 *
 * The production login already measured every stage in ITEM-611.  This module
 * keeps only the finished scalar result, so a diagnostic snapshot composed
 * later - including one after a late medium insertion or a repeated save -
 * carries the same record the serial stream carried, with no authentication,
 * KDF, epoch write or other side effect at snapshot time.
 */

#include "kernel_login_trace.h"

#include "arch/x86_64/timer.h"
#include "hfoc_build.h"

#include <stddef.h>

static enum kernel_login_trace_phase login_trace_phase =
    KERNEL_LOGIN_TRACE_NOT_ATTEMPTED;
static uint32_t login_trace_attempts;
static struct kernel_login_trace login_trace_last;
static bool login_trace_last_valid;

void kernel_login_trace_begin(void) {
    if (login_trace_phase != KERNEL_LOGIN_TRACE_IN_PROGRESS) {
        ++login_trace_attempts;
    }
    login_trace_phase = KERNEL_LOGIN_TRACE_IN_PROGRESS;
}

void kernel_login_trace_commit(const struct kernel_login_trace *trace) {
    if (trace == NULL) {
        return;
    }
    login_trace_last = *trace;
    login_trace_last_valid = true;
    if (login_trace_attempts == 0U) {
        login_trace_attempts = 1U;
    }
    login_trace_phase = KERNEL_LOGIN_TRACE_COMPLETED;
}

static bool login_trace_emit_text(kernel_login_trace_sink_fn sink,
                                  void *context, const char *text) {
    if (text == NULL || text[0] == '\0') {
        return true;
    }
    return sink(context, text);
}

static bool login_trace_emit_u64(kernel_login_trace_sink_fn sink,
                                 void *context, uint64_t value) {
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    } else {
        while (value != 0U && count < sizeof(digits)) {
            digits[count++] = (char)('0' + (value % 10U));
            value /= 10U;
        }
    }
    while (count != 0U) {
        char text[2];

        text[0] = digits[--count];
        text[1] = '\0';
        if (!sink(context, text)) {
            return false;
        }
    }
    return true;
}

static bool login_trace_emit_pair(kernel_login_trace_sink_fn sink,
                                  void *context, const char *name,
                                  uint64_t value) {
    return login_trace_emit_text(sink, context, " ") &&
           login_trace_emit_text(sink, context, name) &&
           login_trace_emit_text(sink, context, "=") &&
           login_trace_emit_u64(sink, context, value);
}

static bool login_trace_emit_cycles(kernel_login_trace_sink_fn sink,
                                    void *context, const char *name,
                                    uint64_t cycles) {
    return login_trace_emit_pair(sink, context, name,
                                 timer_cycles_to_us(cycles));
}

bool kernel_login_trace_emit(enum kernel_login_trace_phase phase,
                             const struct kernel_login_trace *trace,
                             uint32_t attempt,
                             kernel_login_trace_sink_fn sink, void *context) {
    if (sink == NULL) {
        return false;
    }
    if (!login_trace_emit_text(sink, context, "[LOGIN-TRACE] v=1 build=") ||
        !login_trace_emit_text(sink, context, hfoc_build_id())) {
        return false;
    }
    if (phase == KERNEL_LOGIN_TRACE_COMPLETED && trace != NULL) {
        return login_trace_emit_text(sink, context, " available=1") &&
               login_trace_emit_pair(sink, context, "attempt", attempt) &&
               login_trace_emit_pair(sink, context, "ok",
                                     trace->ok ? 1U : 0U) &&
               login_trace_emit_pair(sink, context, "stage", trace->stage) &&
               login_trace_emit_cycles(sink, context, "account_us",
                                       trace->account_cycles) &&
               login_trace_emit_pair(sink, context, "account_iterations",
                                     trace->account_iterations) &&
               login_trace_emit_cycles(sink, context, "seed_us",
                                       trace->seed_cycles) &&
               login_trace_emit_pair(sink, context, "seed_reads",
                                     trace->seed.slot_reads) &&
               login_trace_emit_pair(sink, context, "seed_writes",
                                     trace->seed.slot_writes) &&
               login_trace_emit_pair(sink, context, "seed_opens",
                                     trace->seed.opens) &&
               login_trace_emit_pair(sink, context, "seed_seals",
                                     trace->seed.seals) &&
               login_trace_emit_pair(sink, context, "seed_readbacks",
                                     trace->seed.readbacks) &&
               login_trace_emit_pair(sink, context, "seed_kdf_calls",
                                     trace->seed.kdf_calls) &&
               login_trace_emit_pair(sink, context, "seed_kdf_iterations",
                                     trace->seed.kdf_iterations) &&
               login_trace_emit_cycles(sink, context, "seed_read_us",
                                       trace->seed.read_cycles) &&
               login_trace_emit_cycles(sink, context, "seed_open_us",
                                       trace->seed.open_cycles) &&
               login_trace_emit_cycles(sink, context, "seed_seal_us",
                                       trace->seed.seal_cycles) &&
               login_trace_emit_cycles(sink, context, "seed_write_us",
                                       trace->seed.write_cycles) &&
               login_trace_emit_cycles(sink, context, "seed_readback_us",
                                       trace->seed.readback_cycles) &&
               login_trace_emit_cycles(sink, context, "publish_us",
                                       trace->publish_cycles) &&
               login_trace_emit_cycles(sink, context, "entropy_us",
                                       trace->entropy_cycles) &&
               login_trace_emit_cycles(sink, context, "total_us",
                                       trace->total_cycles);
    }
    if (phase == KERNEL_LOGIN_TRACE_IN_PROGRESS) {
        return login_trace_emit_text(sink, context,
                                     " available=0 state=IN_PROGRESS") &&
               login_trace_emit_pair(sink, context, "attempt", attempt);
    }
    return login_trace_emit_text(sink, context,
                                 " available=0 state=NOT_ATTEMPTED");
}

struct login_trace_line {
    char *record;
    size_t capacity;
    size_t length;
};

static bool login_trace_line_sink(void *context, const char *fragment) {
    struct login_trace_line *line = context;

    if (fragment == NULL) {
        return true;
    }
    while (*fragment != '\0') {
        if (line->length + 1U >= line->capacity) {
            line->record[0] = '\0';
            line->length = 0U;
            return false;
        }
        line->record[line->length++] = *fragment++;
    }
    line->record[line->length] = '\0';
    return true;
}

size_t kernel_login_trace_snapshot_render(char *record, size_t capacity) {
    struct login_trace_line line;
    const struct kernel_login_trace *trace = NULL;

    if (record == NULL || capacity == 0U) {
        return 0U;
    }
    record[0] = '\0';
    line.record = record;
    line.capacity = capacity;
    line.length = 0U;
    if (login_trace_phase == KERNEL_LOGIN_TRACE_COMPLETED &&
        login_trace_last_valid) {
        trace = &login_trace_last;
    }
    if (!kernel_login_trace_emit(login_trace_phase, trace,
                                 login_trace_attempts, login_trace_line_sink,
                                 &line)) {
        record[0] = '\0';
        return 0U;
    }
    return line.length;
}

bool kernel_login_trace_emit_state(kernel_login_trace_sink_fn sink,
                                   void *context) {
    const struct kernel_login_trace *trace = NULL;

    if (login_trace_phase == KERNEL_LOGIN_TRACE_COMPLETED &&
        login_trace_last_valid) {
        trace = &login_trace_last;
    }
    return kernel_login_trace_emit(login_trace_phase, trace,
                                   login_trace_attempts, sink, context);
}

void kernel_login_trace_status_report(void (*sink)(void *context,
                                                   const char *line),
                                      void *context) {
    char record[KERNEL_LOGIN_TRACE_LINE_MAX];

    if (sink == NULL) {
        return;
    }
    if (kernel_login_trace_snapshot_render(record, sizeof(record)) == 0U) {
        /* A trace that cannot fit is never published as complete.  The
           refusal names itself instead of dropping fields. */
        sink(context,
             "[LOGIN-TRACE] v=1 available=0 reason=TRACE_OVERFLOW");
        return;
    }
    sink(context, record);
}
