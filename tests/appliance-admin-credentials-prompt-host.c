/*
 * ITEM-506 host proof for the console credential conversation.
 *
 * The module is pure: it asks the questions, enforces the model's policy one
 * line at a time, restarts the password pair on a mismatch and zeroes every
 * buffer when the conversation ends.  The proof drives both conversations
 * (change and login) through refusals, boundaries and cancellation, and
 * checks that no prompt text ever repeats a secret.
 */

#include "appliance_admin_credentials_prompt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_ADMIN_CREDENTIALS_PROMPT_HOST_TEST

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

/*
 * The zeroization claims are about bytes, not about the length counters: a
 * buffer whose length says zero but whose bytes still hold the password is
 * exactly the regression these checks exist for.
 */
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

static enum appliance_admin_credentials_prompt_action submit(
    struct appliance_admin_credentials_prompt *prompt,
    const char *line,
    char *text,
    size_t text_size,
    size_t *text_length)
{
    *text_length = 0U;
    text[0] = '\0';
    return appliance_admin_credentials_prompt_submit(
        prompt, line, line == NULL ? 0U : strlen(line), text, text_size,
        text_length);
}

static void test_change_conversation(void)
{
    static const char password[] = "correct horse battery staple";
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;
    size_t length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the change conversation starts");
    CHECK(prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT &&
          text_contains(text, "Current password"),
          "the first question asks the current password");

    CHECK(submit(&prompt, "", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "cannot be empty") &&
          prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT,
          "an empty current password is refused and re-asked");

    CHECK(submit(&prompt, "old password", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "New login"),
          "the current password is taken and the login is asked");

    CHECK(submit(&prompt, "bad login", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Login must be") &&
          prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN,
          "a login outside the policy is refused and re-asked");

    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "New password"),
          "the login is taken and the password is asked");

    CHECK(submit(&prompt, "short", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "12-128") &&
          prompt.step ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD,
          "a short password is refused with the rule");
    CHECK(submit(&prompt, "password", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "weak list"),
          "a weak password is refused by name");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "not the login"),
          "a password equal to the login is refused");
    {
        static const char spaces[] = "            ";
        CHECK(submit(&prompt, spaces, text, sizeof(text), &text_length) ==
                  APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "not all spaces"),
              "an all-space password is refused");
    }

    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Repeat new password") &&
          prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM,
          "the accepted password is asked again");
    CHECK(submit(&prompt, "different password", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "do not match") &&
          prompt.step ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD &&
          prompt.password_length == 0U,
          "a mismatch restarts the password pair and drops the first value");
    CHECK(bytes_are_zero(prompt.password, sizeof(prompt.password)),
          "a mismatch zeroes the dropped first password bytes");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Repeat new password"),
          "the password pair restarts cleanly");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE,
          "the matching pair submits the change");
    CHECK(appliance_admin_credentials_prompt_login(&prompt, &length) != NULL &&
          length == 8U &&
          strncmp(appliance_admin_credentials_prompt_login(&prompt, &length),
                  "operator", 8U) == 0,
          "the submitted login is readable");
    CHECK(appliance_admin_credentials_prompt_password(&prompt, &length) !=
              NULL &&
          length == strlen(password) &&
          strncmp(appliance_admin_credentials_prompt_password(&prompt, &length),
                  password, length) == 0,
          "the submitted password is readable");
    CHECK(appliance_admin_credentials_prompt_current(&prompt, &length) !=
              NULL &&
          length == strlen("old password"),
          "the current password is readable for the verification step");
    CHECK(!text_contains(text, password),
          "the terminal action text never repeats the password");

    appliance_admin_credentials_prompt_reset(&prompt);
    CHECK(appliance_admin_credentials_prompt_login(&prompt, &length) == NULL &&
          appliance_admin_credentials_prompt_password(&prompt, &length) ==
              NULL &&
          appliance_admin_credentials_prompt_current(&prompt, &length) ==
              NULL,
          "reset zeroes every secret buffer");
    CHECK(bytes_are_zero(prompt.current, sizeof(prompt.current)) &&
          bytes_are_zero(prompt.login, sizeof(prompt.login)) &&
          bytes_are_zero(prompt.password, sizeof(prompt.password)) &&
          bytes_are_zero(prompt.confirm, sizeof(prompt.confirm)),
          "reset zeroes the secret bytes, not only the lengths");
}

static void test_set_conversation(void)
{
    static const char password[] = "install time passphrase";
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;
    size_t length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET, text,
              sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the installer set conversation starts");
    CHECK(prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN &&
              text_contains(text, "New login") &&
              strcmp(appliance_admin_credentials_prompt_mode_string(prompt.mode),
                     "SET") == 0,
          "the first question is the new login, never the current password");
    CHECK(submit(&prompt, "", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "Login must be"),
          "an empty login is refused");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "New password"),
          "the login is taken and the password is asked");
    CHECK(submit(&prompt, "short", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "12-128"),
          "the installer enforces the same password policy");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "Repeat new password"),
          "the accepted password is asked again");
    CHECK(submit(&prompt, "different password", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "do not match") &&
              prompt.step ==
                  APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD,
          "a mismatch restarts the pair on the installer too");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Repeat new password"),
          "the installer pair restarts cleanly");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET,
          "the matching pair submits the set, not a change");
    CHECK(strcmp(appliance_admin_credentials_prompt_action_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET),
                 "SUBMIT_SET") == 0,
          "the set action names itself");
    CHECK(appliance_admin_credentials_prompt_login(&prompt, &length) != NULL &&
              length == 8U,
          "the installer login is readable");
    CHECK(appliance_admin_credentials_prompt_password(&prompt, &length) !=
              NULL &&
              length == strlen(password),
          "the installer password is readable");
    CHECK(appliance_admin_credentials_prompt_current(&prompt, &length) == NULL,
          "the installer conversation never asks for a current password");
    CHECK(!text_contains(text, password),
          "the installer question text never repeats the password");
    appliance_admin_credentials_prompt_reset(&prompt);
    CHECK(appliance_admin_credentials_prompt_login(&prompt, &length) == NULL &&
              appliance_admin_credentials_prompt_password(&prompt, &length) ==
                  NULL,
          "reset zeroes the installer conversation");
}

static void test_login_conversation_and_cancel(void)
{
    static const char password[] = "correct horse battery staple";
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;
    size_t length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK &&
          text_contains(text, "Login"),
          "the login conversation starts with the login question");
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE,
          "a conversation already in progress is refused");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Password"),
          "the login is taken and the password is asked");
    CHECK(submit(&prompt, "", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "cannot be empty"),
          "an empty password is refused and re-asked");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN,
          "a non-empty password submits the login");

    /* A cancellation drops everything that was typed. */
    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the cancel fixture starts");
    CHECK(submit(&prompt, "old password", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT,
          "the cancel fixture takes the current password");
    CHECK(appliance_admin_credentials_prompt_cancel(
              &prompt, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED &&
          text_contains(text, "cancelled"),
          "the conversation cancels");
    CHECK(appliance_admin_credentials_prompt_current(&prompt, &length) ==
              NULL,
          "cancelling zeroes the current password");
    CHECK(bytes_are_zero(prompt.current, sizeof(prompt.current)) &&
          bytes_are_zero(prompt.login, sizeof(prompt.login)) &&
          bytes_are_zero(prompt.password, sizeof(prompt.password)) &&
          bytes_are_zero(prompt.confirm, sizeof(prompt.confirm)),
          "cancelling zeroes the secret bytes");
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "a cancelled conversation can restart");
}

static void test_bounds_and_errors(void)
{
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    char long_line[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 2U];
    char max_login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    char max_password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    size_t text_length = 0U;
    size_t index;

    for (index = 0U; index < sizeof(long_line) - 1U; ++index) {
        long_line[index] = 'a';
    }
    long_line[sizeof(long_line) - 1U] = '\0';
    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX; ++index) {
        max_login[index] = 'l';
    }
    max_login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX] = '\0';
    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX; ++index) {
        max_password[index] = (index % 7U == 0U) ? ' ' : 'p';
    }
    max_password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX] = '\0';

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the bounds fixture starts");
    CHECK(submit(&prompt, long_line, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "longer than") &&
          prompt.current_length == 0U,
          "an over-long line is refused, not truncated");
    CHECK(submit(&prompt, max_login, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "New login"),
          "the current password of the maximum line is accepted");
    CHECK(submit(&prompt, max_login, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "New password"),
          "a 32-character login is accepted");
    CHECK(submit(&prompt, max_password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Repeat new password"),
          "a 128-character password is accepted");
    CHECK(submit(&prompt, max_password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE,
          "the maximum pair submits");

    CHECK(appliance_admin_credentials_prompt_start(
              NULL, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL,
          "a null conversation is refused");
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_NONE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE,
          "an unknown mode is refused");
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, 4U, &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL,
          "a too-small text buffer is refused");
    appliance_admin_credentials_prompt_reset(&prompt);
    CHECK(appliance_admin_credentials_prompt_submit(
              &prompt, "x", 1U, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE &&
          prompt.last_status ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE,
          "a line sent to an idle conversation is refused with BAD_STATE");
    {
        enum appliance_admin_credentials_prompt_action action =
            appliance_admin_credentials_prompt_submit(
                NULL, "x", 1U, text, sizeof(text), &text_length);

        CHECK(action == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE,
              "a null prompt submission is refused");
    }
}

/*
 * The claim is that the terminal text never repeats a secret.  The first proof
 * checked it only after SUBMIT_CHANGE, where the module has just set the text
 * to the empty string, so it was vacuously true.  Here every emitted text -
 * questions and refusals alike - is scanned while the secrets are live.
 */
static void test_secrets_never_reach_the_text(void)
{
    static const char current[] = "the old password";
    static const char password[] = "correct horse battery staple";
    static const char weak[] = "password";
    static const char different[] = "different";
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK &&
          !text_contains(text, current) && !text_contains(text, password),
          "the first question repeats no secret");
    CHECK(submit(&prompt, current, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          !text_contains(text, current) && !text_contains(text, password),
          "the login question repeats no secret");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          !text_contains(text, current) && !text_contains(text, password),
          "the password question repeats no secret");
    CHECK(submit(&prompt, weak, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "weak list") &&
          !text_contains(text, current) && !text_contains(text, password),
          "a weak-password refusal repeats no secret");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          !text_contains(text, password) && !text_contains(text, different),
          "the confirm question repeats no secret");
    CHECK(submit(&prompt, different, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "do not match") &&
          !text_contains(text, password) && !text_contains(text, different),
          "a mismatch refusal repeats no secret");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          !text_contains(text, password),
          "the restarted pair repeats no secret");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE &&
          !text_contains(text, password) && !text_contains(text, current),
          "the terminal action repeats no secret");
    appliance_admin_credentials_prompt_reset(&prompt);

    /* The login conversation scans the same way. */
    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the login scan fixture starts");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          !text_contains(text, password),
          "the login question repeats no secret");
    CHECK(submit(&prompt, password, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN &&
          !text_contains(text, password),
          "the login terminal action repeats no secret");
    appliance_admin_credentials_prompt_reset(&prompt);
}

/* The exact policy edges the contract names: 12 accepted, 33 refused, 1 ok. */
static void test_policy_edges(void)
{
    static const char twelve[] = "twelve chars";
    char login33[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 2U];
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;
    size_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U;
         ++index) {
        login33[index] = 'l';
    }
    login33[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U] = '\0';

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the policy-edge fixture starts");
    CHECK(submit(&prompt, "old", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT,
          "the current password is taken");
    CHECK(submit(&prompt, "a", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "New password"),
          "a one-character login is accepted");
    CHECK(submit(&prompt, twelve, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Repeat new password"),
          "a twelve-character password is accepted");
    CHECK(submit(&prompt, twelve, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE,
          "the minimum pair submits");
    appliance_admin_credentials_prompt_reset(&prompt);

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the 33-character fixture starts");
    CHECK(submit(&prompt, "old", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT,
          "the 33-character fixture takes the current password");
    CHECK(submit(&prompt, login33, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
          text_contains(text, "Login must be") &&
          prompt.step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN,
          "a 33-character login is refused and re-asked");
    appliance_admin_credentials_prompt_reset(&prompt);
}

static void test_names(void)
{
    CHECK(strcmp(appliance_admin_credentials_prompt_mode_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE),
                 "CHANGE") == 0,
          "the change mode has a stable name");
    CHECK(strcmp(appliance_admin_credentials_prompt_step_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM),
                 "CONFIRM") == 0,
          "the confirm step has a stable name");
    CHECK(strcmp(appliance_admin_credentials_prompt_action_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN),
                 "SUBMIT_LOGIN") == 0,
          "the login submission has a stable name");
    CHECK(strcmp(appliance_admin_credentials_prompt_status_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL),
                 "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL") == 0,
          "the buffer refusal has a stable name");
    CHECK(appliance_admin_credentials_prompt_status_string(
              (enum appliance_admin_credentials_prompt_status)999) == NULL,
          "an unknown status has no name");
}

/*
 * ITEM-509 increment 20: the conversation carries its language, the questions
 * and refusals come from the one catalog in it, and a language the catalog
 * cannot answer is the named refusal - never an English question.  The Russian
 * refusal and the repeated question must still fit TEXT_MAX together, which the
 * PROMPT action itself proves.
 */
static void test_russian_conversation(void)
{
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the Russian change conversation starts");
    CHECK(prompt.language == APPLIANCE_LANGUAGE_RU &&
              strcmp(text, "Текущий пароль: ") == 0,
          "the first question is the Russian catalog question");
    CHECK(submit(&prompt, "", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              strcmp(text,
                     "Текущий пароль не может быть пустым. "
                     "Текущий пароль: ") == 0 &&
              text_length < sizeof(text),
          "the Russian refusal precedes the repeated question and fits");
    CHECK(submit(&prompt, "old password", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              strcmp(text, "Новый логин: ") == 0,
          "the next Russian question comes from the catalog");
    CHECK(submit(&prompt, "bad login", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "Логин: 1-32 символа") &&
              text_contains(text, "Новый логин: "),
          "the Russian login refusal repeats the Russian question");
    CHECK(submit(&prompt, "operator", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              strcmp(text, "Новый пароль: ") == 0,
          "the Russian password question comes from the catalog");
    CHECK(submit(&prompt, "short", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text,
                            "Пароль: 12-128 печатных символов") &&
              text_contains(text, "Новый пароль: "),
          "the Russian password refusal repeats the Russian question");
    CHECK(submit(&prompt, "correct horse battery staple", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              strcmp(text, "Повторите новый пароль: ") == 0,
          "the Russian confirmation question comes from the catalog");
    CHECK(submit(&prompt, "different password entirely", text,
                 sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              text_contains(text, "Пароли не совпадают.") &&
              text_contains(text, "Новый пароль: "),
          "the Russian mismatch refusal repeats the Russian question");
    CHECK(submit(&prompt, "correct horse battery staple", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT &&
              strcmp(text, "Повторите новый пароль: ") == 0,
          "the restarted Russian pair asks the confirmation again");
    CHECK(submit(&prompt, "correct horse battery staple", text, sizeof(text),
                 &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE,
          "the Russian conversation submits like the English one");
    appliance_admin_credentials_prompt_reset(&prompt);

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN, text,
              sizeof(text), &text_length, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK &&
              strcmp(text, "Логин: ") == 0,
          "the Russian login conversation starts");
    CHECK(appliance_admin_credentials_prompt_cancel(
              &prompt, text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED &&
              strcmp(text, "Ввод учётных данных отменён.") == 0,
          "the Russian cancellation notice comes from the catalog");
}

static void test_missing_translation_is_named(void)
{
    struct appliance_admin_credentials_prompt prompt;
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length,
              (enum appliance_language)7) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION &&
              strcmp(text, "CREDENTIAL TRANSLATION MISSING") == 0 &&
              prompt.last_status ==
                  APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION,
          "an unknown language starts no conversation and names the refusal");
    appliance_admin_credentials_prompt_reset(&prompt);

    memset(&prompt, 0, sizeof(prompt));
    CHECK(appliance_admin_credentials_prompt_start(
              &prompt, APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE,
              text, sizeof(text), &text_length, APPLIANCE_LANGUAGE_DEFAULT) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK,
          "the English conversation starts before the language is broken");
    prompt.language = (enum appliance_language)7;
    CHECK(submit(&prompt, "old password", text, sizeof(text), &text_length) ==
              APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE &&
              strcmp(text, "CREDENTIAL TRANSLATION MISSING") == 0 &&
              prompt.last_status ==
                  APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION,
          "a language the catalog cannot answer later is named, not ignored");
    CHECK(strcmp(appliance_admin_credentials_prompt_status_string(
                     APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION),
                 "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION") ==
              0,
          "the missing-translation status has a stable name");
}

int main(void)
{
    test_change_conversation();
    test_set_conversation();
    test_login_conversation_and_cancel();
    test_bounds_and_errors();
    test_secrets_never_reach_the_text();
    test_policy_edges();
    test_names();
    test_russian_conversation();
    test_missing_translation_is_named();
    if (failures != 0) {
        fprintf(stderr,
                "appliance admin credentials prompt host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance admin credentials prompt host test: ALL OK\n");
    return 0;
}

#endif
