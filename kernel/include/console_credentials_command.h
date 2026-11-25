#ifndef HFOC_CONSOLE_CREDENTIALS_COMMAND_H
#define HFOC_CONSOLE_CREDENTIALS_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-506: the console's account vocabulary.
 *
 * The owner asked for one understandable command instead of the several
 * `admin-*` ones, and for a password that is never an argument.  `credentials`
 * starts the change conversation (current password, new login, new password
 * twice) and `login` opens one bounded local session; both take no argument,
 * so a typed password cannot reach the input history or a command record.
 *
 * The break-glass reset ITEM-465 added must not disappear silently - a
 * forgotten password would otherwise brick the appliance - so it stays as the
 * same two-line gesture under the one command family: `credentials reset`
 * arms it, `credentials reset confirm` performs it, and any other submitted
 * line disarms it.  The classifier is pure, and the console calls it once per
 * submitted line, so the operator's gesture, the refusal of an unarmed
 * confirmation and the removal of the old names are one rule the host proof
 * drives, not two implementations that can drift.
 *
 * Matching is ASCII-case-insensitive and otherwise byte-exact: `CREDENTIALS`,
 * `Credentials` and `credentials` are the change command; `LOGIN` is the login
 * command; a line carrying anything else (a trailing word, a missing space) is
 * not any of them and clears the pending reset request.
 */
#define CONSOLE_CREDENTIALS_COMMAND_CHANGE_WORD "credentials"
#define CONSOLE_CREDENTIALS_COMMAND_LOGIN_WORD "login"
#define CONSOLE_CREDENTIALS_COMMAND_RESET_REQUEST_WORD "credentials reset"
#define CONSOLE_CREDENTIALS_COMMAND_RESET_CONFIRM_WORD "credentials reset confirm"

enum console_credentials_command_kind {
    /* None of the account commands: the caller treats the line as an ordinary
       command and the pending reset request, if any, is disarmed. */
    CONSOLE_CREDENTIALS_COMMAND_NONE = 0,
    /* `CREDENTIALS`: start the change conversation; nothing is written yet. */
    CONSOLE_CREDENTIALS_COMMAND_CHANGE = 1,
    /* `LOGIN`: start the local sign-in conversation. */
    CONSOLE_CREDENTIALS_COMMAND_LOGIN = 2,
    /* `CREDENTIALS RESET`: arm the gesture; nothing is reset yet. */
    CONSOLE_CREDENTIALS_COMMAND_RESET_ARM = 3,
    /* `CREDENTIALS RESET CONFIRM` with the gesture armed: perform the reset. */
    CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE = 4,
    /* `CREDENTIALS RESET CONFIRM` with nothing armed: refuse and say so. */
    CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED = 5,
};

static inline char console_credentials_command_fold(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static inline bool console_credentials_command_equals(const char *line,
                                                      size_t length,
                                                      const char *keyword) {
    size_t index = 0U;

    if (line == NULL || keyword == NULL) {
        return false;
    }
    while (keyword[index] != '\0') {
        if (index >= length ||
            console_credentials_command_fold(line[index]) != keyword[index]) {
            return false;
        }
        ++index;
    }
    return index == length;
}

/*
 * Classify one submitted console line and update the pending reset flag.  A
 * NULL `reset_pending` cannot carry the gesture, so it classifies as NONE.
 */
static inline enum console_credentials_command_kind
console_credentials_command_classify(const char *line, size_t length,
                                     bool *reset_pending) {
    if (reset_pending == NULL) {
        return CONSOLE_CREDENTIALS_COMMAND_NONE;
    }
    if (console_credentials_command_equals(
            line, length, CONSOLE_CREDENTIALS_COMMAND_RESET_CONFIRM_WORD)) {
        if (*reset_pending) {
            *reset_pending = false;
            return CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE;
        }
        return CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED;
    }
    if (console_credentials_command_equals(
            line, length, CONSOLE_CREDENTIALS_COMMAND_RESET_REQUEST_WORD)) {
        *reset_pending = true;
        return CONSOLE_CREDENTIALS_COMMAND_RESET_ARM;
    }
    *reset_pending = false;
    if (console_credentials_command_equals(
            line, length, CONSOLE_CREDENTIALS_COMMAND_CHANGE_WORD)) {
        return CONSOLE_CREDENTIALS_COMMAND_CHANGE;
    }
    if (console_credentials_command_equals(
            line, length, CONSOLE_CREDENTIALS_COMMAND_LOGIN_WORD)) {
        return CONSOLE_CREDENTIALS_COMMAND_LOGIN;
    }
    return CONSOLE_CREDENTIALS_COMMAND_NONE;
}

#endif
