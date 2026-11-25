#include "appliance_admin_credentials_prompt.h"

#include <string.h>

/*
 * ITEM-506 credential questions.  See the header for the contract; this file
 * owns the order of the questions, the refusals and the zeroization.
 *
 * ITEM-509 increment 20: the words live in the one catalog, not here.  The
 * conversation carries its language and every question and refusal is looked
 * up in it; a key the catalog cannot answer is the named refusal
 * `CREDENTIAL TRANSLATION MISSING` and status MISSING_TRANSLATION, so no
 * surface ever prints an English sentence into a Russian conversation.
 */
#define APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TEXT \
    "CREDENTIAL TRANSLATION MISSING"

static void appliance_admin_credentials_prompt_zero(
    void *destination,
    size_t length)
{
    volatile uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_admin_credentials_prompt_append(
    char *buffer,
    size_t buffer_size,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return true;
    }
    while (text[index] != '\0') {
        if (*offset + 1U >= buffer_size) {
            return false;
        }
        buffer[*offset] = text[index];
        ++(*offset);
        ++index;
    }
    buffer[*offset] = '\0';
    return true;
}

static enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_write(
    struct appliance_admin_credentials_prompt *prompt,
    const char *reason,
    const char *question,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    size_t offset = 0U;

    if (text_size != 0U) {
        text_out[0] = '\0';
    }
    *text_length_out = 0U;
    if (text_size == 0U ||
        !appliance_admin_credentials_prompt_append(
            text_out, text_size, &offset, reason) ||
        !appliance_admin_credentials_prompt_append(
            text_out, text_size, &offset, question)) {
        prompt->last_status =
            APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL;
        if (text_size != 0U) {
            text_out[0] = '\0';
        }
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
    *text_length_out = offset;
    return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT;
}

/*
 * ITEM-509 increment 20: the catalog could not answer in the conversation's
 * language.  The named refusal is the only text written and the status records
 * why; nothing silently falls back to English.
 */
static enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_missing(
    struct appliance_admin_credentials_prompt *prompt,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    const char *missing = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TEXT;
    const size_t length = strlen(missing);

    prompt->last_status =
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION;
    *text_length_out = 0U;
    if (text_size == 0U || length + 1U > text_size) {
        if (text_size != 0U) {
            text_out[0] = '\0';
        }
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
    memcpy(text_out, missing, length + 1U);
    *text_length_out = length;
    return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
}

/*
 * ITEM-509 increment 20: the question each step asks, as a catalog key looked
 * up in the conversation's language.  NULL means the catalog cannot answer -
 * never an English fallback.
 */
static const char *appliance_admin_credentials_prompt_question(
    enum appliance_admin_credentials_prompt_step step,
    enum appliance_language language)
{
    const char *key;

    switch (step) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:
        key = "credential.prompt.current";
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN:
        key = "credential.prompt.new_login";
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:
        key = "credential.prompt.new_password";
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:
        key = "credential.prompt.confirm";
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME:
        key = "credential.prompt.login";
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:
        key = "credential.prompt.password";
        break;
    default:
        return NULL;
    }
    return appliance_language_string(language, key);
}

/*
 * Copy one entered line into its buffer.  A line longer than the model's
 * maximum is refused, never cut; the caller sees the refusal and the repeated
 * question.
 */
static bool appliance_admin_credentials_prompt_store(
    char *destination,
    size_t destination_size,
    size_t *length_out,
    const char *line,
    size_t line_length)
{
    if (line == NULL && line_length != 0U) {
        return false;
    }
    if (line_length > APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX ||
        line_length + 1U > destination_size) {
        return false;
    }
    appliance_admin_credentials_prompt_zero(destination, destination_size);
    if (line_length != 0U) {
        memcpy(destination, line, line_length);
    }
    destination[line_length] = '\0';
    *length_out = line_length;
    return true;
}

enum appliance_admin_credentials_prompt_status
appliance_admin_credentials_prompt_start(
    struct appliance_admin_credentials_prompt *prompt,
    enum appliance_admin_credentials_prompt_mode mode,
    char *text_out,
    size_t text_size,
    size_t *text_length_out,
    enum appliance_language language)
{
    const char *question;

    if (prompt == NULL || text_out == NULL || text_length_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL;
    }
    if (prompt->version == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION &&
        prompt->step != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE) {
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE;
    }
    appliance_admin_credentials_prompt_zero(prompt, sizeof(*prompt));
    prompt->version = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION;
    prompt->language = language;
    prompt->mode = mode;
    switch (mode) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE:
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT;
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN:
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME;
        break;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET:
        /*
         * ITEM-506 increment 4: a fresh install has no current password to
         * prove, so the conversation starts at the new login and the caller
         * commits the pair directly.
         */
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN;
        break;
    default:
        prompt->mode = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_NONE;
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE;
        prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE;
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE;
    }
    prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK;
    question = appliance_admin_credentials_prompt_question(
        prompt->step, prompt->language);
    if (question == NULL) {
        (void)appliance_admin_credentials_prompt_missing(
            prompt, text_out, text_size, text_length_out);
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION;
    }
    if (appliance_admin_credentials_prompt_write(
            prompt, NULL, question, text_out, text_size, text_length_out) !=
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT) {
        return prompt->last_status;
    }
    return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK;
}

enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_submit(
    struct appliance_admin_credentials_prompt *prompt,
    const char *line,
    size_t line_length,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    const char *too_long;
    const char *current_empty;
    const char *login_rule;
    const char *password_rule;
    const char *password_weak;
    const char *password_mismatch;
    const char *password_empty;
    const char *question_current;
    const char *question_new_login;
    const char *question_new_password;
    const char *question_confirm;
    const char *question_login;
    const char *question_password;

    if (prompt == NULL || text_out == NULL || text_length_out == NULL ||
        (line == NULL && line_length != 0U)) {
        if (prompt != NULL) {
            prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL;
        }
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
    if (prompt->version != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION ||
        prompt->step == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE) {
        prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE;
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
    prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK;
    /*
     * ITEM-509 increment 20: every word this call can print is looked up before
     * the first byte is written; one key the catalog cannot answer is the named
     * refusal, never a question half in English.
     */
    too_long = appliance_language_string(
        prompt->language, "credential.refusal.too_long");
    current_empty = appliance_language_string(
        prompt->language, "credential.refusal.current_empty");
    login_rule = appliance_language_string(
        prompt->language, "credential.refusal.login_policy");
    password_rule = appliance_language_string(
        prompt->language, "credential.refusal.password_policy");
    password_weak = appliance_language_string(
        prompt->language, "credential.refusal.weak_password");
    password_mismatch = appliance_language_string(
        prompt->language, "credential.refusal.mismatch");
    password_empty = appliance_language_string(
        prompt->language, "credential.refusal.password_empty");
    question_current = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT, prompt->language);
    question_new_login = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN, prompt->language);
    question_new_password = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD,
        prompt->language);
    question_confirm = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM, prompt->language);
    question_login = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME, prompt->language);
    question_password = appliance_admin_credentials_prompt_question(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD,
        prompt->language);
    if (too_long == NULL || current_empty == NULL || login_rule == NULL ||
        password_rule == NULL || password_weak == NULL ||
        password_mismatch == NULL || password_empty == NULL ||
        question_current == NULL || question_new_login == NULL ||
        question_new_password == NULL || question_confirm == NULL ||
        question_login == NULL || question_password == NULL) {
        return appliance_admin_credentials_prompt_missing(
            prompt, text_out, text_size, text_length_out);
    }
    if (line_length > APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX) {
        return appliance_admin_credentials_prompt_write(
            prompt, too_long,
            appliance_admin_credentials_prompt_question(prompt->step,
                                                        prompt->language),
            text_out, text_size, text_length_out);
    }

    switch (prompt->step) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:
        if (line_length == 0U) {
            return appliance_admin_credentials_prompt_write(
                prompt, current_empty,
                question_current,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->current, sizeof(prompt->current),
                &prompt->current_length, line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_current,
                text_out, text_size, text_length_out);
        }
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN;
        return appliance_admin_credentials_prompt_write(
            prompt, NULL,
            question_new_login,
            text_out, text_size, text_length_out);

    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN:
        if (appliance_admin_credential_validate_login(line, line_length) !=
            APPLIANCE_ADMIN_CREDENTIAL_OK) {
            return appliance_admin_credentials_prompt_write(
                prompt, login_rule,
                question_new_login,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->login, sizeof(prompt->login), &prompt->login_length,
                line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_new_login,
                text_out, text_size, text_length_out);
        }
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD;
        return appliance_admin_credentials_prompt_write(
            prompt, NULL,
            question_new_password,
            text_out, text_size, text_length_out);

    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:
        switch (appliance_admin_credential_validate_password(
            line, line_length, prompt->login, prompt->login_length)) {
        case APPLIANCE_ADMIN_CREDENTIAL_OK:
            break;
        case APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD:
            return appliance_admin_credentials_prompt_write(
                prompt, password_weak,
                question_new_password,
                text_out, text_size, text_length_out);
        default:
            return appliance_admin_credentials_prompt_write(
                prompt, password_rule,
                question_new_password,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->password, sizeof(prompt->password),
                &prompt->password_length, line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_new_password,
                text_out, text_size, text_length_out);
        }
        appliance_admin_credentials_prompt_zero(
            prompt->confirm, sizeof(prompt->confirm));
        prompt->confirm_length = 0U;
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM;
        return appliance_admin_credentials_prompt_write(
            prompt, NULL,
            question_confirm,
            text_out, text_size, text_length_out);

    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:
        if (line_length != prompt->password_length ||
            (line_length != 0U &&
             memcmp(line, prompt->password, line_length) != 0)) {
            /*
             * A mismatch restarts the pair: the operator types the new
             * password twice again, so a typo can never be committed.
             */
            appliance_admin_credentials_prompt_zero(
                prompt->password, sizeof(prompt->password));
            prompt->password_length = 0U;
            prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD;
            return appliance_admin_credentials_prompt_write(
                prompt, password_mismatch,
                question_new_password,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->confirm, sizeof(prompt->confirm),
                &prompt->confirm_length, line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_confirm,
                text_out, text_size, text_length_out);
        }
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE;
        *text_length_out = 0U;
        if (text_size != 0U) {
            text_out[0] = '\0';
        }
        return prompt->mode == APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET
            ? APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET
            : APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE;

    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME:
        if (appliance_admin_credential_validate_login(line, line_length) !=
            APPLIANCE_ADMIN_CREDENTIAL_OK) {
            return appliance_admin_credentials_prompt_write(
                prompt, login_rule,
                question_login,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->login, sizeof(prompt->login), &prompt->login_length,
                line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_login,
                text_out, text_size, text_length_out);
        }
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD;
        return appliance_admin_credentials_prompt_write(
            prompt, NULL,
            question_password,
            text_out, text_size, text_length_out);

    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:
        if (line_length == 0U || line_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
            return appliance_admin_credentials_prompt_write(
                prompt, password_empty,
                question_password,
                text_out, text_size, text_length_out);
        }
        if (!appliance_admin_credentials_prompt_store(
                prompt->password, sizeof(prompt->password),
                &prompt->password_length, line, line_length)) {
            return appliance_admin_credentials_prompt_write(
                prompt, too_long,
                question_password,
                text_out, text_size, text_length_out);
        }
        prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE;
        *text_length_out = 0U;
        if (text_size != 0U) {
            text_out[0] = '\0';
        }
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN;

    default:
        prompt->last_status = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE;
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
}

enum appliance_admin_credentials_prompt_action
appliance_admin_credentials_prompt_cancel(
    struct appliance_admin_credentials_prompt *prompt,
    char *text_out,
    size_t text_size,
    size_t *text_length_out)
{
    const char *cancelled;
    size_t length;

    if (prompt == NULL || text_out == NULL || text_length_out == NULL) {
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE;
    }
    /*
     * ITEM-509 increment 20: the notice comes from the catalog in the
     * conversation's language; a missing key is the named refusal.  The
     * language is read before the zeroing, which also clears it.
     */
    cancelled = appliance_language_string(prompt->language,
                                          "credential.cancelled");
    if (cancelled == NULL) {
        cancelled = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TEXT;
    }
    appliance_admin_credentials_prompt_zero(prompt, sizeof(*prompt));
    *text_length_out = 0U;
    length = strlen(cancelled);
    if (text_size == 0U || length + 1U > text_size) {
        if (text_size != 0U) {
            text_out[0] = '\0';
        }
        return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED;
    }
    memcpy(text_out, cancelled, length + 1U);
    *text_length_out = length;
    return APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED;
}

void appliance_admin_credentials_prompt_reset(
    struct appliance_admin_credentials_prompt *prompt)
{
    if (prompt == NULL) {
        return;
    }
    appliance_admin_credentials_prompt_zero(prompt, sizeof(*prompt));
}

const char *appliance_admin_credentials_prompt_login(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out)
{
    if (prompt == NULL || length_out == NULL ||
        prompt->version != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION ||
        prompt->login_length == 0U) {
        return NULL;
    }
    *length_out = prompt->login_length;
    return prompt->login;
}

const char *appliance_admin_credentials_prompt_password(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out)
{
    if (prompt == NULL || length_out == NULL ||
        prompt->version != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION ||
        prompt->password_length == 0U) {
        return NULL;
    }
    *length_out = prompt->password_length;
    return prompt->password;
}

const char *appliance_admin_credentials_prompt_current(
    const struct appliance_admin_credentials_prompt *prompt,
    size_t *length_out)
{
    if (prompt == NULL || length_out == NULL ||
        prompt->version != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_VERSION ||
        prompt->current_length == 0U) {
        return NULL;
    }
    *length_out = prompt->current_length;
    return prompt->current;
}

const char *appliance_admin_credentials_prompt_mode_string(
    enum appliance_admin_credentials_prompt_mode mode)
{
    switch (mode) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_NONE:
        return "NONE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE:
        return "CHANGE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN:
        return "LOGIN";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET:
        return "SET";
    default:
        return NULL;
    }
}

const char *appliance_admin_credentials_prompt_step_string(
    enum appliance_admin_credentials_prompt_step step)
{
    switch (step) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_IDLE:
        return "IDLE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:
        return "CURRENT";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN:
        return "NEW_LOGIN";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:
        return "NEW_PASSWORD";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:
        return "CONFIRM";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME:
        return "LOGIN_NAME";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:
        return "LOGIN_PASSWORD";
    default:
        return NULL;
    }
}

const char *appliance_admin_credentials_prompt_action_string(
    enum appliance_admin_credentials_prompt_action action)
{
    switch (action) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_NONE:
        return "NONE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT:
        return "PROMPT";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE:
        return "SUBMIT_CHANGE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN:
        return "SUBMIT_LOGIN";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED:
        return "CANCELLED";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET:
        return "SUBMIT_SET";
    default:
        return NULL;
    }
}

const char *appliance_admin_credentials_prompt_status_string(
    enum appliance_admin_credentials_prompt_status status)
{
    switch (status) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK:
        return "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL:
        return "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_NULL";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE:
        return "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BAD_STATE";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL:
        return "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_BUFFER_TOO_SMALL";
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION:
        return "APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION";
    default:
        return NULL;
    }
}
