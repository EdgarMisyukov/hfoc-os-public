#ifndef HFOC_APPLIANCE_MANAGEMENT_PANEL_H
#define HFOC_APPLIANCE_MANAGEMENT_PANEL_H

#include "appliance_admin_auth.h"
#include "appliance_management_web_ui.h"
#include "appliance_panel_login.h"
#include "appliance_production_auth.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-384 panel dispatch.
 *
 * This caller-owned, allocation-free composition turns a panel target into a
 * rendered document. It owns no session, no credential, no authorization
 * rule, and no route registration: every decision is delegated to the
 * existing ITEM-365 production auth composition (the ITEM-320 administrator
 * session boundary plus the ITEM-321 management authorization matrix), and
 * every document is rendered by the ITEM-384 web UI renderer.
 *
 * Deny by default: a panel target that is not one of the eleven published
 * sections is not a panel target at all and falls through to the ordinary
 * fixed route table, which answers 404. A panel request without a valid
 * administrator session is answered with the sign-in document and `401`; a
 * denied session with `403`; a locked-out administrator authentication
 * boundary with `429` and the remaining window; an unpublished management
 * plane with `503`. None of these is hidden behind an empty body.
 */

#define APPLIANCE_MANAGEMENT_PANEL_VERSION 1U
/*
 * ITEM-445 step 2: where a sign-in and a sign-out send the browser next.
 * ITEM-513 corrected the sign-in destination: it published
 * `/panel/dashboard`, which no panel target and no fixed route serves - the
 * Dashboard's declared section path is `/panel` - so a browser that followed
 * the 303 landed on the 404 document instead of the panel it had just been
 * admitted to.  The new QEMU language proof is the first proof that follows
 * the redirect, and it read that 404; the destination is now the declared
 * Dashboard target, which the section model, the authorization matrix and the
 * navigation all already serve.  The sign-out destination is unchanged.
 */
#define APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION "/panel"
#define APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH 6U
#define APPLIANCE_MANAGEMENT_PANEL_SIGN_OUT_LOCATION "/"
#define APPLIANCE_MANAGEMENT_PANEL_SIGN_OUT_LOCATION_LENGTH 1U
#define APPLIANCE_MANAGEMENT_PANEL_BODY_MAX \
    APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX

/* The session lifetime the panel asks the administrator boundary for. */
#define APPLIANCE_MANAGEMENT_PANEL_SESSION_LIFETIME \
    APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX

enum appliance_management_panel_status {
    APPLIANCE_MANAGEMENT_PANEL_OK = 0,
    APPLIANCE_MANAGEMENT_PANEL_NULL = 1,
    APPLIANCE_MANAGEMENT_PANEL_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL = 3,
    APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED = 4,
    APPLIANCE_MANAGEMENT_PANEL_ALLOWED = 5,
    APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED = 6,
    APPLIANCE_MANAGEMENT_PANEL_DENIED = 7,
    APPLIANCE_MANAGEMENT_PANEL_THROTTLED = 8,
    APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE = 9,
    APPLIANCE_MANAGEMENT_PANEL_BUFFER_TOO_SMALL = 10,
    /*
     * ITEM-509 increment 10: the renderer refused a page because the catalog
     * has no value for one of its keys in the language the machine is set to.
     * It is a named status of its own, not BUFFER_TOO_SMALL: the page is not
     * too large, it is untranslated, and the response that leaves the panel is
     * a 503 that names this cause.
     */
    APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION = 11,
    /* ITEM-619: the credential job is running; the caller must retry later. */
    APPLIANCE_MANAGEMENT_PANEL_PENDING = 12,
};

/*
 * ITEM-382 panel target classification. A panel target is one of the nine
 * management sections or the about/licenses page; anything else is not a panel
 * target and falls through to the fixed route table.
 */
enum appliance_management_panel_target_kind {
    APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE = 0,
    APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION = 1,
    APPLIANCE_MANAGEMENT_PANEL_TARGET_LICENSES = 2,
    /*
     * ITEM-445 step 2: the two browser sign-in targets. `/` is the sign-in
     * form's action and `/panel/logout` is the sign-out form's action; both
     * are POST-only, and a GET of `/` stays the frozen public sign-in
     * document served by the fixed route table.
     */
    APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN = 3,
    APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT = 4,
    /*
     * ITEM-508 increment 34: the ACME control page.  A page of its own - the
     * read-only ACME block is on the Dashboard, whose 2048-byte page cannot
     * hold the control forms - so it is a target kind like the licenses page,
     * not a management section.
     */
    APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME = 5,
    /*
     * ITEM-513: the nav's one-click language switch.  POST only, authorized
     * through the same `PANEL` route as the pages, and it answers the one
     * `hfoc_lang` cookie and a redirect instead of a document.
     */
    APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE = 6,
    /*
     * ITEM-516: the management-gate page.  A page of its own for the same
     * reason the ACME control page is: the warning the operator must read
     * before the setting is ever enabled does not fit on a section page.
     */
    APPLIANCE_MANAGEMENT_PANEL_TARGET_GATE = 7,
};

/*
 * ITEM-445 step 2: what the wire path extracted for this request besides the
 * request line. A caller that passes NULL to the dispatcher means "no cookie,
 * no body", which is exactly the pre-item behavior.
 */
struct appliance_management_panel_request {
    const uint8_t *cookie_header;
    uint32_t cookie_header_length;
    const uint8_t *content_type;
    uint32_t content_type_length;
    const uint8_t *body;
    uint32_t body_length;
};

struct appliance_management_panel_response {
    enum http_status_code status;
    enum http_content_type content_type;
    const uint8_t *body;
    uint32_t body_length;
    /*
     * The one `Set-Cookie` value a sign-in or sign-out answers with, and the
     * `Location` that sends the browser on, or NULL and zero for every other
     * response.
     */
    const uint8_t *set_cookie;
    uint32_t set_cookie_length;
    const uint8_t *location;
    uint32_t location_length;
};

/*
 * ITEM-445 step 2: the credential boundary the browser form consults. The
 * panel owns no secret, no verifier and no token generator; the caller binds
 * the one owner that already has them (the ITEM-401 administrator enrollment
 * for the ordinary boot, the fixture boundary for the QEMU proof), and the
 * panel only turns its decision into a page and a cookie.
 *
 * ITEM-506: the account is a login and a password, so the boundary carries
 * both fields the form parsed.  The `username` field stops being decorative:
 * the owner compares it with the stored login, and a panel that passed only
 * the password would accept half of the credential.
 */
enum appliance_management_panel_credential_status {
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK = 0,
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED = 1,
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_THROTTLED = 2,
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_UNAVAILABLE = 3,
    /* ITEM-619: the cooperative check has not finished; call again. */
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING = 4,
};

struct appliance_management_panel_credential {
    void *context;
    enum appliance_management_panel_credential_status (*authenticate)(
        void *context,
        const uint8_t *user,
        uint32_t user_length,
        const uint8_t *secret,
        uint32_t secret_length,
        uint64_t now_tick,
        uint32_t lifetime_ticks,
        uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
        uint32_t *session_id_out);
};

struct appliance_management_panel {
    bool initialized;
    uint32_t version;
    struct appliance_production_auth *auth;
    /*
     * Borrowed dynamic values. The panel never stores a copy and never
     * trusts them: the renderer escapes every one of them.
     */
    struct appliance_management_web_ui_view view;
    bool view_published;
    struct appliance_management_web_ui_licenses licenses;
    bool licenses_published;
    /*
     * ITEM-504: the bounded copy of the applied route table the Routing page
     * lists.  The route configuration owns the table; the panel renders the
     * copy it was given and escapes every byte of it.
     */
    struct appliance_management_web_ui_routes routes;
    bool routes_published;
    /*
     * ITEM-505: the active and saved network readings the Network page shows.
     * The serving composition owns the renderings; the panel borrows the
     * pointers for the render call and escapes every one of them.
     */
    struct appliance_management_web_ui_network network;
    bool network_published;
    /*
     * ITEM-505 increment 3: the active identity and the saved hostname the
     * System page shows.  The serving composition owns the renderings; the
     * panel borrows the pointers for the render call.
     */
    struct appliance_management_web_ui_system system;
    bool system_published;
    /*
     * ITEM-506 increment 6: the stored login the Account page shows.  The
     * serving composition owns the rendering; the panel borrows the pointer
     * for the render call and never sees the password.
     */
    struct appliance_management_web_ui_account account;
    bool account_published;
    /*
     * ITEM-507 increment 5: the active and saved name tables the Certificates
     * page shows.  The serving composition owns the renderings; the panel
     * borrows the pointers for the render call and never sees key material.
     */
    struct appliance_management_web_ui_certificates certificates;
    bool certificates_published;
    /*
     * ITEM-508 increment 34: the durable ACME control the control page shows.
     * The serving composition owns the rendering and the record; the panel
     * borrows the pointers for the render call and never sees a key or a token.
     */
    struct appliance_management_web_ui_acme_control acme_control;
    bool acme_control_published;
    /*
     * ITEM-516: the management gate the gate page shows.  The serving
     * composition owns the rendering and the durable record; the panel copies
     * the structure and never sees a secret.
     */
    struct appliance_management_web_ui_gate gate;
    bool gate_published;
    /* ITEM-445 step 2: the bound credential boundary, if any. */
    struct appliance_management_panel_credential credential;
    bool credential_published;
    uint32_t allowed_count;
    uint32_t unauthenticated_count;
    uint32_t denied_count;
    uint32_t throttled_count;
    uint32_t unavailable_count;
    uint32_t sign_in_count;
    uint32_t sign_out_count;
    uint8_t login_page[APPLIANCE_MANAGEMENT_PANEL_BODY_MAX];
    uint8_t section_page[APPLIANCE_MANAGEMENT_PANEL_BODY_MAX];
    uint8_t licenses_page[APPLIANCE_MANAGEMENT_PANEL_BODY_MAX];
    uint8_t acme_page[APPLIANCE_MANAGEMENT_PANEL_BODY_MAX];
    uint8_t gate_page[APPLIANCE_MANAGEMENT_PANEL_BODY_MAX];
    /* The cookie value the last browser sign-in or sign-out answered with. */
    uint8_t set_cookie_value[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
    /*
     * ITEM-513: the value the language switch answered with, or the clearing
     * value when a request presented a language cookie the reader refused.
     * The panel keeps the bytes it publishes: `set_cookie` in a response is a
     * borrowed pointer, exactly like the page bodies above.
     */
    uint8_t language_cookie_value[APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_MAX];
    /* Counters: switches applied and cookies the reader refused by name. */
    uint32_t language_count;
    uint32_t language_cookie_refused_count;
};

enum appliance_management_panel_status appliance_management_panel_init(
    struct appliance_management_panel *panel,
    struct appliance_production_auth *auth);
bool appliance_management_panel_ready(
    const struct appliance_management_panel *panel);
enum appliance_management_panel_status appliance_management_panel_set_view(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_view *view);
enum appliance_management_panel_status appliance_management_panel_clear_view(
    struct appliance_management_panel *panel);
/*
 * ITEM-382 license inventory presentation. The owner of the shipped payload
 * publishes what it found; the panel stores no copy it can trust and escapes
 * every value the renderer prints. An owner that publishes nothing leaves the
 * page UNAVAILABLE with its reason.
 */
enum appliance_management_panel_status appliance_management_panel_set_licenses(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_licenses *licenses);
enum appliance_management_panel_status
appliance_management_panel_clear_licenses(
    struct appliance_management_panel *panel);
/*
 * ITEM-504: publish the applied route table for the Routing page.  The owner
 * (the route configuration behind the management plane) composes the bounded
 * table text and publishes it; the panel answers nothing it was not given, so
 * an owner that publishes nothing leaves the page saying so.
 */
enum appliance_management_panel_status appliance_management_panel_set_routes(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_routes *routes);
enum appliance_management_panel_status
appliance_management_panel_clear_routes(
    struct appliance_management_panel *panel);
/*
 * ITEM-505: publish the network readings for the Network page.  The serving
 * composition owns the active and saved renderings; the panel copies the
 * structure (the strings stay borrowed for the render call) and answers
 * nothing it was not given, so an owner that publishes nothing leaves the
 * page saying so.
 */
enum appliance_management_panel_status appliance_management_panel_set_network(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_network *network);
enum appliance_management_panel_status
appliance_management_panel_clear_network(
    struct appliance_management_panel *panel);
/*
 * ITEM-505 increment 3: publish the system readings for the System page.  The
 * serving composition owns the active identity and the saved hostname; the
 * panel copies the structure (the strings stay borrowed for the render call)
 * and answers nothing it was not given.
 */
enum appliance_management_panel_status appliance_management_panel_set_system(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_system *system);
enum appliance_management_panel_status
appliance_management_panel_clear_system(
    struct appliance_management_panel *panel);
/*
 * ITEM-506 increment 6: publish the stored login for the Account page.  The
 * serving composition owns the rendering and never publishes the password; the
 * panel copies the structure (the string stays borrowed for the render call)
 * and answers nothing it was not given.
 */
enum appliance_management_panel_status appliance_management_panel_set_account(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_account *account);
enum appliance_management_panel_status
appliance_management_panel_clear_account(
    struct appliance_management_panel *panel);
/*
 * ITEM-507 increment 5: publish the active and saved name tables for the
 * Certificates page.  The serving composition owns the renderings (and never
 * publishes key material); the panel copies the structure (the strings stay
 * borrowed for the render call) and answers nothing it was not given.
 */
enum appliance_management_panel_status
appliance_management_panel_set_certificates(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_certificates *certificates);
enum appliance_management_panel_status
appliance_management_panel_clear_certificates(
    struct appliance_management_panel *panel);
/*
 * ITEM-508 increment 34: publish the durable ACME control for the control
 * page.  The serving composition owns the rendering; the panel copies the
 * structure (the strings stay borrowed for the render call) and answers
 * nothing it was not given.
 */
enum appliance_management_panel_status
appliance_management_panel_set_acme_control(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_acme_control *control);
enum appliance_management_panel_status
appliance_management_panel_clear_acme_control(
    struct appliance_management_panel *panel);
/*
 * ITEM-516: publish the management gate for the gate page.  The serving
 * composition owns the rendering and the durable record; the panel copies the
 * structure (the strings stay borrowed for the render call) and answers
 * nothing it was not given.
 */
enum appliance_management_panel_status
appliance_management_panel_set_gate(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_gate *gate);
enum appliance_management_panel_status
appliance_management_panel_clear_gate(
    struct appliance_management_panel *panel);
/*
 * Publish the declared inventory this build ships: the compiled-in
 * `appliance_license_inventory` mirror, whose notices the payload contract
 * forces onto the image and whose payload the LICENSE_PAYLOAD acceptance stage
 * verifies. The helper reads no file and verifies no checksum.
 */
enum appliance_management_panel_status
appliance_management_panel_publish_declared_licenses(
    struct appliance_management_panel *panel);
/* True when the target is one of the eleven published panel sections. */
bool appliance_management_panel_target(
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_web_ui_section *section_out);
/*
 * Classify a panel target: a section, the about/licenses page, or neither.
 * `section_out` is only written for a section target and may be NULL.
 */
enum appliance_management_panel_target_kind
appliance_management_panel_target_kind(
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_web_ui_section *section_out);
/*
 * The panel sign-in flow: it issues a session through the existing
 * administrator authentication boundary with the caller-supplied proof and
 * token. It adds no authentication factor and generates no secret.
 */
enum appliance_management_panel_status appliance_management_panel_login(
    struct appliance_management_panel *panel,
    const struct appliance_admin_auth_proof *proof,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *session_id_out);
/*
 * ITEM-445 step 2: bind or clear the one credential boundary the browser form
 * consults. The panel copies the two words it is given; the context they point
 * at stays owned by the caller and must outlive the binding. A NULL argument
 * clears the binding and the browser form answers UNAVAILABLE.
 */
enum appliance_management_panel_status
appliance_management_panel_set_credential(
    struct appliance_management_panel *panel,
    const struct appliance_management_panel_credential *credential);
enum appliance_management_panel_status appliance_management_panel_dispatch(
    struct appliance_management_panel *panel,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    uint64_t now_tick,
    const struct appliance_management_web_ui_confirmation *confirmation,
    const struct appliance_management_panel_request *request,
    struct appliance_management_panel_response *response_out);
uint32_t appliance_management_panel_allowed_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_unauthenticated_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_denied_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_throttled_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_unavailable_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_sign_in_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_sign_out_count(
    const struct appliance_management_panel *panel);
/*
 * ITEM-513: how many language switches this panel has applied, and how many
 * requests presented a language cookie the reader refused by name.  A refusal
 * is counted and answered by the record's language; it is never silently
 * ignored and never half-applied.
 */
uint32_t appliance_management_panel_language_count(
    const struct appliance_management_panel *panel);
uint32_t appliance_management_panel_language_cookie_refused_count(
    const struct appliance_management_panel *panel);
const char *appliance_management_panel_status_string(
    enum appliance_management_panel_status status);

#endif
