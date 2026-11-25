/*
 * ITEM-445: the browser sign-in form and the one session cookie.
 *
 * The 2026-09-18 decision made the panel reachable from the laptop's browser:
 * one form that posts the administrator secret to the same origin, and one
 * `HttpOnly` session cookie that the section pages accept next to the existing
 * `Authorization: Bearer` header.  This proof drives the real module - the
 * kernel source is linked into the binary, not copied - and it pins the
 * properties the decision depends on:
 *
 *   * the form decodes what a browser actually sends (`+`, `%XX`, a charset
 *     parameter) and refuses what it cannot decode honestly instead of
 *     guessing;
 *   * a partial or repeated submission never reaches the credential boundary;
 *   * the cookie carries the issued token as 64 hex digits and names every
 *     attribute the contract requires: HttpOnly, Secure, SameSite=Strict,
 *     Path=/, and the short Max-Age;
 *   * sign-out is the same cookie with an empty value and Max-Age=0;
 *   * the cookie reader accepts the header with or without its name, refuses a
 *     value it cannot trust (wrong length, non-hex, two session cookies), and
 *     reports absence as absence rather than as a token;
 *   * a buffer that cannot hold the whole cookie is refused, so an attribute
 *     can never be truncated away;
 *   * the content security policy still refuses scripts and now allows the
 *     form to post to its own origin;
 *   * the secret is not echoed into any value this module produces, and the
 *     parse buffer is zeroed when the caller is done with it.
 */
#include "appliance_panel_login.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static unsigned int checks;

#define CHECK(condition, message) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static uint32_t text_length(const char *text)
{
    return (uint32_t)strlen(text);
}

static bool bytes_contain(
    const uint8_t *haystack,
    uint32_t haystack_length,
    const char *needle)
{
    uint32_t needle_length = text_length(needle);
    uint32_t index;

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

static void test_form_parse(void)
{
    static const char content_type[] =
        "application/x-www-form-urlencoded";
    struct appliance_panel_login_form form;
    enum appliance_panel_login_status status;

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=admin&password=s3cret",
        text_length("username=admin&password=s3cret"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "a plain submission parses");
    CHECK(form.user_present && form.user_length == 5U &&
              memcmp(form.user, "admin", 5U) == 0,
          "the form names the administrator");
    CHECK(form.password_present && form.password_length == 6U &&
              memcmp(form.password, "s3cret", 6U) == 0,
          "the form carries the secret");
    appliance_panel_login_form_zero(&form);

    /* A charset parameter on the content type is what a browser sends. */
    status = appliance_panel_login_form_parse(
        (const uint8_t *)"application/x-www-form-urlencoded; charset=UTF-8",
        text_length("application/x-www-form-urlencoded; charset=UTF-8"),
        (const uint8_t *)"username=a&password=b",
        text_length("username=a&password=b"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "a charset parameter does not break the form");

    /* Percent escapes and `+` decode to the bytes the browser typed. */
    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=ad%6Din&password=a+b%21",
        text_length("username=ad%6Din&password=a+b%21"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              memcmp(form.user, "admin", 5U) == 0 &&
              memcmp(form.password, "a b!", 4U) == 0,
          "percent escapes and plus signs decode");
    appliance_panel_login_form_zero(&form);

    /* An unknown field is ignored; the two declared fields still parse. */
    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"csrf=x&username=admin&password=b",
        text_length("csrf=x&username=admin&password=b"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "an unknown field is ignored");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)"text/plain",
        text_length("text/plain"),
        (const uint8_t *)"username=a&password=b",
        text_length("username=a&password=b"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE,
          "a form body that is not a form is refused");

    status = appliance_panel_login_form_parse(
        NULL,
        0U,
        (const uint8_t *)"username=a&password=b",
        text_length("username=a&password=b"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE,
          "a form body with no content type is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"",
        0U,
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MISSING_FIELD,
          "an empty form never reaches the credential boundary");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=admin",
        text_length("username=admin"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MISSING_FIELD,
          "a submission without the secret is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&username=b&password=c",
        text_length("username=a&username=b&password=c"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a repeated user field is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password=b&password=c",
        text_length("username=a&password=b&password=c"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a repeated secret field is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password=%zz",
        text_length("username=a&password=%zz"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a non-hex escape is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password=%4",
        text_length("username=a&password=%4"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a truncated escape is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password=%0a",
        text_length("username=a&password=%0a"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a decoded line break is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password=%00",
        text_length("username=a&password=%00"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a decoded NUL is refused");

    status = appliance_panel_login_form_parse(
        (const uint8_t *)content_type,
        text_length(content_type),
        (const uint8_t *)"username=a&password",
        text_length("username=a&password"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a field without a value is refused");

    {
        uint8_t oversized[APPLIANCE_PANEL_LOGIN_BODY_MAX + 2U];

        memset(oversized, (int)'a', sizeof(oversized));
        memcpy(oversized, "username=a&password=", 20U);
        status = appliance_panel_login_form_parse(
            (const uint8_t *)content_type,
            text_length(content_type),
            oversized,
            (uint32_t)sizeof(oversized),
            &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_TOO_LARGE,
              "a body beyond the bound is refused");
    }

    {
        uint8_t long_value[APPLIANCE_PANEL_LOGIN_FIELD_MAX + 64U];
        uint8_t body[APPLIANCE_PANEL_LOGIN_FIELD_MAX + 96U];
        uint32_t body_length;

        memset(long_value, (int)'x', sizeof(long_value));
        memcpy(body, "username=a&password=", 20U);
        memcpy(body + 20U, long_value, sizeof(long_value));
        body_length = 20U + (uint32_t)sizeof(long_value);
        status = appliance_panel_login_form_parse(
            (const uint8_t *)content_type,
            text_length(content_type),
            body,
            body_length,
            &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_TOO_LARGE,
              "a value beyond the field bound is refused, not truncated");
    }
}

/*
 * ITEM-506 increment 6: the credential-change form.  The same decoder and the
 * same refusals as the sign-in form, four declared names, and the policy's own
 * maximum password length accepted instead of cut at 127.
 */
static void test_account_form_parse(void)
{
    struct appliance_panel_account_form form;
    enum appliance_panel_login_status status;
    static const char body[] =
        "current=old+secret&login=operator&password=a+b%26c&repeat=a+b%26c";

    status = appliance_panel_account_form_parse(
        (const uint8_t *)body, text_length(body), &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "the four-field account form parses");
    CHECK(form.current_present && form.current_length == 10U &&
              memcmp(form.current, "old secret", 10U) == 0,
          "the current password decodes `+` to a space");
    CHECK(form.login_present && form.login_length == 8U &&
              memcmp(form.login, "operator", 8U) == 0,
          "the new login parses");
    CHECK(form.password_present && form.password_length == 5U &&
              memcmp(form.password, "a b&c", 5U) == 0 &&
              form.repeat_present && form.repeat_length == 5U &&
              memcmp(form.repeat, "a b&c", 5U) == 0,
          "the new password and its repeat decode `%26` and `+`");
    appliance_panel_account_form_zero(&form);

    /* A missing declared field is refused, never defaulted. */
    status = appliance_panel_account_form_parse(
        (const uint8_t *)"current=a&login=b&password=c",
        text_length("current=a&login=b&password=c"), &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MISSING_FIELD,
          "a missing repeat field is refused");

    /* A repeated field is refused. */
    status = appliance_panel_account_form_parse(
        (const uint8_t *)"current=a&current=b&login=c&password=d&repeat=d",
        text_length("current=a&current=b&login=c&password=d&repeat=d"),
        &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a repeated field is refused");

    /* A field the form does not declare is refused, not ignored. */
    status = appliance_panel_account_form_parse(
        (const uint8_t *)"current=a&login=b&password=c&repeat=c&extra=x",
        text_length("current=a&login=b&password=c&repeat=c&extra=x"), &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a stray field is refused");

    /* A malformed escape and a decoded NUL are refused. */
    status = appliance_panel_account_form_parse(
        (const uint8_t *)"current=a&login=b&password=bad%2&repeat=bad%2",
        text_length("current=a&login=b&password=bad%2&repeat=bad%2"), &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a truncated escape is refused");
    status = appliance_panel_account_form_parse(
        (const uint8_t *)"current=a&login=b&password=x%00y&repeat=x%00y",
        text_length("current=a&login=b&password=x%00y&repeat=x%00y"), &form);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a decoded NUL is refused");

    /* The policy's maximum password is usable; one byte more is refused. */
    {
        char maximum[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
        char over[
            APPLIANCE_PANEL_ACCOUNT_BODY_MAX +
            APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 8U];
        size_t index;

        for (index = 0U; index < (size_t)APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX;
             ++index) {
            maximum[index] = 'p';
        }
        maximum[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX] = '\0';
        snprintf(over, sizeof(over),
                 "current=a&login=b&password=%s&repeat=%s", maximum, maximum);
        status = appliance_panel_account_form_parse(
            (const uint8_t *)over, (uint32_t)strlen(over), &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
                  form.password_length ==
                      APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX,
              "a 128-byte password is accepted, not cut at 127");
        appliance_panel_account_form_zero(&form);

        snprintf(over, sizeof(over),
                 "current=a&login=b&password=%sx&repeat=%sx", maximum,
                 maximum);
        status = appliance_panel_account_form_parse(
            (const uint8_t *)over, (uint32_t)strlen(over), &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_TOO_LARGE,
              "a 129-byte password is refused, not truncated");
    }

    /* A 33-byte login is over the login bound and refused. */
    {
        static const char over_login[] =
            "current=a&login=0123456789012345678901234567890123"
            "&password=b&repeat=b";

        status = appliance_panel_account_form_parse(
            (const uint8_t *)over_login, text_length(over_login), &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_TOO_LARGE,
              "an over-long login is refused, not truncated");
    }

    /* A body past the account cap is refused whole. */
    {
        static uint8_t huge[APPLIANCE_PANEL_ACCOUNT_BODY_MAX + 1U];

        memset(huge, 'a', sizeof(huge));
        status = appliance_panel_account_form_parse(
            huge, (uint32_t)sizeof(huge), &form);
        CHECK(status == APPLIANCE_PANEL_LOGIN_TOO_LARGE,
              "a body past the account cap is refused");
    }
}

static void test_account_form_zero(void)
{
    struct appliance_panel_account_form form;
    uint32_t index;
    bool any = false;

    CHECK(appliance_panel_account_form_parse(
              (const uint8_t *)
                  "current=old&login=operator&password=new+secret&repeat=new+secret",
              text_length(
                  "current=old&login=operator&password=new+secret&repeat=new+secret"),
              &form) == APPLIANCE_PANEL_LOGIN_OK,
          "the account secret parses before it is zeroed");
    appliance_panel_account_form_zero(&form);
    for (index = 0U; index < sizeof(form); ++index) {
        if (((const uint8_t *)&form)[index] != 0U) {
            any = true;
        }
    }
    CHECK(!any, "the account parse buffer is zeroed whole");
}

static void test_form_zero(void)
{
    static const char content_type[] =
        "application/x-www-form-urlencoded";
    struct appliance_panel_login_form form;
    uint32_t index;
    bool any = false;

    CHECK(appliance_panel_login_form_parse(
              (const uint8_t *)content_type,
              text_length(content_type),
              (const uint8_t *)"username=admin&password=topsecret",
              text_length("username=admin&password=topsecret"),
              &form) == APPLIANCE_PANEL_LOGIN_OK,
          "the secret parses before it is zeroed");
    appliance_panel_login_form_zero(&form);
    for (index = 0U; index < sizeof(form); ++index) {
        if (((const uint8_t *)&form)[index] != 0U) {
            any = true;
        }
    }
    CHECK(!any, "the parse buffer is zeroed after the proof is derived");
}

static void test_cookie_set(void)
{
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t value[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
    uint32_t length = 0U;
    uint32_t index;
    enum appliance_panel_login_status status;

    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = (uint8_t)index;
    }
    status = appliance_panel_login_cookie_set(
        token,
        APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
        value,
        sizeof(value),
        &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "a session cookie is formatted");
    CHECK(bytes_contain(value, length, "hfoc_session=0001020304050607"),
          "the cookie carries the issued token as lower-case hex");
    CHECK(bytes_contain(value, length, "; HttpOnly"),
          "the cookie is HttpOnly");
    CHECK(bytes_contain(value, length, "; Secure"),
          "the cookie is Secure");
    CHECK(bytes_contain(value, length, "; SameSite=Strict"),
          "the cookie is SameSite=Strict");
    CHECK(bytes_contain(value, length, "; Path=/"),
          "the cookie is scoped to the panel root");
    CHECK(bytes_contain(value, length, "; Max-Age=900"),
          "the cookie carries the short session lifetime");
    CHECK(!bytes_contain(value, length, "SameSite=None"),
          "the cookie never relaxes SameSite");

    /* The exact buffer bound holds the whole value; one byte less refuses. */
    status = appliance_panel_login_cookie_set(
        token,
        APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
        value,
        length - 1U,
        &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL,
          "a buffer that cannot hold the attributes is refused");

    /* Sign-out is the same cookie, empty, already expired. */
    status = appliance_panel_login_cookie_clear(value, sizeof(value), &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK,
          "the sign-out cookie is formatted");
    CHECK(bytes_contain(value, length, "hfoc_session=;"),
          "sign-out empties the cookie value");
    CHECK(bytes_contain(value, length, "; Max-Age=0"),
          "sign-out expires the cookie immediately");
    CHECK(bytes_contain(value, length, "; HttpOnly") &&
              bytes_contain(value, length, "; Secure") &&
              bytes_contain(value, length, "; SameSite=Strict"),
          "sign-out keeps every protective attribute");
}

static void test_cookie_token(void)
{
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t other[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    bool present = false;
    uint32_t index;
    enum appliance_panel_login_status status;

    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = (uint8_t)(0xf0U | index);
    }
    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"Cookie: other=1; hfoc_session=f0f1f2f3f4f5f6f7"
                         "f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f; z=2",
        text_length("Cookie: other=1; hfoc_session=f0f1f2f3f4f5f6f7"
                    "f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f; z=2"),
        token,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present,
          "the session cookie is found among other cookies");
    CHECK(memcmp(token, "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7"
                       "\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff"
                       "\x00\x01\x02\x03\x04\x05\x06\x07"
                       "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f", 32U) == 0,
          "the hex value decodes to the issued token");

    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"hfoc_session=F0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF"
                         "000102030405060708090A0B0C0D0E0F",
        text_length("hfoc_session=F0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF"
                    "000102030405060708090A0B0C0D0E0F"),
        other,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present &&
              memcmp(other, token, 32U) == 0,
          "the reader accepts the header without its name and upper-case hex");

    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"other=1",
        text_length("other=1"),
        token,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && !present,
          "a header without the session cookie reports absence");

    status = appliance_panel_login_cookie_token(NULL, 0U, token, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && !present,
          "no cookie header at all reports absence");

    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"hfoc_session=00ff",
        text_length("hfoc_session=00ff"),
        token,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a short cookie value is refused");

    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"hfoc_session=zzf1f2f3f4f5f6f7f8f9fafbfcfdfeff"
                         "000102030405060708090a0b0c0d0e0f",
        text_length("hfoc_session=zzf1f2f3f4f5f6f7f8f9fafbfcfdfeff"
                    "000102030405060708090a0b0c0d0e0f"),
        token,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a non-hex cookie value is refused");

    status = appliance_panel_login_cookie_token(
        (const uint8_t *)"hfoc_session=000102030405060708090a0b0c0d0e0f"
                         "000102030405060708090a0b0c0d0e0f; "
                         "hfoc_session=000102030405060708090a0b0c0d0e0f"
                         "000102030405060708090a0b0c0d0e0f",
        text_length("hfoc_session=000102030405060708090a0b0c0d0e0f"
                    "000102030405060708090a0b0c0d0e0f; "
                    "hfoc_session=000102030405060708090a0b0c0d0e0f"
                    "000102030405060708090a0b0c0d0e0f"),
        token,
        &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "two session cookies are refused, not resolved by guessing");

    /* A cookie the writer produced is a cookie the reader accepts. */
    {
        uint8_t value[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
        uint32_t length = 0U;
        uint8_t round_trip[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];

        CHECK(appliance_panel_login_cookie_set(
                  token,
                  APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
                  value,
                  sizeof(value),
                  &length) == APPLIANCE_PANEL_LOGIN_OK,
              "the cookie value is written for the round trip");
        status = appliance_panel_login_cookie_token(
            value, length, round_trip, &present);
        CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present &&
                  memcmp(round_trip, token, sizeof(token)) == 0,
              "the cookie the panel sets is the cookie the panel reads");
    }
}

/*
 * ITEM-513: the language is a property of the browser, not of the machine.
 *
 * The owner's decision (2026-10-10) adds a second cookie next to the session:
 * `hfoc_lang=en|ru`, set by the nav's `EN | RU` buttons with one click and no
 * token.  The shared record `v1:system.language` stays the machine's default
 * and is never written by this path; a browser without the cookie renders the
 * record's language.  The reader is deliberately as strict as the session
 * reader: only the exact two codes are accepted, a duplicate is refused rather
 * than resolved, and a value the reader cannot trust is a named refusal the
 * caller answers by falling back to the record - never by guessing.
 */
static void test_language_control(void)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_COUNT;
    bool present = true;
    uint8_t value[APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_MAX];
    uint32_t length = 0U;
    enum appliance_panel_login_status status;

    /* The writer, both codes, and the attributes the decision names. */
    status = appliance_panel_login_language_cookie_set(
        APPLIANCE_LANGUAGE_RU, value, sizeof(value), &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              length == text_length("hfoc_lang=ru; HttpOnly; Secure; "
                                    "SameSite=Strict; Path=/") &&
              memcmp(value, "hfoc_lang=ru; HttpOnly; Secure; "
                            "SameSite=Strict; Path=/",
                     length) == 0,
          "the language cookie names the code and every attribute");
    status = appliance_panel_login_language_cookie_set(
        APPLIANCE_LANGUAGE_EN, value, sizeof(value), &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              length == text_length("hfoc_lang=en; HttpOnly; Secure; "
                                    "SameSite=Strict; Path=/") &&
              memcmp(value, "hfoc_lang=en; HttpOnly; Secure; "
                            "SameSite=Strict; Path=/",
                     length) == 0,
          "the English code is written the same way");

    /* A buffer that cannot hold the whole value is refused, not truncated. */
    {
        uint8_t small[8];
        uint32_t small_length = 0U;

        CHECK(appliance_panel_login_language_cookie_set(
                  APPLIANCE_LANGUAGE_RU, small, sizeof(small),
                  &small_length) ==
                  APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL,
              "a buffer smaller than the cookie is refused");
    }

    /* The reader: with and without the header name, among other cookies. */
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"Cookie: other=1; hfoc_lang=ru; z=2",
        text_length("Cookie: other=1; hfoc_lang=ru; z=2"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present &&
              language == APPLIANCE_LANGUAGE_RU,
          "the language cookie is found among other cookies");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=en",
        text_length("hfoc_lang=en"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present &&
              language == APPLIANCE_LANGUAGE_EN,
          "the reader accepts the header without its name");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"other=1",
        text_length("other=1"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && !present,
          "a header without the language cookie reports absence");
    status = appliance_panel_login_language_cookie(
        NULL, 0U, &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && !present,
          "no cookie header at all reports absence");

    /* A value the reader cannot trust is a named refusal, never a guess. */
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=",
        text_length("hfoc_lang="),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "an empty language cookie is refused by name");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=de",
        text_length("hfoc_lang=de"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a language outside the catalog is refused by name");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=EN",
        text_length("hfoc_lang=EN"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "an upper-case code is refused rather than folded");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=english",
        text_length("hfoc_lang=english"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "a longer value is refused rather than truncated");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"hfoc_lang=en; hfoc_lang=ru",
        text_length("hfoc_lang=en; hfoc_lang=ru"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "two language cookies are refused, not resolved by guessing");
    status = appliance_panel_login_language_cookie(
        (const uint8_t *)"xhfoc_lang=ru; hfoc_langx=ru",
        text_length("xhfoc_lang=ru; hfoc_langx=ru"),
        &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && !present,
          "a cookie whose name only contains the name is not the cookie");

    /* The writer's value is the reader's value. */
    status = appliance_panel_login_language_cookie(
        value, length, &language, &present);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK && present &&
              language == APPLIANCE_LANGUAGE_EN,
          "the cookie the panel sets is the cookie the panel reads");

    /* Sign-out leaves the language alone; clearing is a value and a Max-Age. */
    status = appliance_panel_login_language_cookie_clear(
        value, sizeof(value), &length);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              length == text_length("hfoc_lang=; HttpOnly; Secure; "
                                    "SameSite=Strict; Path=/; Max-Age=0") &&
              memcmp(value, "hfoc_lang=; HttpOnly; Secure; "
                            "SameSite=Strict; Path=/; Max-Age=0",
                     length) == 0,
          "clearing the language cookie is the same cookie with Max-Age=0");

    /*
     * The nav form's body: exactly one field, `language=en|ru`.  Anything
     * else - a second field, a repeated field, a stray `=`, an unknown code -
     * is MALFORMED, so the switch never half-applies.
     */
    language = APPLIANCE_LANGUAGE_COUNT;
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=ru", text_length("language=ru"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              language == APPLIANCE_LANGUAGE_RU,
          "the switch form parses the Russian code");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=en", text_length("language=en"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_OK &&
              language == APPLIANCE_LANGUAGE_EN,
          "the switch form parses the English code");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=de", text_length("language=de"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "the switch form refuses a code outside the catalog");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=", text_length("language="),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "the switch form refuses an empty value");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=ru&hostname=x",
        text_length("language=ru&hostname=x"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "the switch form refuses a second field");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=ru=1", text_length("language=ru=1"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "the switch form refuses a stray separator");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"hostname=x", text_length("hostname=x"),
        &language);
    CHECK(status == APPLIANCE_PANEL_LOGIN_MALFORMED,
          "the switch form refuses a body that is not its field");
    status = appliance_panel_login_language_form_parse(
        (const uint8_t *)"language=ru", text_length("language=ru"), NULL);
    CHECK(status == APPLIANCE_PANEL_LOGIN_NULL,
          "the switch form refuses a missing output");
}

static void test_csp(void)
{
    static const char policy[] =
        "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'";
    static const char old_policy[] =
        "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; "
        "form-action 'none'; frame-ancestors 'none'";
    static const char scripted_policy[] =
        "default-src 'none'; script-src 'self'; form-action 'self'; "
        "frame-ancestors 'none'";

    CHECK(appliance_panel_login_csp_ok(
              (const uint8_t *)policy, text_length(policy)),
          "the sign-in policy allows the form to post to its own origin");
    CHECK(!appliance_panel_login_csp_ok(
              (const uint8_t *)old_policy, text_length(old_policy)),
          "a policy that still forbids the form is not the item's policy");
    CHECK(!appliance_panel_login_csp_ok(
              (const uint8_t *)scripted_policy,
              text_length(scripted_policy)),
          "a policy that permits a script is refused");
    CHECK(!appliance_panel_login_csp_ok(
              (const uint8_t *)"form-action 'self'",
              text_length("form-action 'self'")),
          "a policy without default-src 'none' is refused");
    CHECK(!appliance_panel_login_csp_ok(NULL, 0U),
          "no policy is not an acceptable policy");
}

static void test_no_secret_echo(void)
{
    static const char content_type[] =
        "application/x-www-form-urlencoded";
    static const char secret[] = "hunter2-secret";
    uint8_t body[128];
    uint8_t value[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
    struct appliance_panel_login_form form;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t length = 0U;
    uint32_t index;

    snprintf((char *)body, sizeof(body),
             "username=admin&password=%s", secret);
    CHECK(appliance_panel_login_form_parse(
              (const uint8_t *)content_type,
              text_length(content_type),
              body,
              text_length((const char *)body),
              &form) == APPLIANCE_PANEL_LOGIN_OK,
          "the secret is parsed for the proof");
    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = (uint8_t)(index * 7U);
    }
    CHECK(appliance_panel_login_cookie_set(
              token,
              APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
              value,
              sizeof(value),
              &length) == APPLIANCE_PANEL_LOGIN_OK,
          "a session cookie is produced for the echo check");
    CHECK(!bytes_contain(value, length, secret),
          "the secret never reaches the cookie");
    CHECK(!bytes_contain(form.user, form.user_length, secret) &&
              !bytes_contain(form.password, form.password_length - 1U, secret),
          "the secret is not echoed into the other field");
    appliance_panel_login_form_zero(&form);
}

int main(void)
{
    test_form_parse();
    test_account_form_parse();
    test_form_zero();
    test_account_form_zero();
    test_cookie_set();
    test_cookie_token();
    /* ITEM-513: the per-browser language cookie and its one-field form. */
    test_language_control();
    test_csp();
    test_no_secret_echo();

    if (failures != 0) {
        fprintf(stderr, "Panel login host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("Panel login host test: OK (%u checks)\n", checks);
    return 0;
}
