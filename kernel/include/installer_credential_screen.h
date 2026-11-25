#ifndef HFOC_INSTALLER_CREDENTIAL_SCREEN_H
#define HFOC_INSTALLER_CREDENTIAL_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_admin_credential.h"
#include "appliance_admin_credentials_prompt.h"
#include "appliance_language.h"
#include "keyboard.h"

/*
 * ITEM-506 increment 4: the installer's account screen.
 *
 * The owner wants the login and the password set when the system is installed,
 * not only on the first console after it.  The storage screens already own the
 * machine while the operator is choosing a disk; this screen is the same shape
 * one step later: it paints one question, reads the operator's line **without
 * echoing it**, and hands the finished pair to the install flow.
 *
 * The conversation itself is the ITEM-506 prompt module in `MODE_SET`
 * (`appliance_admin_credentials_prompt`), which already enforces the policy
 * (login 1..32 of `A-Za-z0-9._-`, password 12..128 printable, no trimming,
 * weak list refused, a mismatch restarts the pair).  This module owns only the
 * hidden line editor and the screen: a line that overflows the buffer is
 * refused as too long instead of being cut, the line is zeroed after every
 * step, and nothing about the password is ever painted or written to the
 * serial stream.
 *
 * The key handling is a pure function of one event at a time
 * (`installer_credential_screen_key`), so the host proof drives the whole
 * conversation, the refusals and the zeroization without a framebuffer.
 * `installer_credential_screen_run` is the thin machine loop: it paints the
 * question through the display abstraction and publishes the same lines to the
 * serial stream, exactly as the storage screen does, so a machine whose only
 * output is serial is not a special case.
 */

#define INSTALLER_CREDENTIAL_SCREEN_LINE_MAX \
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX
#define INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX \
    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX

enum installer_credential_screen_status {
    INSTALLER_CREDENTIAL_SCREEN_STATUS_OK = 0,
    INSTALLER_CREDENTIAL_SCREEN_STATUS_NULL = 1,
    INSTALLER_CREDENTIAL_SCREEN_STATUS_BAD_STATE = 2,
    INSTALLER_CREDENTIAL_SCREEN_STATUS_BUFFER_TOO_SMALL = 3,
    /*
     * ITEM-509 increment 20: the catalog cannot answer the questions or the
     * screen's own lines, so the conversation did not start; the named refusal
     * is what the machine loop paints.
     */
    INSTALLER_CREDENTIAL_SCREEN_STATUS_MISSING_TRANSLATION = 4,
};

enum installer_credential_screen_action {
    /* Nothing happened; the question on screen is still the current one. */
    INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE = 0,
    /* A step was answered; `text_out` holds the next question or refusal. */
    INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE = 1,
    /* The pair is complete and validated; the caller commits the account. */
    INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE = 2,
    /* The operator pressed ENTER on an empty line; nothing is committed. */
    INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED = 3,
};

struct installer_credential_screen {
    uint32_t version;
    /*
     * ITEM-509 increment 20: the conversation's language.  The questions come
     * from the shared prompt module and the title and hint below come from the
     * catalog in the same language, so the screen cannot speak two languages.
     */
    enum appliance_language language;
    struct appliance_admin_credentials_prompt prompt;
    char line[INSTALLER_CREDENTIAL_SCREEN_LINE_MAX + 1U];
    size_t line_length;
    /*
     * A character that did not fit the line is remembered, so an over-long
     * line is refused as too long instead of being silently cut to a legal
     * password; one backspace clears it.
     */
    bool overflow;
    bool active;
    bool complete;
};

/*
 * ITEM-509 increment 20: the screen's two static lines, read from the catalog.
 * Each returns NULL for a language the catalog cannot answer; the screen paints
 * the named refusal instead of those lines and never falls back to English.
 */
const char *installer_credential_screen_title(enum appliance_language language);
const char *installer_credential_screen_hint(enum appliance_language language);

/* Start the conversation; `text_out` receives the first question. */
enum installer_credential_screen_status installer_credential_screen_begin(
    struct installer_credential_screen *screen,
    enum appliance_language language,
    char *text_out,
    size_t text_size,
    size_t *text_length_out);
/*
 * ITEM-598: an update verifies the existing account before it can provision
 * the host seed.  It uses the same hidden line and prompt owner, but starts in
 * LOGIN mode so it never overwrites the preserved credential.
 */
enum installer_credential_screen_status
installer_credential_screen_begin_mode(
    struct installer_credential_screen *screen,
    enum appliance_admin_credentials_prompt_mode mode,
    enum appliance_language language,
    char *text_out,
    size_t text_size,
    size_t *text_length_out);

/*
 * Feed one keyboard event.  CHARACTER appends (never beyond the cap; the
 * overflow is remembered), BACKSPACE removes one and clears the overflow,
 * ENTER submits the hidden line, and every other event is ignored.  `text_out`
 * is filled only for CONTINUE (the next question or the refusal).
 */
enum installer_credential_screen_action installer_credential_screen_key(
    struct installer_credential_screen *screen,
    enum keyboard_event_type type,
    char character,
    char *text_out,
    size_t text_size,
    size_t *text_length_out);

/* The completed pair; NULL until COMPLETE (and after reset/zero). */
const char *installer_credential_screen_login(
    const struct installer_credential_screen *screen,
    size_t *length_out);
const char *installer_credential_screen_password(
    const struct installer_credential_screen *screen,
    size_t *length_out);

/* Zero every secret buffer and return the screen to its idle state. */
void installer_credential_screen_reset(
    struct installer_credential_screen *screen);

/*
 * The machine loop: paint the current question, read local input until the
 * pair is complete or the operator cancels, and return the action.  Never
 * paints, echoes or serialises a typed character.  The language is explicit, as
 * on every other installer screen.
 */
enum installer_credential_screen_action installer_credential_screen_run(
    struct installer_credential_screen *screen,
    enum appliance_language language);
enum installer_credential_screen_action installer_credential_screen_run_mode(
    struct installer_credential_screen *screen,
    enum appliance_admin_credentials_prompt_mode mode,
    enum appliance_language language);

#endif
