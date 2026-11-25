#ifndef HFOC_APPLIANCE_PANEL_LOGIN_H
#define HFOC_APPLIANCE_PANEL_LOGIN_H

#include "appliance_admin_auth.h"
#include "appliance_admin_credential.h"
#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-445 browser sign-in for the management panel.
 *
 * Until this item the panel was reachable only by a client that could send
 * `Authorization: Bearer <token>`, and the token was issued by the local
 * administration console: the panel published no credential form and no cookie,
 * so a browser on the laptop could not sign in at all.  The owner's decision
 * (2026-09-18, option 1) is one form, one cookie:
 *
 *   * the sign-in page carries exactly one form; it posts the administrator
 *     secret to the same origin, and the page loads no script and no external
 *     asset;
 *   * a successful sign-in answers `Set-Cookie` with one `hfoc_session`
 *     cookie that is `HttpOnly`, `Secure`, `SameSite=Strict`, `Path=/` and
 *     short lived, and sends the browser to the section list;
 *   * a section accepts that cookie or the pre-existing
 *     `Authorization: Bearer` header, so curl and the browser both keep
 *     working;
 *   * sign-out posts to its own target and answers a cookie that expires
 *     immediately.
 *
 * This module owns no session, no credential, no tick and no authorization
 * decision.  It parses the two byte strings a browser sends (the url-encoded
 * form body and the `Cookie` header) and it formats the one byte string the
 * browser stores (`Set-Cookie`).  Every decision stays with the existing
 * administrator-session boundary, and the token in the cookie is exactly the
 * token that boundary issued: the panel re-presents it as the bearer token it
 * already understands, so there is still one session store and one expiry
 * rule.
 *
 * The password is never stored beyond the parse buffer, never copied into a
 * record, and zeroed by `appliance_panel_login_form_zero()` as soon as the
 * caller has derived the proof.
 */

#define APPLIANCE_PANEL_LOGIN_VERSION 1U
#define APPLIANCE_PANEL_LOGIN_COOKIE_NAME "hfoc_session"
#define APPLIANCE_PANEL_LOGIN_COOKIE_NAME_LENGTH 12U
/* The token is 32 opaque bytes; the cookie carries them as 64 lower-case hex. */
#define APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH \
    (APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH * 2U)
/*
 * ITEM-513: the second cookie the browser keeps.  The language is a property
 * of the browser, not of the machine: the nav's `EN | RU` buttons set this
 * cookie with one click, it wins over the shared `v1:system.language` record
 * while it is present, and the record stays untouched.  The value is exactly
 * one code the catalog knows (`en`/`ru`); a value the reader cannot trust is a
 * named refusal, and the panel falls back to the record instead of guessing.
 */
#define APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME "hfoc_lang"
#define APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME_LENGTH 9U
#define APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_LENGTH 2U
/* One code, the fixed attributes and a `Max-Age=0` on the clear path. */
#define APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_MAX 96U
/* `language=` plus the code, and nothing else. */
#define APPLIANCE_PANEL_LANGUAGE_FIELD_MAX 32U
/*
 * The exact `Set-Cookie` value the panel emits for a session.  It is bounded
 * by the cookie name, the hex token, the fixed attributes and a ten-digit
 * `Max-Age`, so a buffer of this size cannot truncate an accepted value.
 */
#define APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX 160U
#define APPLIANCE_PANEL_LOGIN_FIELD_MAX 128U
#define APPLIANCE_PANEL_LOGIN_BODY_MAX 512U
/*
 * ITEM-506 increment 6: the credential-change form has four fields and the
 * password policy allows 128 printable bytes each, so its worst-case encoded
 * body is around 1.6 KB; the cap is the server's own framing bound, not the
 * login form's 512.
 */
#define APPLIANCE_PANEL_ACCOUNT_BODY_MAX 2048U
#define APPLIANCE_PANEL_LOGIN_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PANEL_LOGIN_CONTENT_TYPE_LENGTH 33U
/*
 * The short browser session the panel asks the administrator boundary for.
 * The token and its expiry stay in the one administrator session store; the
 * cookie's `Max-Age` matches this lifetime so the browser drops it when the
 * session behind it has already expired.
 */
#define APPLIANCE_PANEL_LOGIN_SESSION_SECONDS 900U

enum appliance_panel_login_status {
    APPLIANCE_PANEL_LOGIN_OK = 0,
    APPLIANCE_PANEL_LOGIN_NULL = 1,
    APPLIANCE_PANEL_LOGIN_MALFORMED = 2,
    APPLIANCE_PANEL_LOGIN_TOO_LARGE = 3,
    APPLIANCE_PANEL_LOGIN_MISSING_FIELD = 4,
    APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE = 5,
    APPLIANCE_PANEL_LOGIN_BUFFER_TOO_SMALL = 6,
    APPLIANCE_PANEL_LOGIN_ABSENT = 7,
};

struct appliance_panel_login_form {
    bool user_present;
    bool password_present;
    uint32_t user_length;
    uint32_t password_length;
    /*
     * ITEM-506 increment 6: one byte past the policy maximum, so a full
     * 128-byte password is accepted and still NUL-terminated.  The previous
     * size made the login form refuse the policy's own maximum.
     */
    uint8_t user[APPLIANCE_PANEL_LOGIN_FIELD_MAX + 1U];
    uint8_t password[APPLIANCE_PANEL_LOGIN_FIELD_MAX + 1U];
};

/*
 * Parse `application/x-www-form-urlencoded` into the two fields the form
 * declares.  `+` is a space and `%XX` is one decoded byte; a malformed escape,
 * a field with no `=`, a repeated `username`/`password`, or a value that does
 * not fit the field buffer is refused as MALFORMED or TOO_LARGE instead of
 * being silently truncated.  Unknown fields are ignored; a body that lacks
 * either declared field is MISSING_FIELD, so a partial submission never
 * reaches the credential boundary.
 */
enum appliance_panel_login_status appliance_panel_login_form_parse(
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_login_form *form_out);

/*
 * Zero a parsed form, password included.  The caller calls this after the
 * proof has been derived, so the secret does not outlive the request.
 */
void appliance_panel_login_form_zero(struct appliance_panel_login_form *form);

/*
 * ITEM-506 increment 6: the credential-change form.  Four fields exactly -
 * `current`, `login`, `password`, `repeat` - decoded with the same rules as
 * the sign-in form (`+` is a space, `%XX` is one byte, a malformed escape, a
 * decoded NUL or line break, a repeated field and a field with no `=` are
 * refused).  A field that overflows its policy bound is TOO_LARGE instead of
 * being cut, a missing field is MISSING_FIELD, and a field the form does not
 * declare is refused rather than ignored: the change form has no room for a
 * stray parameter.  The caller must zero the form after the proof is derived
 * and never copy the password anywhere but the bounded candidate it holds for
 * the confirmation window.
 */
struct appliance_panel_account_form {
    bool current_present;
    bool login_present;
    bool password_present;
    bool repeat_present;
    uint32_t current_length;
    uint32_t login_length;
    uint32_t password_length;
    uint32_t repeat_length;
    uint8_t current[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    uint8_t login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    uint8_t password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    uint8_t repeat[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
};

enum appliance_panel_login_status appliance_panel_account_form_parse(
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_account_form *form_out);

void appliance_panel_account_form_zero(
    struct appliance_panel_account_form *form);

/*
 * Extract the session token from a `Cookie` header value.  The header may be
 * given with or without its `Cookie:` name.  `present_out` is false and the
 * status is OK when no `hfoc_session` cookie is present; a cookie of the right
 * name with anything but 64 hex digits, or two `hfoc_session` cookies in one
 * header, is MALFORMED.  A caller that sees MALFORMED must treat the request
 * as unsigned rather than guess.
 */
enum appliance_panel_login_status appliance_panel_login_cookie_token(
    const uint8_t *cookie_header,
    uint32_t cookie_header_length,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    bool *present_out);

/*
 * Format the `Set-Cookie` value for an issued token:
 *
 *   hfoc_session=<64 hex>; HttpOnly; Secure; SameSite=Strict; Path=/;
 *   Max-Age=<max_age_seconds>
 *
 * and, for sign-out, the same value with an empty token and `Max-Age=0`.  Both
 * refuse a buffer that cannot hold the whole value instead of truncating an
 * attribute: a truncated `SameSite` or `Secure` would be a silently weaker
 * cookie.
 */
enum appliance_panel_login_status appliance_panel_login_cookie_set(
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t max_age_seconds,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out);

enum appliance_panel_login_status appliance_panel_login_cookie_clear(
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out);

/*
 * ITEM-513: the per-browser language cookie.  `language_cookie()` reads
 * `hfoc_lang` from the same header the session reader reads, with the same
 * rules: the header may carry its `Cookie:` name, a pair with no `=` is not a
 * cookie pair, surrounding spaces are trimmed, and two `hfoc_lang` cookies in
 * one header are MALFORMED rather than resolved.  Only the exact two codes the
 * catalog knows are accepted: an empty value, a longer value, a value outside
 * the catalog and an upper-case code are all MALFORMED, so the caller answers
 * with a named refusal and the record's language instead of guessing.
 * `present_out` is false and the status is OK when the cookie is absent;
 * `language_out` is only written when it is present.
 *
 * `language_cookie_set()` writes
 *
 *   hfoc_lang=<en|ru>; HttpOnly; Secure; SameSite=Strict; Path=/
 *
 * and `language_cookie_clear()` the same value with `Max-Age=0`.  Neither
 * truncates: a buffer that cannot hold the whole value is
 * BUFFER_TOO_SMALL, exactly like the session cookie.
 *
 * `language_form_parse()` parses the one-field body the nav buttons submit:
 * `language=en` or `language=ru`, byte for byte.  A second field, a repeated
 * `=`, an empty value, an unknown code and a body that is not the declared
 * field are MALFORMED.  The form has exactly one click and no token, so the
 * parse is the whole contract.
 */
enum appliance_panel_login_status appliance_panel_login_language_cookie(
    const uint8_t *cookie_header,
    uint32_t cookie_header_length,
    enum appliance_language *language_out,
    bool *present_out);

enum appliance_panel_login_status appliance_panel_login_language_cookie_set(
    enum appliance_language language,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out);

enum appliance_panel_login_status appliance_panel_login_language_cookie_clear(
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written_out);

enum appliance_panel_login_status appliance_panel_login_language_form_parse(
    const uint8_t *body,
    uint32_t body_length,
    enum appliance_language *language_out);

/*
 * The content security policy the sign-in form needs: the one attribute that
 * changes relative to the pre-item policy is `form-action 'self'` (the form
 * posts to the same origin, and nowhere else).  `script-src` stays absent, so
 * the page cannot run a script, and the function refuses a policy that grew
 * one or that weakened `default-src 'none'`.
 */
bool appliance_panel_login_csp_ok(
    const uint8_t *policy,
    uint32_t policy_length);

const char *appliance_panel_login_status_string(
    enum appliance_panel_login_status status);

#endif
