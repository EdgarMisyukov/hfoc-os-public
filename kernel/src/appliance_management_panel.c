#include "appliance_management_panel.h"

#include "appliance_license_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-386: the ordinary production boot serves the authenticated panel
 * family itself, so the panel dispatch is part of the production image.  The
 * host-test arm is unchanged and the host-test literal stays where the gate
 * expects it.
 */
#if defined(APPLIANCE_MANAGEMENT_PANEL_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool appliance_management_panel_target_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *expected)
{
    uint32_t index;

    for (index = 0U; index < target_length; ++index) {
        if (expected[index] == '\0') {
            return false;
        }
        if (target[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return expected[target_length] == '\0';
}

static bool appliance_management_panel_prefix(
    const uint8_t *target,
    uint32_t target_length)
{
    if (target_length < APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH_LENGTH) {
        return false;
    }
    if (!appliance_management_panel_target_equal(
            target,
            APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH_LENGTH,
            APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH)) {
        return false;
    }
    return target_length ==
               APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH_LENGTH ||
           target[APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH_LENGTH] ==
               (uint8_t)'/';
}

/*
 * ITEM-509 increment 8: `/panel/help/<id>` is the Help section's topic page.
 * The suffix is one catalog id (the renderer resolves it against the one help
 * catalog and names an unknown id instead of hiding it), so this helper only
 * enforces the shape: a non-empty segment of letters, digits, `-` and `_`, no
 * second slash and no other byte.  The copy is NUL terminated for the
 * renderer, which looks the id up by exact text.
 */
static bool appliance_management_panel_help_topic(
    const uint8_t *target,
    uint32_t target_length,
    char *topic_out,
    size_t topic_capacity)
{
    uint32_t index;
    uint32_t topic_length;

    if (target == NULL ||
        target_length <=
            APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH) {
        return false;
    }
    if (memcmp(
            target,
            APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX,
            APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH) != 0) {
        return false;
    }
    topic_length =
        target_length - APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH;
    if (topic_length > APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_MAX) {
        return false;
    }
    for (index = APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH;
         index < target_length;
         ++index) {
        const uint8_t value = target[index];
        const bool allowed =
            (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
            (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
            (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
            value == (uint8_t)'-' ||
            value == (uint8_t)'_';

        if (!allowed) {
            return false;
        }
    }
    if (topic_out != NULL) {
        if ((size_t)topic_length + 1U > topic_capacity) {
            return false;
        }
        memcpy(
            topic_out,
            target + APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH,
            (size_t)topic_length);
        topic_out[topic_length] = '\0';
    }
    return true;
}

bool appliance_management_panel_target(
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_web_ui_section *section_out)
{
    if (section_out == NULL) {
        return false;
    }
    return appliance_management_panel_target_kind(
               target, target_length, section_out) ==
           APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION;
}

enum appliance_management_panel_target_kind
appliance_management_panel_target_kind(
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_web_ui_section *section_out)
{
    size_t index;

    if (target == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE;
    }
    /*
     * ITEM-445 step 2: the two browser sign-in targets are classified first,
     * because `/` is not under the panel prefix at all.
     */
    if (target_length == HTTP_POLICY_FORM_TARGET_LENGTH &&
        target[0] == (uint8_t)'/') {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN;
    }
    if (target_length == HTTP_POLICY_SIGN_OUT_TARGET_LENGTH &&
        memcmp(
            target,
            HTTP_POLICY_SIGN_OUT_TARGET,
            HTTP_POLICY_SIGN_OUT_TARGET_LENGTH) == 0) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT;
    }
    if (!appliance_management_panel_prefix(target, target_length)) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE;
    }
    /* ITEM-382: the about/licenses page is a panel page, not a section. */
    if (appliance_management_panel_target_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH)) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_LICENSES;
    }
    /* ITEM-508 increment 34: the ACME control page is a panel page too. */
    if (appliance_management_panel_target_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH)) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME;
    }
    /* ITEM-516: the management-gate page is a panel page too. */
    if (appliance_management_panel_target_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH)) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_GATE;
    }
    /*
     * ITEM-513: the nav's one-click language switch.  It is a panel target
     * like the pages above - authorized through the same `PANEL` route, POST
     * only - but it renders no page of its own: its answer is the browser's
     * `hfoc_lang` cookie and a redirect.
     */
    if (appliance_management_panel_target_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH)) {
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE;
    }
    /*
     * ITEM-509 increment 8: a Help topic page is the Help section with a
     * catalog id in its path.  It resolves to the same section so the same
     * authorization route and the same page budget cover it; the dispatch
     * extracts the id for the renderer.
     */
    if (appliance_management_panel_help_topic(
            target, target_length, NULL, 0U)) {
        if (section_out != NULL) {
            *section_out = APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP;
        }
        return APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION;
    }
    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        const char *path = appliance_management_web_ui_section_path(
            (enum appliance_management_web_ui_section)index);

        if (path == NULL) {
            continue;
        }
        if (appliance_management_panel_target_equal(
                target, target_length, path)) {
            if (section_out != NULL) {
                *section_out =
                    (enum appliance_management_web_ui_section)index;
            }
            return APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION;
        }
    }
    return APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE;
}

static bool appliance_management_panel_initialized(
    const struct appliance_management_panel *panel)
{
    return panel != NULL &&
           panel->initialized &&
           panel->version == APPLIANCE_MANAGEMENT_PANEL_VERSION &&
           panel->auth != NULL;
}

/*
 * ITEM-513 (was ITEM-509 increment 10): the one place the panel decides which
 * language this request renders in.  The owner's decision of 2026-10-10 gives
 * the priority: a valid `hfoc_lang` cookie - the browser's own choice, set by
 * the nav's one-click switch - wins over the shared `v1:system.language`
 * record; the record wins over the catalog's default `en`.  The record is the
 * one the System page publishes and the console command writes, and this path
 * never writes it.
 *
 * A cookie the reader refuses (empty, unknown, upper-case, duplicated) is a
 * named refusal: `refused_out` is set so the caller counts it and answers the
 * browser a clearing cookie, and the request falls back to the record exactly
 * as if the cookie had been absent.  The page is never half rendered in the
 * refused language and never silently guessed at.
 *
 * `source_out` reports whether a source actually declared the language - the
 * cookie or the record - rather than the catalog's default answering for both
 * of them.  The Help surface needs the difference: the compiled chrome always
 * renders, but the Help body keeps ITEM-509's named refusal when no source
 * exists, instead of presenting the compiled topic pages as if a machine
 * record had chosen them.
 */
static enum appliance_language appliance_management_panel_language(
    const struct appliance_management_panel *panel,
    const struct appliance_management_panel_request *request,
    bool *refused_out,
    bool *source_out)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
    bool cookie_present = false;

    if (refused_out != NULL) {
        *refused_out = false;
    }
    if (source_out != NULL) {
        *source_out = false;
    }
    if (request != NULL) {
        const enum appliance_panel_login_status cookie_status =
            appliance_panel_login_language_cookie(
                request->cookie_header,
                request->cookie_header_length,
                &language,
                &cookie_present);

        if (cookie_status == APPLIANCE_PANEL_LOGIN_MALFORMED) {
            if (refused_out != NULL) {
                *refused_out = true;
            }
        } else if (cookie_status == APPLIANCE_PANEL_LOGIN_OK &&
                   cookie_present) {
            if (source_out != NULL) {
                *source_out = true;
            }
            return language;
        }
    }
    language = APPLIANCE_LANGUAGE_DEFAULT;
    if (panel != NULL && panel->system_published &&
        panel->system.language_active != NULL) {
        if (appliance_language_parse(
                panel->system.language_active,
                strlen(panel->system.language_active),
                &language) != APPLIANCE_LANGUAGE_OK) {
            return APPLIANCE_LANGUAGE_DEFAULT;
        }
        if (source_out != NULL) {
            *source_out = true;
        }
    }
    return language;
}

/*
 * ITEM-509 increment 10: turn a renderer refusal into the response the panel
 * publishes.  A missing translation is named as its own status and answered
 * with a 503 that carries no body, so the wire never sees the zero status the
 * older overflow path left behind; every other refusal keeps the existing
 * BUFFER_TOO_SMALL answer.
 */
static enum appliance_management_panel_status
appliance_management_panel_render_refusal(
    enum appliance_management_web_ui_status status,
    struct appliance_management_panel_response *response_out)
{
    if (status == APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION) {
        response_out->status = HTTP_STATUS_503_SERVICE_UNAVAILABLE;
        response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        response_out->body = NULL;
        response_out->body_length = 0U;
        return APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_PANEL_BUFFER_TOO_SMALL;
}

bool appliance_management_panel_ready(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return false;
    }
    return appliance_production_auth_available(panel->auth);
}

enum appliance_management_panel_status appliance_management_panel_init(
    struct appliance_management_panel *panel,
    struct appliance_production_auth *auth)
{
    if (panel == NULL || auth == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(panel, 0, sizeof(*panel));
    panel->initialized = true;
    panel->version = APPLIANCE_MANAGEMENT_PANEL_VERSION;
    panel->auth = auth;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_set_credential(
    struct appliance_management_panel *panel,
    const struct appliance_management_panel_credential *credential)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (credential == NULL) {
        memset(&panel->credential, 0, sizeof(panel->credential));
        panel->credential_published = false;
        return APPLIANCE_MANAGEMENT_PANEL_OK;
    }
    if (credential->authenticate == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    panel->credential = *credential;
    panel->credential_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status appliance_management_panel_set_view(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_view *view)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (view == NULL) {
        return appliance_management_panel_clear_view(panel);
    }
    panel->view = *view;
    panel->view_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status appliance_management_panel_clear_view(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->view, 0, sizeof(panel->view));
    panel->view_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status appliance_management_panel_set_licenses(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_licenses *licenses)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (licenses == NULL) {
        return appliance_management_panel_clear_licenses(panel);
    }
    panel->licenses = *licenses;
    panel->licenses_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_licenses(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->licenses, 0, sizeof(panel->licenses));
    panel->licenses_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

/*
 * ITEM-504: the Routing page lists the applied table its owner published.
 * The panel stores the bounded copy, so the page renders what the route
 * configuration applied and nothing else; `clear` withdraws it and the page
 * says the table is not published instead of showing an empty one.
 */
enum appliance_management_panel_status appliance_management_panel_set_routes(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_routes *routes)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (routes == NULL) {
        return appliance_management_panel_clear_routes(panel);
    }
    panel->routes = *routes;
    panel->routes_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_routes(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->routes, 0, sizeof(panel->routes));
    panel->routes_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

/*
 * ITEM-505: the Network page shows the active and the saved reading its owner
 * published.  The panel copies the structure and keeps the owner's strings
 * borrowed for the render call; `clear` withdraws the publication and the
 * page says the pair is not published instead of showing a blank.
 */
enum appliance_management_panel_status appliance_management_panel_set_network(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_network *network)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (network == NULL) {
        return appliance_management_panel_clear_network(panel);
    }
    panel->network = *network;
    panel->network_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_network(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->network, 0, sizeof(panel->network));
    panel->network_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

/*
 * ITEM-505 increment 3: the System page shows the active identity and the
 * saved hostname its owner published.  The panel copies the structure and
 * keeps the owner's strings borrowed for the render call; `clear` withdraws
 * the publication and the page says the pair is not published.
 */
enum appliance_management_panel_status appliance_management_panel_set_system(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_system *system)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (system == NULL) {
        return appliance_management_panel_clear_system(panel);
    }
    panel->system = *system;
    panel->system_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_system(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->system, 0, sizeof(panel->system));
    panel->system_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

/*
 * ITEM-506 increment 6: the Account page shows the stored login its owner
 * published.  The panel copies the structure and keeps the owner's string
 * borrowed for the render call; `clear` withdraws the publication and the page
 * says the login is not published.  The password never passes through here.
 */
enum appliance_management_panel_status appliance_management_panel_set_account(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_account *account)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (account == NULL) {
        return appliance_management_panel_clear_account(panel);
    }
    panel->account = *account;
    panel->account_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_account(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->account, 0, sizeof(panel->account));
    panel->account_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_set_certificates(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_certificates *certificates)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (certificates == NULL) {
        return appliance_management_panel_clear_certificates(panel);
    }
    panel->certificates = *certificates;
    panel->certificates_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_certificates(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->certificates, 0, sizeof(panel->certificates));
    panel->certificates_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_set_acme_control(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_acme_control *control)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (control == NULL) {
        return appliance_management_panel_clear_acme_control(panel);
    }
    panel->acme_control = *control;
    panel->acme_control_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_acme_control(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->acme_control, 0, sizeof(panel->acme_control));
    panel->acme_control_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

/* ITEM-516: the management gate the gate page renders. */
enum appliance_management_panel_status
appliance_management_panel_set_gate(
    struct appliance_management_panel *panel,
    const struct appliance_management_web_ui_gate *gate)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (gate == NULL) {
        return appliance_management_panel_clear_gate(panel);
    }
    panel->gate = *gate;
    panel->gate_published = true;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_clear_gate(
    struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&panel->gate, 0, sizeof(panel->gate));
    panel->gate_published = false;
    return APPLIANCE_MANAGEMENT_PANEL_OK;
}

enum appliance_management_panel_status
appliance_management_panel_publish_declared_licenses(
    struct appliance_management_panel *panel)
{
    struct appliance_management_web_ui_licenses licenses;

    if (!appliance_management_panel_initialized(panel)) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(&licenses, 0, sizeof(licenses));
    licenses.declared = true;
    licenses.payload_complete = true;
    licenses.manifest = appliance_license_inventory_manifest();
    licenses.payload_root = appliance_license_inventory_payload_root();
    licenses.declared_count = appliance_license_inventory_dependency_count();
    licenses.shipped_count = appliance_license_inventory_dependency_count();
    return appliance_management_panel_set_licenses(panel, &licenses);
}

enum appliance_management_panel_status appliance_management_panel_login(
    struct appliance_management_panel *panel,
    const struct appliance_admin_auth_proof *proof,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *session_id_out)
{
    enum appliance_admin_auth_status status;

    if (!appliance_management_panel_initialized(panel) ||
        proof == NULL || token == NULL || session_id_out == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    if (!appliance_production_auth_available(panel->auth)) {
        ++panel->unavailable_count;
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    status = appliance_admin_auth_authenticate_local(
        &panel->auth->admin,
        proof,
        token,
        now_tick,
        lifetime_ticks,
        session_id_out);
    switch (status) {
    case APPLIANCE_ADMIN_AUTH_OK:
        return APPLIANCE_MANAGEMENT_PANEL_OK;
    case APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED:
        ++panel->throttled_count;
        return APPLIANCE_MANAGEMENT_PANEL_THROTTLED;
    case APPLIANCE_ADMIN_AUTH_AUTH_FAILED:
    case APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL:
    case APPLIANCE_ADMIN_AUTH_SESSION_FULL:
    case APPLIANCE_ADMIN_AUTH_SESSION_ID_EXHAUSTED:
        ++panel->denied_count;
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    case APPLIANCE_ADMIN_AUTH_NULL:
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    default:
        ++panel->unavailable_count;
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
}

static void appliance_management_panel_sign_in(
    struct appliance_management_panel *panel,
    enum appliance_language language,
    const struct appliance_admin_auth_decision *decision,
    const char *notice,
    uint64_t lockout_remaining_ticks,
    enum http_status_code status,
    struct appliance_management_panel_response *response_out)
{
    struct appliance_management_web_ui_session session;
    size_t length = 0U;

    memset(&session, 0, sizeof(session));
    session.authenticated =
        decision != NULL && decision->authenticated;
    if (session.authenticated) {
        session.session_id = decision->session_id;
        session.principal_id = decision->principal_id;
    }
    session.notice = notice;
    session.lockout_remaining_ticks = lockout_remaining_ticks;
    /*
     * ITEM-513: the refusal page follows the same priority as every other
     * panel page - the browser's cookie, then the record, then the default -
     * so the language does not change under the operator at the sign-in page.
     */
    session.language = language;
    if (appliance_management_web_ui_render_login(
            &session,
            (char *)panel->login_page,
            sizeof(panel->login_page),
            &length) != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
        response_out->status = HTTP_STATUS_503_SERVICE_UNAVAILABLE;
        response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        response_out->body = NULL;
        response_out->body_length = 0U;
        return;
    }
    response_out->status = status;
    response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
    response_out->body = panel->login_page;
    response_out->body_length = (uint32_t)length;
}

static enum appliance_management_panel_status
appliance_management_panel_denial(
    struct appliance_management_panel *panel,
    enum appliance_language language,
    const struct appliance_admin_auth_decision *decision,
    const char *notice,
    struct appliance_management_panel_response *response_out)
{
    if (decision->authenticated) {
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            decision,
            notice,
            0U,
            HTTP_STATUS_403_FORBIDDEN,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    }
    ++panel->unauthenticated_count;
    appliance_management_panel_sign_in(
        panel,
        language,
        decision,
        notice,
        0U,
        HTTP_STATUS_401_UNAUTHORIZED,
        response_out);
    return APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED;
}

/*
 * ITEM-445 step 2: turn the request's authentication material into the one
 * canonical bearer line the existing gate already understands. The
 * `Authorization` header wins; otherwise the session cookie is decoded and
 * re-presented as exactly the token the administrator boundary issued. A
 * cookie that cannot be decoded honestly is treated as unsigned, never
 * guessed at.
 */
static enum appliance_production_auth_status
appliance_management_panel_authorize(
    struct appliance_management_panel *panel,
    enum http_method_kind method,
    const char *path,
    uint32_t path_length,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    const struct appliance_management_panel_request *request,
    uint64_t now_tick,
    struct appliance_admin_auth_decision *decision_out,
    enum appliance_management_authorization_route *route_out)
{
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization_header;
    uint32_t header_length = authorization_header_length;
    bool present = false;

    if (header_length == 0U && request != NULL &&
        request->cookie_header_length != 0U &&
        request->cookie_header != NULL) {
        if (appliance_panel_login_cookie_token(
                request->cookie_header,
                request->cookie_header_length,
                token,
                &present) == APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(
                bearer,
                APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(
                bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                token,
                APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    return appliance_production_auth_authorize_request(
        panel->auth,
        header,
        header_length,
        method,
        (const uint8_t *)path,
        path_length,
        now_tick,
        decision_out,
        route_out);
}

/*
 * ITEM-445 step 2: the browser form posts the administrator secret to the
 * same origin. The panel decodes the form, hands the secret to the bound
 * credential boundary (which owns the verifier, the throttle, the token and
 * the session), and answers either the one session cookie with a 303 to the
 * section list, or the sign-in page with the reason. The decoded form, secret
 * included, is zeroed before this function returns on every path.
 */
static enum appliance_management_panel_status
appliance_management_panel_browser_sign_in(
    struct appliance_management_panel *panel,
    const struct appliance_management_panel_request *request,
    uint64_t now_tick,
    struct appliance_management_panel_response *response_out)
{
    struct appliance_admin_auth_decision decision;
    struct appliance_panel_login_form form;
    enum appliance_panel_login_status form_status;
    enum appliance_management_panel_credential_status credential_status;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t cookie_length = 0U;
    uint32_t session_id = 0U;
    /* ITEM-513: the refusal page follows the request's language. */
    const enum appliance_language language =
        appliance_management_panel_language(panel, request, NULL, NULL);

    memset(&decision, 0, sizeof(decision));
    memset(&form, 0, sizeof(form));
    memset(token, 0, sizeof(token));
    if (request == NULL) {
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "FORM_MISSING",
            0U,
            HTTP_STATUS_400_BAD_REQUEST,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    }
    form_status = appliance_panel_login_form_parse(
        request->content_type,
        request->content_type_length,
        request->body,
        request->body_length,
        &form);
    if (form_status != APPLIANCE_PANEL_LOGIN_OK) {
        appliance_panel_login_form_zero(&form);
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            form_status == APPLIANCE_PANEL_LOGIN_BAD_CONTENT_TYPE
                ? "FORM_CONTENT_TYPE"
                : (form_status == APPLIANCE_PANEL_LOGIN_TOO_LARGE
                       ? "FORM_TOO_LARGE"
                       : (form_status ==
                                  APPLIANCE_PANEL_LOGIN_MISSING_FIELD
                              ? "FORM_MISSING_FIELD"
                              : "FORM_MALFORMED")),
            0U,
            HTTP_STATUS_400_BAD_REQUEST,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    }
    if (!appliance_production_auth_available(panel->auth)) {
        appliance_panel_login_form_zero(&form);
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    if (!panel->credential_published) {
        appliance_panel_login_form_zero(&form);
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    if (panel->auth->admin.locked_until_tick > now_tick) {
        const uint64_t remaining =
            panel->auth->admin.locked_until_tick - now_tick;

        appliance_panel_login_form_zero(&form);
        ++panel->throttled_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_THROTTLED",
            remaining,
            HTTP_STATUS_429_TOO_MANY_REQUESTS,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_THROTTLED;
    }
    credential_status = panel->credential.authenticate(
        panel->credential.context,
        form.user,
        form.user_length,
        form.password,
        form.password_length,
        now_tick,
        /*
         * ITEM-475 (round 14): the cookie's `Max-Age` is in seconds, but the
         * session table compares the lifetime against `now_tick`, which is
         * milliseconds (`hfoc_lwip_time_now()`).  Converting here keeps the
         * two ends of the browser session the same length instead of letting
         * the honest 100 Hz PIT expire a "900 second" cookie after 0.9 s.
         */
        APPLIANCE_PANEL_LOGIN_SESSION_SECONDS * 1000U,
        token,
        &session_id);
    appliance_panel_login_form_zero(&form);
    switch (credential_status) {
    case APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING:
        /*
         * ITEM-619: the check is a job now.  The serving loop owns the
         * connection and calls dispatch again when the job completes, so this
         * answer is not a response and must not be encoded as one.  No
         * counter, cookie or token changes here.
         */
        return APPLIANCE_MANAGEMENT_PANEL_PENDING;
    case APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK:
        break;
    case APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_THROTTLED:
        ++panel->throttled_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_THROTTLED",
            panel->auth->admin.locked_until_tick > now_tick
                ? panel->auth->admin.locked_until_tick - now_tick
                : 0U,
            HTTP_STATUS_429_TOO_MANY_REQUESTS,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_THROTTLED;
    case APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED:
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_DENIED",
            0U,
            HTTP_STATUS_401_UNAUTHORIZED,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    default:
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    if (appliance_panel_login_cookie_set(
            token,
            APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
            panel->set_cookie_value,
            (uint32_t)sizeof(panel->set_cookie_value),
            &cookie_length) != APPLIANCE_PANEL_LOGIN_OK) {
        memset(token, 0, sizeof(token));
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    memset(token, 0, sizeof(token));
    ++panel->sign_in_count;
    decision.authenticated = true;
    decision.principal_id = APPLIANCE_ADMIN_PRINCIPAL_ID;
    decision.session_id = session_id;
    response_out->status = HTTP_STATUS_303_SEE_OTHER;
    response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
    response_out->body = NULL;
    response_out->body_length = 0U;
    response_out->set_cookie = panel->set_cookie_value;
    response_out->set_cookie_length = cookie_length;
    response_out->location = (const uint8_t *)
        APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION;
    response_out->location_length =
        APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH;
    return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
}

/*
 * ITEM-445 step 2: sign-out expires the cookie and sends the browser back to
 * the sign-in document. It requires no session: a peer can only expire the
 * cookie it already holds, and the answer carries no other state.
 */
static enum appliance_management_panel_status
appliance_management_panel_browser_sign_out(
    struct appliance_management_panel *panel,
    struct appliance_management_panel_response *response_out)
{
    uint32_t cookie_length = 0U;

    if (appliance_panel_login_cookie_clear(
            panel->set_cookie_value,
            (uint32_t)sizeof(panel->set_cookie_value),
            &cookie_length) != APPLIANCE_PANEL_LOGIN_OK) {
        return APPLIANCE_MANAGEMENT_PANEL_BUFFER_TOO_SMALL;
    }
    ++panel->sign_out_count;
    response_out->status = HTTP_STATUS_303_SEE_OTHER;
    response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
    response_out->body = NULL;
    response_out->body_length = 0U;
    response_out->set_cookie = panel->set_cookie_value;
    response_out->set_cookie_length = cookie_length;
    response_out->location =
        (const uint8_t *)APPLIANCE_MANAGEMENT_PANEL_SIGN_OUT_LOCATION;
    response_out->location_length =
        APPLIANCE_MANAGEMENT_PANEL_SIGN_OUT_LOCATION_LENGTH;
    return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
}

/*
 * ITEM-513: the nav's one-click language switch.  The form submits exactly one
 * field, `language=en|ru`; a body that is anything else is a named refusal
 * (`PANEL_LANGUAGE_BAD_VALUE`, a rendered 400) and changes nothing.  A parsed
 * body answers the one `hfoc_lang` cookie with a 303 back to the dashboard -
 * the same shape the sign-in uses - and never touches the shared
 * `v1:system.language` record: the language is the browser's, and the record
 * stays the machine's default.  No token and no second phase: the decision is
 * one click and one cookie.
 */
static enum appliance_management_panel_status
appliance_management_panel_language_switch(
    struct appliance_management_panel *panel,
    enum appliance_language current,
    const struct appliance_management_panel_request *request,
    const struct appliance_admin_auth_decision *decision,
    struct appliance_management_panel_response *response_out)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
    uint32_t cookie_length = 0U;

    if (request == NULL ||
        appliance_panel_login_language_form_parse(
            request->body, request->body_length, &language) !=
            APPLIANCE_PANEL_LOGIN_OK) {
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            current,
            decision,
            "PANEL_LANGUAGE_BAD_VALUE",
            0U,
            HTTP_STATUS_400_BAD_REQUEST,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_DENIED;
    }
    if (appliance_panel_login_language_cookie_set(
            language,
            panel->language_cookie_value,
            (uint32_t)sizeof(panel->language_cookie_value),
            &cookie_length) != APPLIANCE_PANEL_LOGIN_OK) {
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            current,
            decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    ++panel->language_count;
    response_out->status = HTTP_STATUS_303_SEE_OTHER;
    response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
    response_out->body = NULL;
    response_out->body_length = 0U;
    response_out->set_cookie = panel->language_cookie_value;
    response_out->set_cookie_length = cookie_length;
    response_out->location = (const uint8_t *)
        APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION;
    response_out->location_length =
        APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH;
    return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
}

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
    struct appliance_management_panel_response *response_out)
{
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    enum appliance_production_auth_status auth_status;
    enum appliance_management_panel_target_kind target_kind;
    enum appliance_management_web_ui_section section =
        APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;
    struct appliance_management_web_ui_help help;
    char help_topic[APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_MAX + 1U];
    const char *path;
    size_t path_length = 0U;
    size_t length = 0U;
    bool throttled;
    /*
     * ITEM-513: this request's language and, when the browser presented a
     * language cookie the reader refused, the clearing cookie the answer
     * carries.  Both are decided once, before any page renders.
     */
    enum appliance_language language;
    bool language_cookie_refused = false;
    bool language_source = false;
    uint32_t language_cookie_length = 0U;

    if (!appliance_management_panel_initialized(panel) ||
        response_out == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_NULL;
    }
    memset(response_out, 0, sizeof(*response_out));
    target_kind = appliance_management_panel_target_kind(
        target, target_length, &section);
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE) {
        return APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL;
    }
    language = appliance_management_panel_language(
        panel, request, &language_cookie_refused, &language_source);
    if (language_cookie_refused) {
        ++panel->language_cookie_refused_count;
        if (appliance_panel_login_language_cookie_clear(
                panel->language_cookie_value,
                (uint32_t)sizeof(panel->language_cookie_value),
                &language_cookie_length) != APPLIANCE_PANEL_LOGIN_OK) {
            language_cookie_length = 0U;
        }
    }
    /*
     * ITEM-445 step 2: the two browser targets. `POST /` is the sign-in form
     * and `POST /panel/logout` is the sign-out form; a GET of `/` is
     * deliberately NOT a panel request, so the frozen public sign-in document
     * stays served by the fixed route table under its own counters.
     */
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN) {
        if (method == HTTP_METHOD_POST) {
            return appliance_management_panel_browser_sign_in(
                panel, request, now_tick, response_out);
        }
        return APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL;
    }
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT) {
        if (method == HTTP_METHOD_POST) {
            return appliance_management_panel_browser_sign_out(
                panel, response_out);
        }
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &(struct appliance_admin_auth_decision){ false, 0U, 0U, 0U },
            "PANEL_METHOD_NOT_ALLOWED",
            0U,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED;
    }
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE) {
        /*
         * ITEM-513: the switch is POST only.  A GET is refused like the
         * sign-out target's GET: the panel answers with its own document and
         * never redirects a browser that typed the address.
         */
        if (method != HTTP_METHOD_POST) {
            ++panel->denied_count;
            appliance_management_panel_sign_in(
                panel,
                language,
                &(struct appliance_admin_auth_decision){ false, 0U, 0U, 0U },
                "PANEL_METHOD_NOT_ALLOWED",
                0U,
                HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                response_out);
            return APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED;
        }
        /*
         * The switch is authorized through the same `PANEL` route as every
         * other panel target, with the declared constant and never the raw
         * request bytes.
         */
        path = APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH;
    } else if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_LICENSES) {
        /*
         * ITEM-382: the about/licenses page is authorized through the same
         * route as the section pages. The path handed to the authorization
         * matrix is the declared constant, never the raw request bytes.
         */
        path = APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH;
    } else if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME) {
        /*
         * ITEM-508 increment 34: the ACME control page is authorized through
         * the same route as the section pages, with its declared constant.
         */
        path = APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH;
    } else {
        path = appliance_management_web_ui_section_path(section);
    }
    if (path == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_BAD_STATE;
    }
    while (path[path_length] != '\0') {
        ++path_length;
    }

    if (method != HTTP_METHOD_GET && method != HTTP_METHOD_HEAD &&
        target_kind != APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE) {
        ++panel->denied_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &(struct appliance_admin_auth_decision){ false, 0U, 0U, 0U },
            "PANEL_METHOD_NOT_ALLOWED",
            0U,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED;
    }

    memset(&decision, 0, sizeof(decision));
    if (!appliance_production_auth_available(panel->auth)) {
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }

    /*
     * Throttling is published from the existing administrator
     * authentication boundary; the panel adds no second throttle and never
     * clears the existing lockout.
     */
    throttled = panel->auth->admin.locked_until_tick > now_tick;
    if (throttled) {
        ++panel->throttled_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_THROTTLED",
            panel->auth->admin.locked_until_tick - now_tick,
            HTTP_STATUS_429_TOO_MANY_REQUESTS,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_THROTTLED;
    }

    auth_status = appliance_management_panel_authorize(
        panel,
        method,
        path,
        (uint32_t)path_length,
        authorization_header,
        authorization_header_length,
        request,
        now_tick,
        &decision,
        &route);
    if (auth_status == APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE) {
        ++panel->unavailable_count;
        appliance_management_panel_sign_in(
            panel,
            language,
            &decision,
            "AUTH_UNAVAILABLE",
            0U,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            response_out);
        return APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE;
    }
    if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED ||
        route != APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL) {
        return appliance_management_panel_denial(
            panel,
            language,
            &decision,
            auth_status == APPLIANCE_PRODUCTION_AUTH_DEFAULT_DENIED
                ? "AUTH_DEFAULT_DENIED"
                : "AUTH_DENIED",
            response_out);
    }

    /*
     * ITEM-513: a language cookie the reader refused is answered with the
     * clearing value on the page that renders, so the browser stops sending
     * the value the panel named as refused.  The page itself is the record's
     * language, rendered whole.
     */
    if (language_cookie_length != 0U) {
        response_out->set_cookie = panel->language_cookie_value;
        response_out->set_cookie_length = language_cookie_length;
    }

    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE) {
        return appliance_management_panel_language_switch(
            panel, language, request, &decision, response_out);
    }

    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_LICENSES) {
        /*
         * ITEM-509 increment 10: the page renders in the language the system
         * record holds; the stored view is copied so the panel's own copy
         * stays the owner's publication and the render call gets the current
         * language even when the record loaded after the publication.
         */
        struct appliance_management_web_ui_licenses licenses;
        const struct appliance_management_web_ui_licenses *licenses_view = NULL;
        enum appliance_management_web_ui_status render_status;

        if (panel->licenses_published) {
            licenses = panel->licenses;
            licenses.language = language;
            licenses_view = &licenses;
        }
        render_status = appliance_management_web_ui_render_licenses(
            licenses_view,
            (char *)panel->licenses_page,
            sizeof(panel->licenses_page),
            &length);
        if (render_status != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            return appliance_management_panel_render_refusal(
                render_status, response_out);
        }
        response_out->status = HTTP_STATUS_200_OK;
        response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        response_out->body = panel->licenses_page;
        response_out->body_length = (uint32_t)length;
        ++panel->allowed_count;
        return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
    }
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME) {
        struct appliance_management_web_ui_acme_control control;
        const struct appliance_management_web_ui_acme_control *control_view =
            NULL;
        enum appliance_management_web_ui_status render_status;

        if (panel->acme_control_published) {
            control = panel->acme_control;
            control.language = language;
            control_view = &control;
        }
        render_status = appliance_management_web_ui_render_acme_control(
            control_view,
            confirmation,
            (char *)panel->acme_page,
            sizeof(panel->acme_page),
            &length);
        if (render_status != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            return appliance_management_panel_render_refusal(
                render_status, response_out);
        }
        response_out->status = HTTP_STATUS_200_OK;
        response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        response_out->body = panel->acme_page;
        response_out->body_length = (uint32_t)length;
        ++panel->allowed_count;
        return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
    }
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_GATE) {
        struct appliance_management_web_ui_gate gate;
        const struct appliance_management_web_ui_gate *gate_view = NULL;
        enum appliance_management_web_ui_status render_status;

        if (panel->gate_published) {
            gate = panel->gate;
            gate.language = language;
            gate_view = &gate;
        }
        render_status = appliance_management_web_ui_render_gate(
            gate_view,
            confirmation,
            (char *)panel->gate_page,
            sizeof(panel->gate_page),
            &length);
        if (render_status != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            return appliance_management_panel_render_refusal(
                render_status, response_out);
        }
        response_out->status = HTTP_STATUS_200_OK;
        response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        response_out->body = panel->gate_page;
        response_out->body_length = (uint32_t)length;
        ++panel->allowed_count;
        return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
    }
    /*
     * ITEM-509 increment 8: the Help renderer reads the language and the topic
     * from the request.  ITEM-513 changed the source: the language is now the
     * effective one this request resolved - the browser's cookie, the record,
     * or the catalog's default.  The chrome always renders that language, but
     * the Help body keeps its old named refusal when neither the cookie nor
     * the record declared a language: presenting the compiled topic pages as
     * if a machine record had chosen them would be less honest than the
     * refusal ITEM-509 published.  A valid cookie is a declaration, so a
     * browser that chose `ru` reads Russian help even when the record did not
     * load, and the Dashboard's help block reads the same view.
     */
    memset(&help, 0, sizeof(help));
    help_topic[0] = '\0';
    if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP &&
        appliance_management_panel_help_topic(
            target, target_length, help_topic, sizeof(help_topic)) &&
        help_topic[0] != '\0') {
        help.topic = help_topic;
    }
    help.published = language_source;
    help.language =
        language_source ? appliance_language_code(language) : NULL;
    help.reason = language_source ? NULL : "SYSTEM_LANGUAGE_UNAVAILABLE";
    /*
     * ITEM-509 increment 10: the section page renders in the language the
     * record holds.  The view is copied for the same reason the licenses and
     * ACME views are: the published structure stays the owner's, and the
     * language is the one read at this request.
     */
    {
        struct appliance_management_web_ui_view view;
        const struct appliance_management_web_ui_view *view_arg = NULL;
        enum appliance_management_web_ui_status render_status;

        if (panel->view_published) {
            view = panel->view;
            view.language = language;
            view_arg = &view;
        }
        render_status = appliance_management_web_ui_render_section(
            section,
            view_arg,
            confirmation,
            panel->routes_published ? &panel->routes : NULL,
            panel->network_published ? &panel->network : NULL,
            panel->system_published ? &panel->system : NULL,
            panel->account_published ? &panel->account : NULL,
            panel->certificates_published ? &panel->certificates : NULL,
            &help,
            (char *)panel->section_page,
            sizeof(panel->section_page),
            &length);
        if (render_status != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            return appliance_management_panel_render_refusal(
                render_status, response_out);
        }
    }
    response_out->status = HTTP_STATUS_200_OK;
    response_out->content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
    response_out->body = panel->section_page;
    response_out->body_length = (uint32_t)length;
    ++panel->allowed_count;
    return APPLIANCE_MANAGEMENT_PANEL_ALLOWED;
}

uint32_t appliance_management_panel_allowed_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->allowed_count;
}

uint32_t appliance_management_panel_unauthenticated_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->unauthenticated_count;
}

uint32_t appliance_management_panel_denied_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->denied_count;
}

uint32_t appliance_management_panel_throttled_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->throttled_count;
}

uint32_t appliance_management_panel_unavailable_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->unavailable_count;
}

uint32_t appliance_management_panel_sign_in_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->sign_in_count;
}

uint32_t appliance_management_panel_sign_out_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->sign_out_count;
}

uint32_t appliance_management_panel_language_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->language_count;
}

uint32_t appliance_management_panel_language_cookie_refused_count(
    const struct appliance_management_panel *panel)
{
    if (!appliance_management_panel_initialized(panel)) {
        return 0U;
    }
    return panel->language_cookie_refused_count;
}

const char *appliance_management_panel_status_string(
    enum appliance_management_panel_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_PANEL_OK:
        return "APPLIANCE_MANAGEMENT_PANEL_OK";
    case APPLIANCE_MANAGEMENT_PANEL_NULL:
        return "APPLIANCE_MANAGEMENT_PANEL_NULL";
    case APPLIANCE_MANAGEMENT_PANEL_BAD_STATE:
        return "APPLIANCE_MANAGEMENT_PANEL_BAD_STATE";
    case APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL:
        return "APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL";
    case APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED:
        return "APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED";
    case APPLIANCE_MANAGEMENT_PANEL_ALLOWED:
        return "APPLIANCE_MANAGEMENT_PANEL_ALLOWED";
    case APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED:
        return "APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED";
    case APPLIANCE_MANAGEMENT_PANEL_DENIED:
        return "APPLIANCE_MANAGEMENT_PANEL_DENIED";
    case APPLIANCE_MANAGEMENT_PANEL_THROTTLED:
        return "APPLIANCE_MANAGEMENT_PANEL_THROTTLED";
    case APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE:
        return "APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE";
    case APPLIANCE_MANAGEMENT_PANEL_BUFFER_TOO_SMALL:
        return "APPLIANCE_MANAGEMENT_PANEL_BUFFER_TOO_SMALL";
    case APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION:
        return "APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION";
    default:
        return NULL;
    }
}

#else

typedef int appliance_management_panel_unused;

#endif
