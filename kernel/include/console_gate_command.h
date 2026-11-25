#ifndef HFOC_CONSOLE_GATE_COMMAND_H
#define HFOC_CONSOLE_GATE_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-516: the console's management-gate vocabulary.
 *
 * The owner's decision of 2026-10-10 gives the operator one durable setting:
 * whether the panel answers over the network right after boot without a local
 * login.  Enabling is the dangerous direction and is local-only; disabling is
 * the safe direction and is available from the console and from the panel.
 *
 * The command is one word with a bounded grammar, classified before the
 * ordinary command chain:
 *
 *   `gate`               -> SHOW: print the setting in force
 *   `gate open`          -> OPEN_REQUEST: arm the confirmation, print the
 *                           warning; nothing is written
 *   `gate open confirm`  -> OPEN_CONFIRM when the request is armed: write open
 *                           (the caller also requires a proven local secret)
 *   `gate open confirm`  -> OPEN_UNCONFIRMED when nothing was armed: refuse
 *   `gate close`         -> CLOSE: write the default closed; always allowed
 *   `gate open extra`    -> REFUSE: one value and nothing else
 *   `gate`, ...          -> anything else after the word is a named refusal
 *
 * The classifier is pure and holds the armed half exactly as the ITEM-465/506
 * reset gesture does: any line that is not the confirmation disarms it, so the
 * confirmation must immediately follow the request.
 */
#define CONSOLE_GATE_COMMAND_WORD "gate"
#define CONSOLE_GATE_COMMAND_OPEN_WORD "gate open"
#define CONSOLE_GATE_COMMAND_OPEN_CONFIRM_WORD "gate open confirm"
#define CONSOLE_GATE_COMMAND_CLOSE_WORD "gate close"

enum console_gate_command_kind {
    /* Not the gate command: the ordinary command chain owns the line. */
    CONSOLE_GATE_COMMAND_NONE = 0,
    /* `GATE`: print the current setting. */
    CONSOLE_GATE_COMMAND_SHOW = 1,
    /* `GATE OPEN`: arm the confirmation and print the warning; nothing written. */
    CONSOLE_GATE_COMMAND_OPEN_REQUEST = 2,
    /* `GATE OPEN CONFIRM` with the request armed: the caller may write open. */
    CONSOLE_GATE_COMMAND_OPEN_CONFIRM = 3,
    /* `GATE OPEN CONFIRM` with nothing armed: refuse and say so. */
    CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED = 4,
    /* `GATE CLOSE`: write the default closed. */
    CONSOLE_GATE_COMMAND_CLOSE = 5,
    /* The word is `gate` but the rest is not a form above: refuse, visibly. */
    CONSOLE_GATE_COMMAND_REFUSE = 6,
};

static inline char console_gate_command_fold(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static inline bool console_gate_command_equals(const char *line,
                                               size_t length,
                                               const char *keyword) {
    size_t index = 0U;

    if (line == NULL || keyword == NULL) {
        return false;
    }
    while (keyword[index] != '\0') {
        if (index >= length ||
            console_gate_command_fold(line[index]) != keyword[index]) {
            return false;
        }
        ++index;
    }
    return index == length;
}

/*
 * Classify one submitted line and update the pending flag.  A NULL
 * `open_pending` cannot carry the armed request, so the confirmation would be
 * unconfirmed by definition; the classification itself still happens.
 */
static inline enum console_gate_command_kind
console_gate_command_classify(const char *line, size_t length,
                              bool *open_pending) {
    bool armed = open_pending != NULL && *open_pending;

    if (open_pending != NULL) {
        *open_pending = false;
    }
    if (console_gate_command_equals(
            line, length, CONSOLE_GATE_COMMAND_OPEN_CONFIRM_WORD)) {
        if (open_pending == NULL) {
            return CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED;
        }
        if (armed) {
            return CONSOLE_GATE_COMMAND_OPEN_CONFIRM;
        }
        return CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED;
    }
    if (console_gate_command_equals(
            line, length, CONSOLE_GATE_COMMAND_OPEN_WORD)) {
        if (open_pending != NULL) {
            *open_pending = true;
        }
        return CONSOLE_GATE_COMMAND_OPEN_REQUEST;
    }
    if (console_gate_command_equals(
            line, length, CONSOLE_GATE_COMMAND_CLOSE_WORD)) {
        return CONSOLE_GATE_COMMAND_CLOSE;
    }
    if (console_gate_command_equals(
            line, length, CONSOLE_GATE_COMMAND_WORD)) {
        return CONSOLE_GATE_COMMAND_SHOW;
    }
    /*
     * The word is not the four forms above.  A line whose first word is
     * `gate` is still this command's refusal - `gate opne` must not fall
     * through to the generic unknown-command answer.
     */
    {
        size_t index = 0U;

        while (index < length && line[index] != ' ') {
            ++index;
        }
        if (console_gate_command_equals(line, index,
                                        CONSOLE_GATE_COMMAND_WORD)) {
            return CONSOLE_GATE_COMMAND_REFUSE;
        }
    }
    return CONSOLE_GATE_COMMAND_NONE;
}

#endif
