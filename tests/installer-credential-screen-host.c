/*
 * ITEM-506 increment 4 host proof for the installer's account screen.
 *
 * The screen's conversation is the ITEM-506 prompt module in `MODE_SET`, and
 * this file drives the line editor on top of it: the caller's text stays
 * untouched while the machine loop paints the login and the password mask,
 * backspace removes one and clears an overflow, ENTER
 * submits, an over-long line is refused instead of cut, a mismatch restarts
 * the pair, and an empty line cancels with every buffer zeroed.  The machine
 * loop that paints is compiled out here (HFOC_INSTALLER_CREDENTIAL_SCREEN_HOST_TEST),
 * so the conversation is proven without a framebuffer.
 */

#include "installer_credential_screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INSTALLER_CREDENTIAL_SCREEN_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool text_contains(const char *text, const char *needle)
{
    return text != NULL && strstr(text, needle) != NULL;
}

static bool bytes_are_zero(const char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != '\0') {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-509 increment 20: the machine loop is compiled into this proof too, so
 * the regression that sent the QEMU preserve proof red - an ignored key ending
 * the conversation, because the loop's default branch returned instead of
 * reading on - is caught here in seconds instead of after a 40-minute QEMU run.
 * The display, the local input queue and the serial stream are the stubs below,
 * and the loop is driven with a scripted key sequence.
 */
static struct keyboard_event host_events[64];
static size_t host_event_count;
static size_t host_event_head;
static char host_serial[4096];
static size_t host_serial_length;
static char host_display[8192];
static size_t host_display_length;

static void host_reset(void)
{
    host_event_count = 0U;
    host_event_head = 0U;
    host_serial_length = 0U;
    host_serial[0] = '\0';
    host_display_length = 0U;
    host_display[0] = '\0';
}

static void host_queue_type(const char *text)
{
    while (*text != '\0') {
        host_events[host_event_count].type = KEYBOARD_EVENT_CHARACTER;
        host_events[host_event_count].character = *text;
        host_events[host_event_count].reported_cycles = 0U;
        ++host_event_count;
        ++text;
    }
}

static void host_queue_press(enum keyboard_event_type type)
{
    host_events[host_event_count].type = type;
    host_events[host_event_count].character = '\0';
    host_events[host_event_count].reported_cycles = 0U;
    ++host_event_count;
}

static bool host_serial_contains(const char *needle)
{
    return strstr(host_serial, needle) != NULL;
}

static bool host_display_contains(const char *needle)
{
    return strstr(host_display, needle) != NULL;
}

static void host_display_append(const char *text)
{
    while (text != NULL && *text != '\0' &&
           host_display_length + 1U < sizeof(host_display)) {
        host_display[host_display_length] = *text;
        ++host_display_length;
        ++text;
    }
    host_display[host_display_length] = '\0';
}

void local_input_service(void) {}
bool local_input_pop_event(struct keyboard_event *event)
{
    if (host_event_head >= host_event_count) {
        return false;
    }
    *event = host_events[host_event_head];
    ++host_event_head;
    return true;
}
uint64_t serial_sequence_begin(void) { return 0U; }
void serial_sequence_end(uint64_t flags) { (void)flags; }
void serial_write(const char *text)
{
    while (text != NULL && *text != '\0' &&
           host_serial_length + 1U < sizeof(host_serial)) {
        host_serial[host_serial_length] = *text;
        ++host_serial_length;
        ++text;
    }
    host_serial[host_serial_length] = '\0';
}
bool display_framebuffer_usable(void) { return true; }
uint32_t display_scale(void) { return 1U; }
void display_clear(uint32_t colour) { (void)colour; }
void display_text(uint64_t x, uint64_t y, const char *text, uint32_t colour,
                  uint32_t scale)
{
    (void)x;
    (void)y;
    (void)colour;
    (void)scale;
    host_display_append(text);
    host_display_append("\n");
}
void display_present(void) {}

static enum installer_credential_screen_action type_line(
    struct installer_credential_screen *screen,
    const char *line,
    char *text,
    size_t text_size,
    size_t *text_length)
{
    enum installer_credential_screen_action action =
        INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE;
    const char *cursor;

    for (cursor = line; *cursor != '\0'; ++cursor) {
        action = installer_credential_screen_key(
            screen, KEYBOARD_EVENT_CHARACTER, *cursor, text, text_size,
            text_length);
        if (action != INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE) {
            return action;
        }
    }
    return installer_credential_screen_key(
        screen, KEYBOARD_EVENT_ENTER, '\0', text, text_size, text_length);
}

static void test_set_conversation(void)
{
    static const char password[] = "install time passphrase";
    struct installer_credential_screen screen;
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;
    size_t length = 0U;

    memset(&screen, 0, sizeof(screen));
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_begin(&screen, APPLIANCE_LANGUAGE_DEFAULT,
                                            text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_STATUS_OK,
          "the installer account screen starts");
    CHECK(screen.active && !screen.complete &&
              text_contains(text, "New login"),
          "the first question is the new login");

    /* Typing never echoes: a character event leaves the text buffer alone. */
    memset(text, 0, sizeof(text));
    text_length = 0U;
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_CHARACTER, 'o', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE &&
              text_length == 0U && text[0] == '\0' && screen.line_length == 1U,
          "a typed character is kept hidden and paints nothing");
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_LEFT, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE &&
              screen.line_length == 1U,
          "an editing key the screen does not own changes nothing");
    (void)installer_credential_screen_key(
        &screen, KEYBOARD_EVENT_BACKSPACE, '\0', text, sizeof(text),
        &text_length);

    CHECK(type_line(&screen, "bad login", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "Login must be"),
          "a login outside the policy is refused by the model");

    /* A fresh valid login starts the password pair. */
    CHECK(type_line(&screen, "operator", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "New password"),
          "the login is taken and the password is asked");
    CHECK(type_line(&screen, "short", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "12-128"),
          "the installer enforces the password policy");
    CHECK(type_line(&screen, password, text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "Repeat new password"),
          "the password is asked again");
    CHECK(type_line(&screen, "different password", text, sizeof(text),
                    &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "do not match"),
          "a mismatch restarts the pair");
    CHECK(type_line(&screen, password, text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE,
          "the restarted pair asks the password again");
    CHECK(type_line(&screen, password, text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE,
          "the matching pair completes the screen");
    CHECK(screen.complete && !screen.active,
          "a completed screen is closed");
    CHECK(installer_credential_screen_login(&screen, &length) != NULL &&
              length == 8U &&
              memcmp(installer_credential_screen_login(&screen, &length),
                     "operator", 8U) == 0,
          "the completed login is readable");
    CHECK(installer_credential_screen_password(&screen, &length) != NULL &&
              length == strlen(password),
          "the completed password is readable by the caller");
    CHECK(bytes_are_zero(screen.line, sizeof(screen.line)),
          "the hidden line is zeroed after the last submit");
    installer_credential_screen_reset(&screen);
    CHECK(installer_credential_screen_login(&screen, &length) == NULL &&
              installer_credential_screen_password(&screen, &length) == NULL &&
              bytes_are_zero((const char *)&screen, sizeof(screen)),
          "reset zeroes the whole screen");
}

static void test_update_verify_conversation(void)
{
    static const char password[] = "existing update password";
    struct installer_credential_screen screen;
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;
    size_t length = 0U;

    memset(&screen, 0, sizeof(screen));
    CHECK(installer_credential_screen_begin_mode(
              &screen, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN,
              APPLIANCE_LANGUAGE_DEFAULT, text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_STATUS_OK &&
              text_contains(text, "Login"),
          "an update starts by asking for the existing login");
    CHECK(type_line(&screen, "operator", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "Password"),
          "an update asks for the existing password");
    CHECK(type_line(&screen, password, text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE &&
              screen.complete && !screen.active,
          "the existing pair completes without a new-password step");
    CHECK(installer_credential_screen_login(&screen, &length) != NULL &&
              length == strlen("operator") &&
              installer_credential_screen_password(&screen, &length) != NULL &&
              length == strlen(password),
          "the update caller receives the pair it must verify");
    installer_credential_screen_reset(&screen);
}

static void test_overflow_and_recovery(void)
{
    struct installer_credential_screen screen;
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;
    size_t index;

    memset(&screen, 0, sizeof(screen));
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_begin(&screen, APPLIANCE_LANGUAGE_DEFAULT,
                                            text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_STATUS_OK,
          "the overflow fixture starts");
    for (index = 0U;
         index < INSTALLER_CREDENTIAL_SCREEN_LINE_MAX + 1U; ++index) {
        (void)installer_credential_screen_key(
            &screen, KEYBOARD_EVENT_CHARACTER, 'a', text, sizeof(text),
            &text_length);
    }
    CHECK(screen.overflow &&
              screen.line_length == INSTALLER_CREDENTIAL_SCREEN_LINE_MAX,
          "a character past the cap is remembered as an overflow");
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_ENTER, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "longer than the policy allows"),
          "an over-long line is refused as too long");
    /* The refusal zeroes the line; the flag must not poison the next one. */
    CHECK(!screen.overflow && screen.line_length == 0U,
          "the refusal zeroes the line and the overflow");
    for (index = 0U;
         index < INSTALLER_CREDENTIAL_SCREEN_LINE_MAX + 1U; ++index) {
        (void)installer_credential_screen_key(
            &screen, KEYBOARD_EVENT_CHARACTER, 'a', text, sizeof(text),
            &text_length);
    }
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_BACKSPACE, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE &&
              !screen.overflow,
          "one backspace brings the line back under the cap");
    /* Trim the 128-character line to a valid login and submit it. */
    for (index = 0U; index < 100U; ++index) {
        (void)installer_credential_screen_key(
            &screen, KEYBOARD_EVENT_BACKSPACE, '\0', text, sizeof(text),
            &text_length);
    }
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_ENTER, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "New password"),
          "the shortened line submits instead of staying refused");
}

static void test_cancel_zeroes(void)
{
    struct installer_credential_screen screen;
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;

    memset(&screen, 0, sizeof(screen));
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_begin(&screen, APPLIANCE_LANGUAGE_DEFAULT,
                                            text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_STATUS_OK,
          "the cancel fixture starts");
    /*
     * A login with a space is outside the policy: it is refused, not
     * cancelled, and the hidden line is zeroed by the refusal.
     */
    CHECK(type_line(&screen, "a b", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "Login must be") && screen.active &&
              screen.line_length == 0U,
          "an invalid login is refused, not cancelled");
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_ENTER, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED &&
              text_contains(text, "cancelled") && !screen.active &&
              !screen.complete,
          "an empty line cancels and says so");
    CHECK(bytes_are_zero(screen.line, sizeof(screen.line)) &&
              bytes_are_zero((const char *)&screen.prompt,
                             sizeof(screen.prompt)),
          "a cancelled conversation zeroes the line and the prompt");
    CHECK(installer_credential_screen_login(&screen, &text_length) == NULL &&
              installer_credential_screen_password(&screen, &text_length) ==
                  NULL,
          "a cancelled screen exposes no pair");
}

/*
 * ITEM-509 increment 20: the screen carries one language - the questions come
 * from the shared prompt module and the title and hint from the catalog in the
 * same language - and a language the catalog cannot answer starts no
 * conversation and names the refusal.
 */
static void test_russian_screen(void)
{
    struct installer_credential_screen screen;
    char text[INSTALLER_CREDENTIAL_SCREEN_TEXT_MAX];
    size_t text_length = 0U;

    CHECK(strcmp(installer_credential_screen_title(APPLIANCE_LANGUAGE_DEFAULT),
                 "HFOC OS installer - account") == 0 &&
              strcmp(installer_credential_screen_hint(
                         APPLIANCE_LANGUAGE_DEFAULT),
                     "The password is shown as asterisks as you type.") == 0,
          "the English title and hint are the literals the screen painted");
    CHECK(strcmp(installer_credential_screen_title(APPLIANCE_LANGUAGE_RU),
                 "Установщик HFOC OS — учётная запись") == 0 &&
              strcmp(installer_credential_screen_hint(APPLIANCE_LANGUAGE_RU),
                     "Пароль показывается звёздочками при вводе.") == 0,
          "the Russian title and hint come from the catalog column");
    CHECK(installer_credential_screen_title((enum appliance_language)7) ==
                  NULL &&
              installer_credential_screen_hint((enum appliance_language)7) ==
                  NULL,
          "an unknown language answers NULL instead of falling back");

    memset(&screen, 0, sizeof(screen));
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_begin(&screen, APPLIANCE_LANGUAGE_RU,
                                            text, sizeof(text),
                                            &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_STATUS_OK &&
              screen.language == APPLIANCE_LANGUAGE_RU &&
              strcmp(text, "Новый логин: ") == 0,
          "the Russian account screen starts with the Russian question");
    CHECK(type_line(&screen, "bad login", text, sizeof(text), &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_ACTION_CONTINUE &&
              text_contains(text, "Логин: 1-32 символа") &&
              text_contains(text, "Новый логин: "),
          "the Russian login refusal repeats the Russian question");
    CHECK(installer_credential_screen_key(
              &screen, KEYBOARD_EVENT_ENTER, '\0', text, sizeof(text),
              &text_length) == INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED &&
              strcmp(text, "Ввод учётных данных отменён.") == 0,
          "the Russian cancellation notice comes from the catalog");
    installer_credential_screen_reset(&screen);

    memset(&screen, 0, sizeof(screen));
    memset(text, 0, sizeof(text));
    CHECK(installer_credential_screen_begin(&screen, (enum appliance_language)7,
                                            text, sizeof(text),
                                            &text_length) ==
              INSTALLER_CREDENTIAL_SCREEN_STATUS_MISSING_TRANSLATION &&
              strcmp(text, "CREDENTIAL TRANSLATION MISSING") == 0 &&
              !screen.active,
          "an unknown language starts no conversation and names the refusal");
}

/*
 * ITEM-509 increment 20: the machine loop drives the whole conversation.  A
 * typed character and a key the screen does not own are NONE actions with the
 * screen still active; they must be ignored, not treated as the end of the
 * conversation.  The first scripted event is deliberately an arrow key, and the
 * assertions require every later question to have been painted and serialised.
 */
static void test_the_machine_loop_ignores_ignored_keys(void)
{
    static const char password[] = "install time passphrase";
    struct installer_credential_screen screen;
    enum installer_credential_screen_action action;

    memset(&screen, 0, sizeof(screen));
    host_reset();
    host_queue_press(KEYBOARD_EVENT_LEFT);
    host_queue_type("operator");
    host_queue_press(KEYBOARD_EVENT_ENTER);
    host_queue_type("short");
    host_queue_press(KEYBOARD_EVENT_ENTER);
    host_queue_type(password);
    host_queue_press(KEYBOARD_EVENT_ENTER);
    host_queue_type(password);
    host_queue_press(KEYBOARD_EVENT_ENTER);
    action = installer_credential_screen_run(&screen,
                                             APPLIANCE_LANGUAGE_DEFAULT);
    CHECK(action == INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE &&
              screen.complete,
          "the machine loop completes the whole account conversation");
    CHECK(host_serial_contains("New login: ") &&
              host_serial_contains("New password: ") &&
              host_serial_contains("Repeat new password: "),
          "an ignored key does not end the conversation before its questions");
    CHECK(host_serial_contains("12-128") &&
              host_serial_contains("New password: "),
          "the loop paints the refusal and keeps reading");
    CHECK(host_display_contains("operator"),
          "the installer login is visible as it is typed");
    {
        char stars[sizeof(password)];
        size_t index;

        for (index = 0U; index + 1U < sizeof(stars); ++index) {
            stars[index] = '*';
        }
        stars[sizeof(stars) - 1U] = '\0';
        CHECK(host_display_contains(stars),
              "the installer password paints one star per character");
    }
    CHECK(!host_serial_contains("operator") &&
              !host_serial_contains(password),
          "the installer typed answer stays off the serial stream");
    CHECK(host_serial_contains("Checking the account..."),
          "the completed account paints the check line before the caller runs it");
    CHECK(host_event_head == host_event_count,
          "the loop consumed every scripted event");
    CHECK(!host_serial_contains("INSTALLER TRANSLATION MISSING"),
          "the English conversation never paints the named refusal");
}

/*
 * ITEM-509 increment 20: the loop with a language the catalog cannot answer
 * paints the one named refusal and leaves with NONE instead of spinning on a
 * closed conversation.
 */
static void test_the_machine_loop_names_a_missing_translation(void)
{
    struct installer_credential_screen screen;
    enum installer_credential_screen_action action;

    memset(&screen, 0, sizeof(screen));
    host_reset();
    action = installer_credential_screen_run(&screen,
                                             (enum appliance_language)7);
    CHECK(action == INSTALLER_CREDENTIAL_SCREEN_ACTION_NONE && !screen.active,
          "the machine loop leaves when the catalog cannot answer");
    CHECK(host_serial_contains("INSTALLER TRANSLATION MISSING"),
          "the machine loop paints the named refusal, not a blank screen");
}

int main(void)
{
    test_set_conversation();
    test_update_verify_conversation();
    test_overflow_and_recovery();
    test_cancel_zeroes();
    test_russian_screen();
    test_the_machine_loop_ignores_ignored_keys();
    test_the_machine_loop_names_a_missing_translation();
    if (failures != 0) {
        fprintf(stderr, "installer credential screen host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("installer credential screen host test: ALL OK\n");
    return 0;
}

#endif
