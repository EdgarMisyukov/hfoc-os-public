#include "loop_budget.h"

#include <stddef.h>

enum loop_budget_phase loop_budget_dominant(
    const struct loop_budget_snapshot *snapshot) {
    uint64_t best;
    enum loop_budget_phase phase;

    if (snapshot == NULL) {
        return LOOP_BUDGET_OTHER;
    }
    /*
     * Strict `>` at every step: the first candidate to reach a value keeps it,
     * so a tie resolves to the earlier probe and the published name does not
     * depend on record timing.
     */
    best = snapshot->console_us;
    phase = LOOP_BUDGET_CONSOLE;
    if (snapshot->in_kbd_us > best) {
        best = snapshot->in_kbd_us;
        phase = LOOP_BUDGET_INPUT_KEYBOARD;
    }
    if (snapshot->in_ptr_us > best) {
        best = snapshot->in_ptr_us;
        phase = LOOP_BUDGET_INPUT_POINTER;
    }
    if (snapshot->in_xhci_us > best) {
        best = snapshot->in_xhci_us;
        phase = LOOP_BUDGET_INPUT_XHCI;
    }
    {
        /*
         * The top-level driver span already contains the service sub-span; the
         * remainder is the diagnostics and anything else outside it, so the
         * two never describe the same microseconds twice.
         */
        const uint64_t xhci_rest =
            snapshot->xhci_us > snapshot->in_xhci_us
                ? snapshot->xhci_us - snapshot->in_xhci_us
                : 0U;

        if (xhci_rest > best) {
            best = xhci_rest;
            phase = LOOP_BUDGET_XHCI;
        }
    }
    if (snapshot->net_us > best) {
        best = snapshot->net_us;
        phase = LOOP_BUDGET_NETWORK;
    }
    /*
     * ITEM-486 (round 23): the former remainder's named parts.  They are
     * measured outside the spans above, so they are distinct microseconds and
     * the residual below subtracts them with the rest.
     */
    if (snapshot->svc_us > best) {
        best = snapshot->svc_us;
        phase = LOOP_BUDGET_SERVICES;
    }
    if (snapshot->journal_us > best) {
        best = snapshot->journal_us;
        phase = LOOP_BUDGET_JOURNAL;
    }
    if (snapshot->store_us > best) {
        best = snapshot->store_us;
        phase = LOOP_BUDGET_STORAGE;
    }
    {
        /*
         * `other` is the same remainder the record publishes, so the phase the
         * worst turn names is always one of the numbers on the record.
         */
        const uint64_t accounted = snapshot->console_us + snapshot->xhci_us +
                                   snapshot->input_us + snapshot->net_us +
                                   snapshot->svc_us + snapshot->journal_us +
                                   snapshot->store_us;
        const uint64_t other =
            snapshot->work_us > accounted ? snapshot->work_us - accounted : 0U;

        if (other > best) {
            best = other;
            phase = LOOP_BUDGET_OTHER;
        }
    }
    return phase;
}

const char *loop_budget_phase_name(enum loop_budget_phase phase) {
    switch (phase) {
    case LOOP_BUDGET_CONSOLE:
        return "console";
    case LOOP_BUDGET_XHCI:
        return "xhci";
    case LOOP_BUDGET_INPUT:
        return "input";
    case LOOP_BUDGET_NETWORK:
        return "net";
    case LOOP_BUDGET_INPUT_XHCI:
        return "in_xhci";
    case LOOP_BUDGET_INPUT_KEYBOARD:
        return "in_kbd";
    case LOOP_BUDGET_INPUT_POINTER:
        return "in_ptr";
    case LOOP_BUDGET_SERVICES:
        return "svc";
    case LOOP_BUDGET_JOURNAL:
        return "journal";
    case LOOP_BUDGET_STORAGE:
        return "store";
    case LOOP_BUDGET_OTHER:
    default:
        return "other";
    }
}

bool loop_budget_is_long_turn(uint64_t period_us) {
    return period_us > (uint64_t)LOOP_BUDGET_LONG_TURN_US;
}

/* MOUSE-DIAG (2026-09-27): one record per new boot maximum, and only for a
   turn past the long-turn budget - an ordinary paced turn never publishes. */
bool loop_budget_accounting_mismatch(uint64_t sum_phase_us,
                                     uint64_t raw_elapsed_us,
                                     uint64_t slack_us) {
    if (raw_elapsed_us > UINT64_MAX - slack_us) {
        return false;
    }
    return sum_phase_us > raw_elapsed_us + slack_us;
}

bool loop_budget_iter_max_should_publish(uint64_t period_us,
                                         uint64_t previous_max_us) {
    return loop_budget_is_long_turn(period_us) && period_us > previous_max_us;
}
