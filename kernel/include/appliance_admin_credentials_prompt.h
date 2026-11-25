#ifndef HFOC_APPLIANCE_ADMIN_CREDENTIALS_PROMPT_H
#define HFOC_APPLIANCE_ADMIN_CREDENTIALS_PROMPT_H

#include "appliance_admin_credential.h"
#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-506: the console's credential questions, as a pure state machine.
 *
 * The owner wants one account and one command that changes it (`credentials`),
 * and a local login beside it.  Both are conversations: the console asks for
 * one line at a time, and the operator answers.  This module owns the
 * conversation and nothing else - it holds no device, prints nothing, verifies
 * nothing and commits nothing.  The caller feeds it one entered line per call,
 * gets back either the next question or a terminal action, and is the one that
 * asks the enrollment owner to verify the current password, to set the new
 * credentials or to open the local session.
 *
 * The contract the module enforces:
 *
 *   * `credentials` asks the current password, then the new login, then the
 *     new password twice; a mismatch restarts the password pair, and a
 *     refused login or password keeps the same question with the reason said
 *     out loud;
 *   * `login` asks the login, then the password;
 *   * `set` (ITEM-506 increment 4) asks the new login, then the new password
 *     twice and stops: it is the installer's first set, where no current
 *     password exists to prove and the caller commits the pair directly;
 *   * no secret is ever accepted as an argument: every value arrives one line
 *     at a time through `submit()`, and the caller is expected to read those
 *     lines without echoing them;
 *   * a line longer than the model's bounds is refused as too long, never
 *     truncated; the login is validated by the credential model before it is
 *     kept, and the password the same way;
 *   * an end of input (EOF, a closed channel, a console told to stand down) is
 *     not an empty line: the caller abandons the conversation with `cancel()`,
 *     which is the only way out that zeroes the typed values, because
 *     `submit()` deliberately treats `line == NULL, line_length == 0` as the
 *     operator pressing ENTER on an empty line and re-asks instead of reading
 *     it as "the input is gone";
 *   * every buffer is zeroed by `cancel()` and `reset()`, so the console can
 *     drop the conversation the moment it is told to;
 *   * ITEM-509 increment 20: the questions and the refusals come from the one
 *     string catalog in the conversation's language - the module holds no
 *     English copy of them - and a language the catalog cannot answer is the
 *     named refusal `MISSING_TRANSLATION`, never a silent English question.
 */

#define APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION 1U
/*
 * One entered line can be as long as the model's password maximum; a longer
 * line is refused instead of cut.  The console's own line buffer is the one
 * that ultimately bounds the operator's input (increment 3 raises it to this
 * value).
 */
#define APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX \
    APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX
#define APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX 160U

enum appliance_admin_credentials_prompt_mode {
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_NONE = 0,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE = 1,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN = 2,
    /* The installer's first set: login, then the new password twice. */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET = 3,
};

enum appliance_admin_credentials_prompt_step {
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE = 0,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT = 1,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN = 2,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD = 3,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM = 4,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME = 5,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD = 6,
};

enum appliance_admin_credentials_prompt_action {
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE = 0,
    /* `text_out` holds the next question (or the refusal and the question). */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT = 1,
    /* The caller verifies `current`, then commits `login` + `password`. */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE = 2,
    /* The caller authenticates `login` + `password`. */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN = 3,
    /* The conversation is over; the buffers are already zeroed. */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED = 4,
    /* MODE_SET finished: the caller commits `login` + `password` directly. */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET = 5,
};

enum appliance_admin_credentials_prompt_status {
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK = 0,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL = 1,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE = 2,
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL = 3,
    /*
     * ITEM-509 increment 20: the catalog cannot answer a question or a refusal
     * in the conversation's language.  The named refusal is spoken instead of
     * an empty question or a silent fall back to English.
     */
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION = 4,
};

struct appliance_admin_credentials_prompt {
    uint32_t version;
    /*
     * ITEM-509 increment 20: every question and refusal is read from the one
     * catalog in this language; the zero value is `EN`, so a zeroed or reset
     * conversation speaks the default.
     */
    enum appliance_language language;
    enum appliance_admin_credentials_prompt_mode mode;
    enum appliance_admin_credentials_prompt_step step;
    char current[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 1U];
    size_t current_length;
    char login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    size_t login_length;
    char password[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 1U];
    size_t password_length;
    char confirm[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 1U];
    size_t confirm_length;
    enum appliance_admin_credentials_prompt_status last_status;
};

/*
 * Begin a conversation and write the first question into `text_out`.  A
 * conversation already in progress is refused as BAD_STATE; the caller must
 * `cancel()` or `reset()` it first.  The language is the conversation's own:
 * the question, every later refusal and the cancellation notice come from the
 * catalog in it, and an answer the catalog cannot give is
 * MISSING_TRANSLATION with the named refusal in `text_out`.
 */
enum appliance_admin_credentials_prompt_status
appliance_admin_credentials_prompt_start(
    struct appliance_admin_credentials_prompt *prompt,
    enum appliance_admin_credentials_prompt_mode mode,
    char *text_out,
    size_t text_size,
    size_t *text_length_out,
    enum appliance_language language);
/*
 * Feed one entered line (without its newline) and get the next question or a
 * terminal action.  A refused line keeps the conversation on the same step and
 * the returned text says why before repeating the question.
 *
 * `text_out` must hold a refusal and a question (`TEXT_MAX` is the bound the
 * module guarantees).  A smaller buffer is a hard failure, not a retry: the
 * answer has already been consumed and the step may already have advanced, so
 * the caller abandons the conversation with `cancel()` when the call returns
 * ACTION_NONE with BUFFER_TOO_SMALL.
 */
enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_submit(
    struct appliance_admin_credentials_prompt *prompt,
    const char *line,
    size_t line_length,
    char *text_out,
    size_t text_size,
    size_t *text_length_out);
/* End the conversation now, zeroing every buffer; the action is CANCELLED. */
enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_cancel(
    struct appliance_admin_credentials_prompt *prompt,
    char *text_out,
    size_t text_size,
    size_t *text_length_out);
void appliance_admin_credentials_prompt_reset(
    struct appliance_admin_credentials_prompt *prompt);
const char *appliance_admin_credentials_prompt_login(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out);
const char *appliance_admin_credentials_prompt_password(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out);
const char *appliance_admin_credentials_prompt_current(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out);
const char *appliance_admin_credentials_prompt_mode_string(
    enum appliance_admin_credentials_prompt_mode mode);
const char *appliance_admin_credentials_prompt_step_string(
    enum appliance_admin_credentials_prompt_step step);
const char *appliance_admin_credentials_prompt_action_string(
    enum appliance_admin_credentials_prompt_action action);
const char *appliance_admin_credentials_prompt_status_string(
    enum appliance_admin_credentials_prompt_status status);

#endif
