#include "appliance_panel_login.h"

#include <string.h>

/*
 * ITEM-445: the byte strings a browser signs in with, and nothing else.
 *
 * The module is deliberately small and total: every function either produces
 * the exact bytes the caller asked for or refuses with a status, and no path
 * truncates a value, drops an attribute or guesses at a malformed cookie.  The
 * password buffer is zeroed by the caller through the form-zero helper, so the
 * secret lives no longer than the request that carried it.
 */

static const char appliance_panel_login_hex[] = "0123456789abcdef";

static int appliance_panel_login_hex_value(uint8_t digit)
{
    if (digit >= (uint8_t)'0' && digit <= (uint8_t)'9') {
        return (int)(digit - (uint8_t)'0');
    }
    if (digit >= (uint8_t)'a' && digit <= (uint8_t)'f') {
        return (int)(digit - (uint8_t)'a') + 10;
    }
    if (digit >= (uint8_t)'A' && digit <= (uint8_t)'F') {
        return (int)(digit - (uint8_t)'A') + 10;
    }
    return -1;
}

static bool appliance_panel_login_lower_equal(uint8_t left, char right)
{
    if (left >= (uint8_t)'A' && left <= (uint8_t)'Z') {
        left = (uint8_t)(left - (uint8_t)'A' + (uint8_t)'a');
    }
    return left == (uint8_t)right;
}

static bool appliance_panel_login_prefix_equal(
    const uint8_t *value,
    uint32_t value_length,
    const char *prefix)
{
    uint32_t index;

    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (index >= value_length ||
            !appliance_panel_login_lower_equal(value[index], prefix[index])) {
            return false;
        }
    }
    return true;
}

static bool appliance_panel_login_contains(
    const uint8_t *haystack,
    uint32_t haystack_length,
    const char *needle)
{
    uint32_t needle_length = 0U;
    uint32_t index;

    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    if (needle_length == 0U || needle_length > haystack_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= haystack_length; ++index) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool appliance_panel_login_field_name(
    const uint8_t *name,
    uint32_t name_length,
    const char *expected,
    uint32_t expected_length)
{
    return name_length == expected_length &&
           memcmp(name, expected, expected_length) == 0;
}

/*
 * Decode one url-encoded value into `output`.  `+` is a space, `%XX` is one
 * byte.  A truncated or non-hex escape, or a decoded NUL or line break, is
 * refused: the first cannot be decoded honestly and the second has no place in
 * a credential that a record or a header later quotes.
 */
static enum appliance_panel_login_status appliance_panel_login_decode(
    const uint8_t *input,
    uint32_t input_length,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length_out)
{
    uint32_t index = 0U;
    uint32_t written = 0U;

    while (index < input_length) {
        uint8_t byte = input[index];

        if (byte == (uint8_t)'+') {
            byte = (uint8_t)' ';
            ++index;
        } else if (byte == (uint8_t)'%') {
            int high;
            int low;

            if (index + 2U >= input_length) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            high = appliance_panel_login_hex_value(input[index + 1U]);
            low = appliance_panel_login_hex_value(input[index + 2U]);
            if (high < 0 || low < 0) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            byte = (uint8_t)((high << 4) | low);
            index += 3U;
        } else {
            ++index;
        }
        if (byte == 0U || byte == (uint8_t)'\r' || byte == (uint8_t)'\n') {
            return APPLIANCE_PANEL_LOGIN_MALFORMED;
        }
        if (written >= output_capacity) {
            return APPLIANCE_PANEL_LOGIN_TOO_LARGE;
        }
        output[written] = byte;
        ++written;
    }
    *output_length_out = written;
    return APPLIANCE_PANEL_LOGIN_OK;
}

enum appliance_panel_login_status appliance_panel_login_form_parse(
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_login_form *form_out)
{
    uint32_t index = 0U;
    bool user_seen = false;
    bool password_seen = false;

    if (form_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    memset(form_out, 0, sizeof(*form_out));
    if ((body == NULL && body_length != 0U) ||
        (content_type == NULL && content_type_length != 0U)) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    if (!appliance_panel_login_prefix_equal(
            content_type,
            content_type_length,
            APPLIANCE_PANEL_LOGIN_CONTENT_TYPE)) {
        return APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE;
    }
    if (content_type_length > APPLIANCE_PANEL_LOGIN_CONTENT_TYPE_LENGTH &&
        content_type[APPLIANCE_PANEL_LOGIN_CONTENT_TYPE_LENGTH] !=
            (uint8_t)';') {
        return APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE;
    }
    if (body_length > APPLIANCE_PANEL_LOGIN_BODY_MAX) {
        return APPLIANCE_PANEL_LOGIN_TOO_LARGE;
    }

    while (index < body_length) {
        uint32_t field_end = index;
        uint32_t separator;
        const uint8_t *name;
        uint32_t name_length;
        const uint8_t *value;
        uint32_t value_length;
        enum appliance_panel_login_status status;

        while (field_end < body_length && body[field_end] != (uint8_t)'&') {
            ++field_end;
        }
        separator = index;
        while (separator < field_end && body[separator] != (uint8_t)'=') {
            ++separator;
        }
        if (separator >= field_end) {
            return APPLIANCE_PANEL_LOGIN_MALFORMED;
        }
        name = body + index;
        name_length = separator - index;
        value = body + separator + 1U;
        value_length = field_end - separator - 1U;

        if (appliance_panel_login_field_name(
                name, name_length, "username", 8U)) {
            if (user_seen) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            status = appliance_panel_login_decode(
                value,
                value_length,
                form_out->user,
                APPLIANCE_PANEL_LOGIN_FIELD_MAX,
                &form_out->user_length);
            if (status != APPLIANCE_PANEL_LOGIN_OK) {
                return status;
            }
            form_out->user[form_out->user_length] = 0U;
            form_out->user_present = true;
            user_seen = true;
        } else if (appliance_panel_login_field_name(
                       name, name_length, "password", 8U)) {
            if (password_seen) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            status = appliance_panel_login_decode(
                value,
                value_length,
                form_out->password,
                APPLIANCE_PANEL_LOGIN_FIELD_MAX,
                &form_out->password_length);
            if (status != APPLIANCE_PANEL_LOGIN_OK) {
                return status;
            }
            form_out->password[form_out->password_length] = 0U;
            form_out->password_present = true;
            password_seen = true;
        } else if (name_length == 0U) {
            return APPLIANCE_PANEL_LOGIN_MALFORMED;
        }
        index = field_end + 1U;
    }

    if (!form_out->user_present || !form_out->password_present) {
        appliance_panel_login_form_zero(form_out);
        return APPLIANCE_PANEL_LOGIN_MISSING_FIELD;
    }
    return APPLIANCE_PANEL_LOGIN_OK;
}

void appliance_panel_login_form_zero(struct appliance_panel_login_form *form)
{
    if (form == NULL) {
        return;
    }
    memset(form, 0, sizeof(*form));
}

/*
 * ITEM-506 increment 6: the credential-change form.  The walk is the sign-in
 * walk with four declared names instead of two, and it is deliberately
 * stricter: a name the form does not declare is refused, because a stray
 * parameter on a password form is either a mistake or an attempt to smuggle a
 * field past the executor.  The decoder and its refusals are shared, so a
 * malformed escape, a decoded NUL or line break, a repeated field and a value
 * that does not fit its policy bound are all refused the same way.
 */
enum appliance_panel_login_status appliance_panel_account_form_parse(
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_account_form *form_out)
{
    uint32_t index = 0U;

    if (form_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    memset(form_out, 0, sizeof(*form_out));
    if (body == NULL && body_length != 0U) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    if (body_length > APPLIANCE_PANEL_ACCOUNT_BODY_MAX) {
        return APPLIANCE_PANEL_LOGIN_TOO_LARGE;
    }

    while (index < body_length) {
        uint32_t field_end = index;
        uint32_t separator;
        const uint8_t *name;
        uint32_t name_length;
        const uint8_t *value;
        uint32_t value_length;
        enum appliance_panel_login_status status;
        uint8_t *destination;
        uint32_t capacity;
        uint32_t *length_out;
        bool *present;

        while (field_end < body_length && body[field_end] != (uint8_t)'&') {
            ++field_end;
        }
        separator = index;
        while (separator < field_end && body[separator] != (uint8_t)'=') {
            ++separator;
        }
        if (separator >= field_end) {
            return APPLIANCE_PANEL_LOGIN_MALFORMED;
        }
        name = body + index;
        name_length = separator - index;
        value = body + separator + 1U;
        value_length = field_end - separator - 1U;

        if (appliance_panel_login_field_name(
                name, name_length, "current", 7U)) {
            if (form_out->current_present) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            destination = form_out->current;
            capacity = APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX;
            length_out = &form_out->current_length;
            present = &form_out->current_present;
        } else if (appliance_panel_login_field_name(
                       name, name_length, "login", 5U)) {
            if (form_out->login_present) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            destination = form_out->login;
            capacity = APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX;
            length_out = &form_out->login_length;
            present = &form_out->login_present;
        } else if (appliance_panel_login_field_name(
                       name, name_length, "password", 8U)) {
            if (form_out->password_present) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            destination = form_out->password;
            capacity = APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX;
            length_out = &form_out->password_length;
            present = &form_out->password_present;
        } else if (appliance_panel_login_field_name(
                       name, name_length, "repeat", 6U)) {
            if (form_out->repeat_present) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            destination = form_out->repeat;
            capacity = APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX;
            length_out = &form_out->repeat_length;
            present = &form_out->repeat_present;
        } else {
            return APPLIANCE_PANEL_LOGIN_MALFORMED;
        }
        status = appliance_panel_login_decode(
            value, value_length, destination, capacity, length_out);
        if (status != APPLIANCE_PANEL_LOGIN_OK) {
            appliance_panel_account_form_zero(form_out);
            return status;
        }
        destination[*length_out] = 0U;
        *present = true;
        index = field_end + 1U;
    }

    if (!form_out->current_present || !form_out->login_present ||
        !form_out->password_present || !form_out->repeat_present) {
        appliance_panel_account_form_zero(form_out);
        return APPLIANCE_PANEL_LOGIN_MISSING_FIELD;
    }
    return APPLIANCE_PANEL_LOGIN_OK;
}

void appliance_panel_account_form_zero(
    struct appliance_panel_account_form *form)
{
    if (form == NULL) {
        return;
    }
    memset(form, 0, sizeof(*form));
}

enum appliance_panel_login_status appliance_panel_login_cookie_token(
    const uint8_t *cookie_header,
    uint32_t cookie_header_length,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    bool *present_out)
{
    uint32_t index = 0U;
    bool found = false;

    if (present_out == NULL || token_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    *present_out = false;
    memset(token_out, 0, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    if (cookie_header == NULL) {
        return cookie_header_length == 0U ? APPLIANCE_PANEL_LOGIN_OK
                                          : APPLIANCE_PANEL_LOGIN_NULL;
    }
    /* Accept the header value with or without its `Cookie:` name. */
    if (appliance_panel_login_prefix_equal(
            cookie_header, cookie_header_length, "cookie:")) {
        index = 7U;
    }

    while (index < cookie_header_length) {
        uint32_t pair_end = index;
        uint32_t separator;
        const uint8_t *name;
        uint32_t name_length;
        const uint8_t *value;
        uint32_t value_length;
        uint32_t digit;

        while (pair_end < cookie_header_length &&
               cookie_header[pair_end] != (uint8_t)';') {
            ++pair_end;
        }
        while (index < pair_end &&
               (cookie_header[index] == (uint8_t)' ' ||
                cookie_header[index] == (uint8_t)'\t')) {
            ++index;
        }
        separator = index;
        while (separator < pair_end &&
               cookie_header[separator] != (uint8_t)'=') {
            ++separator;
        }
        while (separator > index &&
               (cookie_header[separator - 1U] == (uint8_t)' ' ||
                cookie_header[separator - 1U] == (uint8_t)'\t')) {
            --separator;
        }
        name = cookie_header + index;
        name_length = separator - index;
        if (separator >= pair_end) {
            /* A pair with no `=` is not a cookie pair at all. */
            value = cookie_header + pair_end;
            value_length = 0U;
        } else {
            value = cookie_header + separator + 1U;
            value_length = pair_end - separator - 1U;
        }
        while (value_length > 0U &&
               (value[value_length - 1U] == (uint8_t)' ' ||
                value[value_length - 1U] == (uint8_t)'\t')) {
            --value_length;
        }

        if (appliance_panel_login_field_name(
                name,
                name_length,
                APPLIANCE_PANEL_LOGIN_COOKIE_NAME,
                APPLIANCE_PANEL_LOGIN_COOKIE_NAME_LENGTH)) {
            if (found) {
                /*
                 * Two session cookies in one header is an attempt to make the
                 * reader pick one; refuse instead of guessing.
                 */
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            if (separator >= pair_end ||
                value_length != APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH) {
                return APPLIANCE_PANEL_LOGIN_MALFORMED;
            }
            for (digit = 0U;
                 digit < APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH;
                 digit += 2U) {
                int high = appliance_panel_login_hex_value(value[digit]);
                int low = appliance_panel_login_hex_value(value[digit + 1U]);

                if (high < 0 || low < 0) {
                    memset(token_out, 0, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
                    return APPLIANCE_PANEL_LOGIN_MALFORMED;
                }
                token_out[digit / 2U] = (uint8_t)((high << 4) | low);
            }
            found = true;
        }
        index = pair_end + 1U;
    }

    if (!found) {
        return APPLIANCE_PANEL_LOGIN_OK;
    }
    *present_out = true;
    return APPLIANCE_PANEL_LOGIN_OK;
}

static enum appliance_panel_login_status appliance_panel_login_cookie_write(
    const uint8_t *hex,
    uint32_t hex_length,
    uint32_t max_age_seconds,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    static const char suffix[] =
        "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=";
    uint8_t age[10];
    uint32_t age_length = 0U;
    uint32_t name_length = APPLIANCE_PANEL_LOGIN_COOKIE_NAME_LENGTH;
    uint32_t suffix_length = (uint32_t)(sizeof(suffix) - 1U);
    uint32_t required;
    uint32_t written = 0U;

    if (output == NULL || written_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    *written_out = 0U;
    do {
        age[age_length] = (uint8_t)('0' + (max_age_seconds % 10U));
        ++age_length;
        max_age_seconds /= 10U;
    } while (max_age_seconds != 0U && age_length < sizeof(age));
    if (max_age_seconds != 0U) {
        /* A lifetime this module cannot print must not be printed wrong. */
        return APPLIANCE_PANEL_LOGIN_TOO_LARGE;
    }

    required = name_length + 1U + hex_length + suffix_length + age_length;
    if (required > output_capacity) {
        return APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL;
    }
    memcpy(output + written,
           APPLIANCE_PANEL_LOGIN_COOKIE_NAME,
           name_length);
    written += name_length;
    output[written] = (uint8_t)'=';
    ++written;
    if (hex_length != 0U) {
        memcpy(output + written, hex, hex_length);
        written += hex_length;
    }
    memcpy(output + written, suffix, suffix_length);
    written += suffix_length;
    while (age_length > 0U) {
        --age_length;
        output[written] = age[age_length];
        ++written;
    }
    *written_out = written;
    return APPLIANCE_PANEL_LOGIN_OK;
}

enum appliance_panel_login_status appliance_panel_login_cookie_set(
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t max_age_seconds,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    uint8_t hex[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH];
    uint32_t index;

    if (token == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        hex[index * 2U] =
            (uint8_t)appliance_panel_login_hex[token[index] >> 4];
        hex[index * 2U + 1U] =
            (uint8_t)appliance_panel_login_hex[token[index] & 0x0fU];
    }
    return appliance_panel_login_cookie_write(
        hex,
        APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH,
        max_age_seconds,
        output,
        output_capacity,
        written_out);
}

enum appliance_panel_login_status appliance_panel_login_cookie_clear(
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    return appliance_panel_login_cookie_write(
        NULL, 0U, 0U, output, output_capacity, written_out);
}

/*
 * ITEM-513: find one named cookie pair in a `Cookie` header.  The walk is the
 * session reader's walk - optional header name, `;` separated pairs, a pair
 * with no `=` is not a cookie pair at all, surrounding spaces and tabs are
 * trimmed - so the language cookie cannot be read by weaker rules than the
 * session cookie.  A second pair with the same name sets `duplicate_out`
 * instead of overwriting the first: the caller refuses rather than guessing.
 */
static enum appliance_panel_login_status appliance_panel_login_cookie_find(
    const uint8_t *cookie_header,
    uint32_t cookie_header_length,
    const char *name,
    uint32_t name_length,
    const uint8_t **value_out,
    uint32_t *value_length_out,
    bool *duplicate_out)
{
    uint32_t index = 0U;
    bool found = false;

    if (value_out == NULL || value_length_out == NULL ||
        duplicate_out == NULL || name == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    *value_out = NULL;
    *value_length_out = 0U;
    *duplicate_out = false;
    if (cookie_header == NULL) {
        return cookie_header_length == 0U ? APPLIANCE_PANEL_LOGIN_OK
                                          : APPLIANCE_PANEL_LOGIN_NULL;
    }
    /* Accept the header value with or without its `Cookie:` name. */
    if (appliance_panel_login_prefix_equal(
            cookie_header, cookie_header_length, "cookie:")) {
        index = 7U;
    }
    while (index < cookie_header_length) {
        uint32_t pair_end = index;
        uint32_t separator;
        const uint8_t *pair_name;
        uint32_t pair_name_length;
        const uint8_t *value;
        uint32_t value_length;

        while (pair_end < cookie_header_length &&
               cookie_header[pair_end] != (uint8_t)';') {
            ++pair_end;
        }
        while (index < pair_end &&
               (cookie_header[index] == (uint8_t)' ' ||
                cookie_header[index] == (uint8_t)'\t')) {
            ++index;
        }
        separator = index;
        while (separator < pair_end &&
               cookie_header[separator] != (uint8_t)'=') {
            ++separator;
        }
        pair_name = cookie_header + index;
        pair_name_length = separator - index;
        if (separator >= pair_end) {
            value = cookie_header + pair_end;
            value_length = 0U;
        } else {
            value = cookie_header + separator + 1U;
            value_length = pair_end - separator - 1U;
        }
        while (value_length > 0U &&
               (value[value_length - 1U] == (uint8_t)' ' ||
                value[value_length - 1U] == (uint8_t)'\t')) {
            --value_length;
        }
        if (appliance_panel_login_field_name(
                pair_name, pair_name_length, name, name_length)) {
            if (found) {
                *duplicate_out = true;
                return APPLIANCE_PANEL_LOGIN_OK;
            }
            *value_out = value;
            *value_length_out = value_length;
            found = true;
        }
        index = pair_end + 1U;
    }
    return APPLIANCE_PANEL_LOGIN_OK;
}

/*
 * ITEM-513: the one writer of the language cookie.  The value is the code the
 * catalog knows, never a caller-supplied byte string: `set` refuses an enum
 * that has no code rather than printing one.  The attributes are the session
 * cookie's protective set; the language cookie carries no `Max-Age`, so it
 * lives for the browser session exactly as the preference does, and the clear
 * path is the same value with `Max-Age=0` for a browser that must drop a value
 * the reader refused.
 */
static enum appliance_panel_login_status
appliance_panel_login_language_cookie_write(
    const char *code,
    bool clear,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    static const char suffix[] =
        "; HttpOnly; Secure; SameSite=Strict; Path=/";
    static const char clear_suffix[] = "; Max-Age=0";
    uint32_t code_length = 0U;
    uint32_t suffix_length = (uint32_t)(sizeof(suffix) - 1U);
    uint32_t clear_length = clear ? (uint32_t)(sizeof(clear_suffix) - 1U) : 0U;
    uint32_t required;
    uint32_t written = 0U;

    if (output == NULL || written_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    *written_out = 0U;
    if (code != NULL) {
        while (code[code_length] != '\0') {
            ++code_length;
        }
    }
    required = APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME_LENGTH + 1U +
               code_length + suffix_length + clear_length;
    if (required > output_capacity) {
        return APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL;
    }
    memcpy(output + written,
           APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME,
           APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME_LENGTH);
    written += APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME_LENGTH;
    output[written] = (uint8_t)'=';
    ++written;
    if (code_length != 0U) {
        memcpy(output + written, code, code_length);
        written += code_length;
    }
    memcpy(output + written, suffix, suffix_length);
    written += suffix_length;
    if (clear) {
        memcpy(output + written, clear_suffix, clear_length);
        written += clear_length;
    }
    *written_out = written;
    return APPLIANCE_PANEL_LOGIN_OK;
}

enum appliance_panel_login_status appliance_panel_login_language_cookie(
    const uint8_t *cookie_header,
    uint32_t cookie_header_length,
    enum appliance_language *language_out,
    bool *present_out)
{
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    bool duplicate = false;
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;

    if (language_out == NULL || present_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    *present_out = false;
    *language_out = APPLIANCE_LANGUAGE_DEFAULT;
    if (appliance_panel_login_cookie_find(
            cookie_header,
            cookie_header_length,
            APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME,
            APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME_LENGTH,
            &value,
            &value_length,
            &duplicate) != APPLIANCE_PANEL_LOGIN_OK) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    if (duplicate) {
        return APPLIANCE_PANEL_LOGIN_MALFORMED;
    }
    if (value == NULL) {
        return APPLIANCE_PANEL_LOGIN_OK;
    }
    if (value_length != APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_LENGTH ||
        appliance_language_parse(
            (const char *)value, value_length, &language) !=
            APPLIANCE_LANGUAGE_OK) {
        return APPLIANCE_PANEL_LOGIN_MALFORMED;
    }
    *language_out = language;
    *present_out = true;
    return APPLIANCE_PANEL_LOGIN_OK;
}

enum appliance_panel_login_status appliance_panel_login_language_cookie_set(
    enum appliance_language language,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    const char *code = appliance_language_code(language);

    if (code == NULL) {
        return APPLIANCE_PANEL_LOGIN_MALFORMED;
    }
    return appliance_panel_login_language_cookie_write(
        code, false, output, output_capacity, written_out);
}

enum appliance_panel_login_status appliance_panel_login_language_cookie_clear(
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out)
{
    return appliance_panel_login_language_cookie_write(
        NULL, true, output, output_capacity, written_out);
}

enum appliance_panel_login_status appliance_panel_login_language_form_parse(
    const uint8_t *body,
    uint32_t body_length,
    enum appliance_language *language_out)
{
    static const char field[] = "language=";
    const uint32_t field_length = (uint32_t)(sizeof(field) - 1U);

    if (language_out == NULL) {
        return APPLIANCE_PANEL_LOGIN_NULL;
    }
    if (body == NULL) {
        return body_length == 0U ? APPLIANCE_PANEL_LOGIN_MALFORMED
                                 : APPLIANCE_PANEL_LOGIN_NULL;
    }
    if (body_length !=
            field_length + APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_LENGTH ||
        memcmp(body, field, field_length) != 0) {
        return APPLIANCE_PANEL_LOGIN_MALFORMED;
    }
    if (appliance_language_parse(
            (const char *)body + field_length,
            APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_LENGTH,
            language_out) != APPLIANCE_LANGUAGE_OK) {
        return APPLIANCE_PANEL_LOGIN_MALFORMED;
    }
    return APPLIANCE_PANEL_LOGIN_OK;
}

bool appliance_panel_login_csp_ok(
    const uint8_t *policy,
    uint32_t policy_length)
{
    if (policy == NULL) {
        return false;
    }
    if (!appliance_panel_login_contains(
            policy, policy_length, "default-src 'none'")) {
        return false;
    }
    if (!appliance_panel_login_contains(
            policy, policy_length, "form-action 'self'")) {
        return false;
    }
    if (appliance_panel_login_contains(
            policy, policy_length, "form-action 'none'")) {
        return false;
    }
    if (appliance_panel_login_contains(policy, policy_length, "script-src")) {
        return false;
    }
    if (!appliance_panel_login_contains(
            policy, policy_length, "frame-ancestors 'none'")) {
        return false;
    }
    return true;
}

const char *appliance_panel_login_status_string(
    enum appliance_panel_login_status status)
{
    switch (status) {
    case APPLIANCE_PANEL_LOGIN_OK:
        return "APPLIANCE_PANEL_LOGIN_OK";
    case APPLIANCE_PANEL_LOGIN_NULL:
        return "APPLIANCE_PANEL_LOGIN_NULL";
    case APPLIANCE_PANEL_LOGIN_MALFORMED:
        return "APPLIANCE_PANEL_LOGIN_MALFORMED";
    case APPLIANCE_PANEL_LOGIN_TOO_LARGE:
        return "APPLIANCE_PANEL_LOGIN_TOO_LARGE";
    case APPLIANCE_PANEL_LOGIN_MISSING_FIELD:
        return "APPLIANCE_PANEL_LOGIN_MISSING_FIELD";
    case APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE:
        return "APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE";
    case APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL:
        return "APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL";
    case APPLIANCE_PANEL_LOGIN_ABSENT:
        return "APPLIANCE_PANEL_LOGIN_ABSENT";
    default:
        return NULL;
    }
}
