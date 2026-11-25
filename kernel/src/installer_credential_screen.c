#include "installer_credential_screen.h"

#include <string.h>

#include "arch/x86_64/timer.h"
#include "display.h"
#include "local_input.h"
#include "serial.h"

/*
 * ITEM-506 increment 4 installer account screen.  See the header for the
 * contract; this file owns the hidden line and the two backends.
 */

#define INSTALLER_CREDENTIAL_SCREEN_VERSION 1U

/* The installer's palette, shared with the storage screens. */
#define CREDENTIAL_BACKGROUND UINT32_C(0x00101820)
#define CREDENTIAL_TITLE_COLOUR UINT32_C(0x00F0F4F8)
#define CREDENTIAL_TEXT_COLOUR UINT32_C(0x00C0D0E0)
#define CREDENTIAL_HINT_COLOUR UINT32_C(0x00FFD070)
#define CREDENTIAL_MARGIN_PIXELS 16U
#define CREDENTIAL_LINE_PIXELS 10U

static void installer_credential_screen_zero(void *destination, size_t length)
{
    volatile uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void installer_credential_screen_zero_line(
    struct installer_credential_screen *screen)
{
    installer_credential_screen_zero(screen->line, sizeof(screen->line));
    screen->line_length = 0U;
    screen->overflow = false;
}

/*
 * ITEM-509 increment 20: the screen's two static lines come from the one
 * catalog; NULL means the catalog cannot answer and the machine loop paints the
 * named refusal instead.
 */
const char *installer_credential_screen_title(enum appliance_language language)
{
    return appliance_language_string(language, "installer.credential.title");
}

const char *installer_credential_screen_hint(enum appliance_language language)
{
    return appliance_language_string(
        language, "installer.credential.password_note");
}

enum installer_credential_screen_status installer_credential_screen_begin(
    struct installer_credential_screen *screen,
    enum appliance_language language,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    return installer_credential_screen_begin_mode(
        screen, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET, language,
        text_out, text_size, text_length_out);
}

enum installer_credential_screen_status
installer_credential_screen_begin_mode(
    struct installer_credential_screen *screen,
    enum appliance_admin_credentials_prompt_mode mode,
    enum appliance_language language,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    enum appliance_admin_credentials_prompt_status status;

    if (screen == NULL || text_out == NULL || text_length_out == NULL) {
        return INSTALLER_CREDENTIAL_SCREEN_STATUS_NULL;
    }
    installer_credential_screen_zero(screen, sizeof(*screen));
    screen->version = INSTALLER_CREDENTIAL_SCREEN_VERSION;
    screen->language = language;
    status = appliance_admin_credentials_prompt_start(
        &screen->prompt, mode, text_out, text_size, text_length_out, language);
    if (status != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK) {
        installer_credential_screen_zero(screen, sizeof(*screen));
        screen->version = INSTALLER_CREDENTIAL_SCREEN_VERSION;
        screen->language = language;
        if (status ==
            APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION) {
            return INSTALLER_CREDENTIAL_SCREEN_STATUS_MISSING_TRANSLATION;
        }
        return INSTALLER_CREDENTIAL_SCREEN_STATUS_BAD_STATE;
    }
    screen->active = true;
    return INSTALLER_CREDENTIAL_SCREEN_STATUS_OK;
}

enum installer_credential_screen_action installer_credential_screen_key(
    struct installer_credential_screen *screen,
    enum keyboard_event_type type,
    char character,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    enum appliance_admin_credentials_prompt_action action;
    size_t submitted_length;

    if (screen == NULL || text_out == NULL || text_length_out == NULL) {
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
    if (screen->version != INSTALLER_CREDENTIAL_SCREEN_VERSION ||
        !screen->active) {
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
    switch (type) {
    case KEYBOARD_EVENT_CHARACTER:
        if (character >= 32 && character <= 126) {
            if (screen->line_length < INSTALLER_CREDENTIAL_SCREEN_LINE_MAX) {
                screen->line[screen->line_length++] = character;
                screen->line[screen->line_length] = '\0';
            } else {
                screen->overflow = true;
            }
        }
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    case KEYBOARD_EVENT_BACKSPACE:
        if (screen->line_length != 0U) {
            --screen->line_length;
            screen->line[screen->line_length] = '\0';
            /* The line is under the cap again, so the refusal cannot stick. */
            screen->overflow = false;
        }
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    case KEYBOARD_EVENT_ENTER:
        break;
    default:
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
    if (screen->line_length == 0U && !screen->overflow) {
        /* An empty line is the way out; nothing is committed. */
        (void)appliance_admin_credentials_prompt_cancel(
            &screen->prompt, text_out, text_size, text_length_out);
        installer_credential_screen_zero_line(screen);
        screen->active = false;
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED;
    }
    /*
     * A line that did not fit is submitted with the synthetic over-long length:
     * the module refuses it without reading the line, so it is never cut to a
     * legal password.
     */
    submitted_length = screen->overflow
        ? INSTALLER_CREDENTIAL_SCREEN_LINE_MAX + 1U
        : screen->line_length;
    action = appliance_admin_credentials_prompt_submit(
        &screen->prompt, screen->line, submitted_length, text_out, text_size,
        text_length_out);
    installer_credential_screen_zero_line(screen);
    switch (action) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT:
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN:
        screen->active = false;
        screen->complete = true;
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED:
        screen->active = false;
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED;
    default:
        appliance_admin_credentials_prompt_reset(&screen->prompt);
        screen->active = false;
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
}

const char *installer_credential_screen_login(
    const struct installer_credential_screen *screen,
    size_t *length_out)
{
    if (screen == NULL || !screen->complete) {
        return NULL;
    }
    return appliance_admin_credentials_prompt_login(&screen->prompt,
                                                    length_out);
}

const char *installer_credential_screen_password(
    const struct installer_credential_screen *screen,
    size_t *length_out)
{
    if (screen == NULL || !screen->complete) {
        return NULL;
    }
    return appliance_admin_credentials_prompt_password(&screen->prompt,
                                                       length_out);
}

void installer_credential_screen_reset(
    struct installer_credential_screen *screen)
{
    if (screen == NULL) {
        return;
    }
    installer_credential_screen_zero(screen, sizeof(*screen));
}

/* --------------------------------------------------------------- the screen */

/*
 * The machine loop needs the display, the local input queue and the serial
 * stream; the host proof provides all three as stubs, so the loop the QEMU
 * proof drives is the loop the host proof drives too - an ignored key must
 * never end the conversation.
 */

static void installer_credential_screen_serial(
    const char *title,
    const char *question,
    const char *hint)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write(title);
    serial_write("\n");
    serial_write(question);
    serial_write("\n");
    serial_write(hint);
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-610: the installer's account screen answers a keystroke the same way the
 * installed console does.  The login is visible as it is typed and every
 * password step paints one `*` per accepted character; the feedback is
 * framebuffer-only, so the serial stream keeps no credential answer and no real
 * password byte is ever painted.
 */
static bool installer_credential_screen_step_is_secret(
    const struct installer_credential_screen *screen)
{
    switch (screen->prompt.step) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:
        return true;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME:
    default:
        return false;
    }
}

static void installer_credential_screen_echo(
    const struct installer_credential_screen *screen,
    char *echo,
    size_t capacity)
{
    size_t length;

    if (echo == NULL || capacity == 0U) {
        return;
    }
    length = screen->line_length;
    if (length >= capacity) {
        length = capacity - 1U;
    }
    for (size_t index = 0U; index < length; ++index) {
        echo[index] = installer_credential_screen_step_is_secret(screen)
                          ? '*'
                          : screen->line[index];
    }
    echo[length] = '\0';
}

static void installer_credential_screen_render(
    const struct installer_credential_screen *screen,
    const char *title,
    const char *question,
    const char *hint,
    bool to_serial)
{
    const uint32_t scale = display_scale() > 0U ? display_scale() : 1U;
    const uint64_t margin = (uint64_t)CREDENTIAL_MARGIN_PIXELS * scale;
    uint64_t y = margin;
    char echo[INSTALLER_CREDENTIAL_SCREEN_LINE_MAX + 1U];

    installer_credential_screen_echo(screen, echo, sizeof(echo));
    if (display_framebuffer_usable()) {
        display_clear(CREDENTIAL_BACKGROUND);
        display_text(margin, y, title, CREDENTIAL_TITLE_COLOUR, scale);
        y += (uint64_t)CREDENTIAL_LINE_PIXELS * scale * 2U;
        display_text(margin, y, question, CREDENTIAL_TEXT_COLOUR, scale);
        y += (uint64_t)CREDENTIAL_LINE_PIXELS * scale * 2U;
        display_text(margin, y, hint, CREDENTIAL_HINT_COLOUR, scale);
        y += (uint64_t)CREDENTIAL_LINE_PIXELS * scale * 2U;
        display_text(margin, y, echo, CREDENTIAL_TEXT_COLOUR, scale);
        display_present();
    }
    memset(echo, 0, sizeof(echo));
    if (to_serial) {
        installer_credential_screen_serial(title, question, hint);
    }
}

static void installer_credential_screen_show(
    const struct installer_credential_screen *screen,
    const char *text)
{
    char line[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    const char *title = installer_credential_screen_title(screen->language);
    const char *hint = installer_credential_screen_hint(screen->language);
    size_t length = text != NULL ? strlen(text) : 0U;

    /*
     * `text` is the prompt module's question or refusal, which never contains a
     * secret; the typed line is painted separately by the renderer's mask.
     */
    if (length >= sizeof(line)) {
        length = sizeof(line) - 1U;
    }
    memset(line, 0, sizeof(line));
    if (length != 0U) {
        memcpy(line, text, length);
    }
    line[length] = '\0';
    /*
     * ITEM-509 increment 20: a catalog that cannot answer is one visible named
     * refusal, not a blank screen and not an English title over a Russian
     * question.
     */
    if (title == NULL || hint == NULL) {
        installer_credential_screen_render(
            screen, "INSTALLER TRANSLATION MISSING", "", "", true);
    } else {
        installer_credential_screen_render(screen, title, line, hint, true);
    }
    memset(line, 0, sizeof(line));
}

/*
 * Per-keystroke refresh: the same screen with the current masked/login line,
 * framebuffer-only, so no serial record is duplicated for every character.
 */
static void installer_credential_screen_refresh(
    const struct installer_credential_screen *screen,
    const char *text)
{
    char line[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    const char *title = installer_credential_screen_title(screen->language);
    const char *hint = installer_credential_screen_hint(screen->language);
    size_t length = text != NULL ? strlen(text) : 0U;

    if (length >= sizeof(line)) {
        length = sizeof(line) - 1U;
    }
    memset(line, 0, sizeof(line));
    if (length != 0U) {
        memcpy(line, text, length);
    }
    line[length] = '\0';
    if (title == NULL || hint == NULL) {
        installer_credential_screen_render(
            screen, "INSTALLER TRANSLATION MISSING", "", "", false);
    } else {
        installer_credential_screen_render(screen, title, line, hint, false);
    }
    memset(line, 0, sizeof(line));
}

/*
 * ITEM-610: the account has been entered and the caller is about to verify or
 * commit it.  Paint that fact before the synchronous work, with the same named
 * translation refusal as the questions.
 */
static void installer_credential_screen_checking(
    const struct installer_credential_screen *screen)
{
    const char *title = installer_credential_screen_title(screen->language);
    const char *hint = installer_credential_screen_hint(screen->language);
    const char *checking = appliance_language_string(
        screen->language, "installer.credential.checking");

    if (title == NULL || hint == NULL) {
        installer_credential_screen_render(
            screen, "INSTALLER TRANSLATION MISSING", "", "", true);
        return;
    }
    if (checking == NULL) {
        installer_credential_screen_render(
            screen, title, "CREDENTIAL TRANSLATION MISSING", hint, true);
        return;
    }
    installer_credential_screen_render(screen, title, checking, hint, true);
}

enum installer_credential_screen_action installer_credential_screen_run(
    struct installer_credential_screen *screen,
    enum appliance_language language)
{
    return installer_credential_screen_run_mode(
        screen, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET, language);
}

enum installer_credential_screen_action installer_credential_screen_run_mode(
    struct installer_credential_screen *screen,
    enum appliance_admin_credentials_prompt_mode mode,
    enum appliance_language language)
{
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;

    if (screen == NULL) {
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
    if (installer_credential_screen_begin_mode(
            screen, mode, language, text, sizeof(text), &text_length) !=
        INSTALLER_CREDENTIAL_SCREEN_STATUS_OK) {
        /*
         * ITEM-509 increment 20: the catalog could not answer, so the screen
         * paints its named refusal instead of a question nobody translated.
         */
        installer_credential_screen_show(screen, "");
        return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    }
    installer_credential_screen_show(screen, text);
    for (;;) {
        struct keyboard_event event;

        local_input_service();
        while (local_input_pop_event(&event)) {
            char next[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
            size_t next_length = 0U;
            enum installer_credential_screen_action action =
                installer_credential_screen_key(
                    screen, event.type, event.character, next, sizeof(next),
                    &next_length);

            switch (action) {
            case INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE: {
                size_t copy = next_length;

                if (copy >= sizeof(text)) {
                    copy = sizeof(text) - 1U;
                }
                memcpy(text, next, copy);
                text[copy] = '\0';
                installer_credential_screen_show(screen, text);
                break;
            }
            case INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE:
                /*
                 * ITEM-610: the verification that follows can take a while, so
                 * the operator sees an honest, localised checking line before
                 * the synchronous work starts.
                 */
                installer_credential_screen_checking(screen);
                return action;
            case INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED:
                return action;
            case INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE:
            default:
                /*
                 * A NONE with the screen still active is an ordinary ignored
                 * key - a typed character, a key the screen does not own - and
                 * the loop keeps reading.  A NONE with the screen closed is the
                 * module's named refusal: it is painted and the loop leaves, so
                 * a missing translation can neither be swallowed nor end the
                 * conversation on the first keystroke.
                 */
                if (screen->active) {
                    installer_credential_screen_refresh(screen, text);
                    break;
                }
                if (next_length != 0U) {
                    installer_credential_screen_show(screen, next);
                }
                return INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
            }
        }
    }
}
