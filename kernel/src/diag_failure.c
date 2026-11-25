/*
 * ITEM-558: the refusal record and its two renderings, with no device in reach.
 *
 * See kernel/include/diag_failure.h for why this is a module of its own: the
 * hardware run's detail named the walk's stage and lost the transfer that
 * stopped it, and the only copy of that transfer was a serial record on a
 * machine with no serial capture and no bound journal.
 */

#include "diag_failure.h"

#include <stddef.h>

static void diag_failure_copy(char *target,
                              size_t size,
                              const char *source) {
    size_t length = 0U;

    if (target == NULL || size == 0U) {
        return;
    }
    while (source != NULL && source[length] != '\0' && length + 1U < size) {
        target[length] = source[length];
        ++length;
    }
    target[length] = '\0';
}

/* Append one field to a bounded line; a field that does not fit is not
   started, so the line never ends with half a name or half a value. */
static size_t diag_failure_append(char *buffer,
                                  size_t size,
                                  size_t length,
                                  const char *text) {
    size_t needed = 0U;

    if (buffer == NULL || text == NULL) {
        return length;
    }
    while (text[needed] != '\0') {
        ++needed;
    }
    if (length + needed + 1U > size) {
        return length;
    }
    for (size_t index = 0U; index < needed; ++index) {
        buffer[length + index] = text[index];
    }
    length += needed;
    buffer[length] = '\0';
    return length;
}

void diag_failure_attempt_reset(struct diag_failure *failure) {
    /*
     * The record is the operator's answer to "what did the machine say", and the
     * attempt that follows is a new question.  Clearing the record here would
     * reproduce the defect this module exists to remove, so the cleanup only
     * documents the invariant: the record outlives the attempt.
     */
    (void)failure;
}

void diag_failure_set_evidence(struct diag_failure *failure,
                               const char *commit,
                               const char *storage) {
    size_t length;

    if (failure == NULL) {
        return;
    }
    /* ITEM-615: the shared record reads these fields directly; copy before the
       detail append so both renderings name the same receipt. */
    diag_failure_copy(failure->commit, sizeof(failure->commit),
                      commit != NULL && commit[0] != '\0' ? commit
                                                          : "NOT_ATTEMPTED");
    diag_failure_copy(failure->storage, sizeof(failure->storage),
                      storage != NULL && storage[0] != '\0' ? storage
                                                            : "NOT_ATTEMPTED");
    length = 0U;
    while (failure->detail[length] != '\0') {
        ++length;
    }
    if (commit != NULL && commit[0] != '\0') {
        length = diag_failure_append(failure->detail, sizeof(failure->detail),
                                     length, " commit=");
        length = diag_failure_append(failure->detail, sizeof(failure->detail),
                                     length, commit);
    }
    if (storage != NULL && storage[0] != '\0') {
        length = diag_failure_append(failure->detail, sizeof(failure->detail),
                                     length, " storage=");
        length = diag_failure_append(failure->detail, sizeof(failure->detail),
                                     length, storage);
    }
}

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
                      const char *controllers) {
    size_t length = 0U;
    const char *pieces[23];
    size_t piece_count = 0U;

    if (failure == NULL) {
        return;
    }
    diag_failure_copy(failure->reason, sizeof(failure->reason), reason);
    diag_failure_copy(failure->step, sizeof(failure->step), step);
    diag_failure_copy(failure->primary, sizeof(failure->primary), primary);
    failure->recorded = true;
    failure->detail[0] = '\0';
    /* ITEM-615: a refusal that never reaches the commit point or the bound
       device says so, instead of carrying an empty or a stale receipt. */
    diag_failure_copy(failure->commit, sizeof(failure->commit),
                      "NOT_ATTEMPTED");
    diag_failure_copy(failure->storage, sizeof(failure->storage),
                      "NOT_ATTEMPTED");

    pieces[piece_count++] = "step=";
    pieces[piece_count++] = failure->step;
    pieces[piece_count++] = " cause=";
    pieces[piece_count++] = failure->reason;
    pieces[piece_count++] = " ehci=";
    pieces[piece_count++] = ehci_state != NULL ? ehci_state : "UNKNOWN";
    pieces[piece_count++] = "/";
    pieces[piece_count++] = ehci_detail != NULL ? ehci_detail : "UNKNOWN";
    pieces[piece_count++] = " medium=";
    pieces[piece_count++] = medium != NULL ? medium : "UNCHECKED";
    pieces[piece_count++] = "/";
    pieces[piece_count++] =
        medium_reason != NULL ? medium_reason : "NOT_PROBED";
    pieces[piece_count++] = " journal=";
    pieces[piece_count++] = journal_state != NULL ? journal_state : "UNKNOWN";
    pieces[piece_count++] = "/";
    pieces[piece_count++] =
        journal_reason != NULL ? journal_reason : "UNKNOWN";
    if (primary_detail != NULL && primary_detail[0] != '\0') {
        pieces[piece_count++] = " ehci_fail=";
        pieces[piece_count++] = primary_detail;
    }
    /*
     * ITEM-561: the start evidence rides in the refusal record.  The save that
     * refuses at MEDIUM_PROBE never writes a report, so this is the only way
     * `diag` can print what the controller was told before the first start.
     */
    if (start_detail != NULL && start_detail[0] != '\0') {
        pieces[piece_count++] = " ehci_start=";
        pieces[piece_count++] = start_detail;
    }
    /*
     * ITEM-562: each controller's own answer, in scan order.  The machine has
     * two USB 2 functions, and the run that reported one of them had no way to
     * say what the other did: one global verdict was overwritten by whichever
     * controller the scan reached last.
     */
    if (controllers != NULL && controllers[0] != '\0') {
        pieces[piece_count++] = " ehci_all=";
        pieces[piece_count++] = controllers;
    }

    for (size_t index = 0U; index < piece_count; ++index) {
        const char *piece = pieces[index];

        while (piece != NULL && *piece != '\0') {
            if (length + 1U >= sizeof(failure->detail)) {
                failure->detail[length] = '\0';
                return;
            }
            failure->detail[length++] = *piece++;
        }
    }
    failure->detail[length] = '\0';
}

/*
 * One `name=value` pair as one piece: a pair is appended whole or not at all,
 * so a line can end after a complete value and never between a name and the
 * value that belongs to it.
 */
static size_t diag_failure_piece(char *piece,
                                 size_t size,
                                 const char *name,
                                 const char *value) {
    size_t length = 0U;

    if (piece == NULL || size == 0U) {
        return 0U;
    }
    piece[0] = ' ';
    length = 1U;
    while (name != NULL && *name != '\0' && length + 1U < size) {
        piece[length++] = *name++;
    }
    if (length + 1U < size) {
        piece[length++] = '=';
    }
    while (value != NULL && *value != '\0' && length + 1U < size) {
        piece[length++] = *value++;
    }
    piece[length] = '\0';
    return length;
}

size_t diag_failure_short_line_prefixed(const struct diag_failure *failure,
                                        const char *prefix,
                                        char *buffer,
                                        size_t size) {
    size_t length;
    char piece[DIAG_FAILURE_CODE_MAX * 2U + 8U];

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (failure == NULL || !failure->recorded) {
        return 0U;
    }
    length = diag_failure_append(buffer, size, 0U,
                                 prefix != NULL ? prefix : "DIAG FAILED");
    if (length == 0U) {
        buffer[0] = '\0';
        return 0U;
    }
    (void)diag_failure_piece(piece, sizeof(piece), "reason", failure->reason);
    length = diag_failure_append(buffer, size, length, piece);
    (void)diag_failure_piece(piece, sizeof(piece), "step", failure->step);
    length = diag_failure_append(buffer, size, length, piece);
    if (failure->primary[0] != '\0') {
        (void)diag_failure_piece(piece, sizeof(piece), "primary",
                                 failure->primary);
        /* The builder refuses the whole piece when it does not fit, so a short
           line never ends with ` primary=` and no value after it. */
        length = diag_failure_append(buffer, size, length, piece);
    }
    return length;
}

size_t diag_failure_short_line(const struct diag_failure *failure,
                               char *buffer,
                               size_t size) {
    return diag_failure_short_line_prefixed(failure, "DIAG FAILED", buffer,
                                            size);
}

size_t diag_failure_detail_line(const struct diag_failure *failure,
                                char *buffer,
                                size_t size) {
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (failure == NULL || !failure->recorded) {
        return 0U;
    }
    while (failure->detail[length] != '\0' && length + 1U < size) {
        buffer[length] = failure->detail[length];
        ++length;
    }
    buffer[length] = '\0';
    return length;
}

/*
 * ITEM-615: append one whole ` name=value` pair, or nothing when the pair does
 * not fit.  The shared record therefore ends after a complete receipt and
 * never between a field's name and its value.
 */
static size_t diag_failure_pair(char *buffer,
                                size_t size,
                                size_t length,
                                const char *name,
                                const char *value) {
    char pair[DIAG_FAILURE_STORAGE_MAX + 32U];
    size_t needed = 0U;

    if (buffer == NULL || size == 0U) {
        return length;
    }
    pair[0] = ' ';
    needed = 1U;
    while (name != NULL && *name != '\0' && needed + 1U < sizeof(pair)) {
        pair[needed++] = *name++;
    }
    if (needed + 1U < sizeof(pair)) {
        pair[needed++] = '=';
    }
    while (value != NULL && *value != '\0' && needed + 1U < sizeof(pair)) {
        pair[needed++] = *value++;
    }
    pair[needed] = '\0';
    if (length + needed + 1U > size) {
        return length;
    }
    return diag_failure_append(buffer, size, length, pair);
}

void diag_failure_status_report(const struct diag_failure *failure,
                                void (*sink)(void *context, const char *line),
                                void *context) {
    char line[DIAG_FAILURE_STATUS_MAX];
    size_t length;

    if (failure == NULL || sink == NULL) {
        return;
    }
    length = diag_failure_append(line, sizeof(line), 0U, "[DIAG-FAIL] state=");
    if (!failure->recorded) {
        length = diag_failure_append(line, sizeof(line), length, "NO_FAILURE");
        length = diag_failure_append(line, sizeof(line), length, " valid=1");
        length = diag_failure_append(line, sizeof(line), length,
                                     " reason=NOT_ATTEMPTED");
        sink(context, line);
        return;
    }
    length = diag_failure_append(line, sizeof(line), length, "RECORDED");
    length = diag_failure_append(line, sizeof(line), length, " valid=1");
    length = diag_failure_pair(line, sizeof(line), length, "reason",
                               failure->reason);
    length = diag_failure_pair(line, sizeof(line), length, "step",
                               failure->step);
    if (failure->primary[0] != '\0') {
        length = diag_failure_pair(line, sizeof(line), length, "primary",
                                   failure->primary);
    }
    length = diag_failure_pair(line, sizeof(line), length, "commit",
                               failure->commit);
    length = diag_failure_pair(line, sizeof(line), length, "storage",
                               failure->storage);
    sink(context, line);
}
