#include "appliance_management_panel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(APPLIANCE_MANAGEMENT_PANEL_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void fill(
    void *destination,
    uint8_t value,
    size_t length)
{
    uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static struct appliance_admin_auth_credential make_credential(
    uint8_t value)
{
    struct appliance_admin_auth_credential credential;

    memset(&credential, 0, sizeof(credential));
    credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    fill(credential.verifier, value, sizeof(credential.verifier));
    return credential;
}

static struct appliance_admin_auth_proof make_proof(uint8_t value)
{
    struct appliance_admin_auth_proof proof;

    memset(&proof, 0, sizeof(proof));
    proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    fill(proof.proof, value, sizeof(proof.proof));
    return proof;
}

static uint32_t make_bearer(
    uint8_t *header,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    memcpy(header,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    return APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH;
}

static bool contains(const uint8_t *body, uint32_t length, const char *text)
{
    size_t text_length = strlen(text);
    uint32_t index;

    if (length < (uint32_t)text_length) {
        return false;
    }
    for (index = 0U; index + (uint32_t)text_length <= length; ++index) {
        if (memcmp(body + index, text, text_length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_targets(void)
{
    static const char *const paths[] = {
        "/panel",
        "/panel/network",
        "/panel/services",
        "/panel/storage",
        "/panel/system",
        "/panel/security",
        "/panel/diagnostics",
        "/panel/updates",
        "/panel/recovery",
        "/panel/routing",
        /* ITEM-506 increment 6: the rendered Account page. */
        "/panel/account",
        /* ITEM-507 increment 5: the rendered Certificates page. */
        "/panel/certificates",
        /* ITEM-509 increment 8: the rendered Help index. */
        "/panel/help",
    };
    static const char *const rejected[] = {
        "/health",
        "/api/v1",
        "/api/v1/system",
        "/panel/",
        "/panel/attack",
        "/panel/system/extra",
        "/panelish",
        "/Panel",
        "/panel?x=1",
        "/panel/system#x",
        "//panel",
        "",
        /* ITEM-382: only the exact about/licenses target is a panel page. */
        "/panel/about",
        "/panel/about/",
        "/panel/about/licenses/",
        "/panel/about/licenses?x=1",
        "/panel/about/licenses#x",
        "//panel/about/licenses",
        "/panel/About/licenses",
        /* ITEM-508 increment 34: only the exact ACME control target. */
        "/panel/acme/",
        "/panel/acme?x=1",
        "/panel/acme#x",
        "//panel/acme",
        "/panel/ACME",
    };
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        enum appliance_management_web_ui_section section =
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;
        const char *path = paths[index];

        CHECK(strcmp(path,
                     appliance_management_web_ui_section_path(
                         (enum appliance_management_web_ui_section)
                             index)) == 0,
              "management panel path table is exact");
        CHECK(appliance_management_panel_target(
                  (const uint8_t *)path,
                  (uint32_t)strlen(path),
                  &section) &&
                  section ==
                      (enum appliance_management_web_ui_section)index &&
                  appliance_management_panel_target_kind(
                      (const uint8_t *)path,
                      (uint32_t)strlen(path),
                      &section) ==
                      APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION,
              "management panel resolves a published target");
    }
    /*
     * ITEM-508 increment 34: the ACME control page is a page of its own, not a
     * section, so it is classified like the licenses page.
     */
    CHECK(appliance_management_panel_target_kind(
              (const uint8_t *)APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH,
              APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH_LENGTH, NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME,
          "management panel classifies the ACME control page");
    /*
     * ITEM-516: the management-gate page is a page of its own too, and only
     * the exact target is one.
     */
    CHECK(appliance_management_panel_target_kind(
              (const uint8_t *)APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH,
              APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH_LENGTH, NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_TARGET_GATE,
          "management panel classifies the management-gate page");
    CHECK(appliance_management_panel_target_kind(
              (const uint8_t *)"/panel/gate/",
              (uint32_t)strlen("/panel/gate/"), NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE &&
              appliance_management_panel_target_kind(
                  (const uint8_t *)"/panel/GATE",
                  (uint32_t)strlen("/panel/GATE"), NULL) ==
                  APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE,
          "management panel refuses a target that only looks like the gate");
    /*
     * ITEM-509 increment 8: the Help index and its topic pages resolve to the
     * Help section.  A syntactically valid id resolves to the same section and
     * the renderer names a topic the catalog does not carry; a malformed path
     * is not a panel target at all.
     */
    {
        static const char *const help_topics[] = {
            "/panel/help",
            "/panel/help/install",
            "/panel/help/certificates",
            "/panel/help/no-such-topic",
        };
        static const char *const help_rejected[] = {
            "/panel/help/",
            "/panel/help/install/extra",
            "/panel/help/install name",
            "/panel/help/install%20name",
            "/panel/help/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        };
        size_t help_index;

        for (help_index = 0U;
             help_index < sizeof(help_topics) / sizeof(help_topics[0]);
             ++help_index) {
            enum appliance_management_web_ui_section help_section =
                APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;

            CHECK(appliance_management_panel_target(
                      (const uint8_t *)help_topics[help_index],
                      (uint32_t)strlen(help_topics[help_index]),
                      &help_section) &&
                      help_section ==
                          APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP &&
                      appliance_management_panel_target_kind(
                          (const uint8_t *)help_topics[help_index],
                          (uint32_t)strlen(help_topics[help_index]),
                          &help_section) ==
                          APPLIANCE_MANAGEMENT_PANEL_TARGET_SECTION,
                  "management panel resolves the Help index and its topics");
        }
        for (help_index = 0U;
             help_index < sizeof(help_rejected) / sizeof(help_rejected[0]);
             ++help_index) {
            CHECK(appliance_management_panel_target_kind(
                      (const uint8_t *)help_rejected[help_index],
                      (uint32_t)strlen(help_rejected[help_index]),
                      NULL) == APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE,
                  "management panel rejects a malformed Help topic path");
        }
    }
    for (index = 0U;
         index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        enum appliance_management_web_ui_section section =
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;

        CHECK(!appliance_management_panel_target(
                  (const uint8_t *)rejected[index],
                  (uint32_t)strlen(rejected[index]),
                  &section) &&
                  appliance_management_panel_target_kind(
                      (const uint8_t *)rejected[index],
                      (uint32_t)strlen(rejected[index]),
                      &section) ==
                      APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE,
              "management panel rejects an unpublished target");
    }
    CHECK(!appliance_management_panel_target(NULL, 4U, NULL) &&
              appliance_management_panel_target_kind(
                  (const uint8_t *)"/panel/about/licenses",
                  APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH_LENGTH,
                  NULL) == APPLIANCE_MANAGEMENT_PANEL_TARGET_LICENSES,
          "management panel target mapping rejects null inputs and classifies "
          "the licenses page without a section out-parameter");
}

static void test_lifecycle(void)
{
    struct appliance_production_auth auth;
    struct appliance_management_panel panel;

    /*
     * The structs are zeroed first: `init` refuses a NULL auth before it
     * publishes anything, so the panel would otherwise be read uninitialized
     * by the readiness check below.
     */
    memset(&auth, 0, sizeof(auth));
    memset(&panel, 0, sizeof(panel));
    CHECK(appliance_management_panel_init(NULL, &auth) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL &&
          appliance_management_panel_init(&panel, NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL,
          "management panel init rejects null inputs");
    CHECK(appliance_management_panel_init(&panel, NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL &&
          !appliance_management_panel_ready(&panel),
          "management panel is not ready without an auth composition");
    CHECK(appliance_management_panel_set_view(NULL, NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL &&
          appliance_management_panel_clear_view(NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL,
          "management panel view handling rejects a null panel");
}

static void test_unavailable_without_credential(void)
{
    struct appliance_production_auth auth;
    struct appliance_management_panel panel;
    struct appliance_management_panel_response response;
    static const uint8_t target[] = "/panel/system";

    memset(&auth, 0, sizeof(auth));
    memset(&panel, 0, sizeof(panel));
    memset(&response, 0, sizeof(response));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
          appliance_management_panel_init(&panel, &auth) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel fixture initializes");
    CHECK(!appliance_management_panel_ready(&panel),
          "management panel is not ready without a published credential");
    CHECK(appliance_management_panel_dispatch(
              &panel,
              HTTP_METHOD_GET,
              target,
              sizeof(target) - 1U,
              NULL,
              0U,
              0U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE &&
              response.status == HTTP_STATUS_503_SERVICE_UNAVAILABLE &&
              response.content_type == HTTP_CONTENT_TYPE_HTML_UTF8 &&
              contains(response.body,
                       response.body_length,
                       "AUTH_UNAVAILABLE"),
          "management panel publishes UNAVAILABLE without a credential");
    CHECK(appliance_management_panel_unavailable_count(&panel) == 1U,
          "management panel counts unavailable requests");
}

struct panel_fixture {
    struct appliance_production_auth auth;
    struct appliance_management_panel panel;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t bearer[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t bearer_length;
};

static bool panel_fixture_init(struct panel_fixture *fixture)
{
    struct appliance_admin_auth_credential credential =
        make_credential(0x42U);
    struct appliance_admin_auth_proof proof = make_proof(0x42U);
    uint32_t session_id = 0U;

    memset(fixture, 0, sizeof(*fixture));
    fill(fixture->token, 0x66U, sizeof(fixture->token));
    fixture->bearer_length =
        make_bearer(fixture->bearer, fixture->token);
    if (appliance_production_auth_init(&fixture->auth) !=
            APPLIANCE_PRODUCTION_AUTH_OK ||
        appliance_production_auth_publish_credential(
            &fixture->auth, &credential) != APPLIANCE_PRODUCTION_AUTH_OK ||
        appliance_management_panel_init(
            &fixture->panel, &fixture->auth) !=
            APPLIANCE_MANAGEMENT_PANEL_OK) {
        return false;
    }
    if (appliance_management_panel_login(
            &fixture->panel,
            &proof,
            fixture->token,
            0U,
            APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
            &session_id) != APPLIANCE_MANAGEMENT_PANEL_OK ||
        !appliance_management_panel_ready(&fixture->panel)) {
        return false;
    }
    return true;
}

static void test_session_flow(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    static const uint8_t dashboard[] = "/panel";
    static const uint8_t system[] = "/panel/system";
    static const uint8_t storage[] = "/panel/storage";
    static const uint8_t updates[] = "/panel/updates";
    uint8_t wrong_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t wrong_bearer[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                         APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t wrong_length;

    CHECK(panel_fixture_init(&fixture),
          "management panel session fixture signs in through the existing "
          "administrator boundary");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              dashboard,
              sizeof(dashboard) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED &&
              contains(response.body, response.body_length, "SIGNED OUT") &&
              contains(response.body,
                       response.body_length,
                       "Authorization: Bearer"),
          "management panel denies a request without a session");
    CHECK(appliance_management_panel_unauthenticated_count(&fixture.panel) ==
              1U,
          "management panel counts unauthenticated requests");

    fill(wrong_token, 0x11U, sizeof(wrong_token));
    wrong_length = make_bearer(wrong_bearer, wrong_token);
    CHECK(wrong_length == APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                             APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  wrong_bearer,
                  wrong_length,
                  1U,
                  NULL,
              NULL,
                  &response) ==
                  APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED,
          "management panel denies a wrong session token");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              system,
              sizeof(system) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              response.content_type == HTTP_CONTENT_TYPE_HTML_UTF8 &&
              contains(response.body, response.body_length, "<h2>System") &&
              contains(response.body, response.body_length, "AVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "/api/v1/system"),
          "management panel serves an authenticated section document");

    /*
     * ITEM-509 increment 8: the Help index and a topic page are served through
     * the same authenticated route.  This fixture publishes no system view, so
     * the page names the missing language instead of rendering an empty
     * document or inventing English text.
     */
    {
        static const uint8_t help[] = "/panel/help";
        static const uint8_t help_topic[] = "/panel/help/storage";

        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  help,
                  sizeof(help) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              response.content_type == HTTP_CONTENT_TYPE_HTML_UTF8 &&
              contains(response.body, response.body_length, "<h2>Help</h2>") &&
              contains(response.body,
                       response.body_length,
                       "built-in help catalog"),
              "management panel serves the Help index");
        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  help_topic,
                  sizeof(help_topic) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body, response.body_length, "<h2>Help</h2>") &&
              /*
               * ITEM-513: the effective language is the cookie, then the
               * record, then the catalog's default - but the Help body keeps
               * ITEM-509's named refusal when neither the cookie nor the
               * record declares a language.  The chrome is the compiled
               * English document; the Help block says what is missing instead
               * of presenting the compiled topics as a chosen language.
               */
              contains(response.body,
                       response.body_length,
                       "SYSTEM_LANGUAGE_UNAVAILABLE"),
              "management panel serves a Help topic page and names the "
              "missing language source");
    }

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_HEAD,
              dashboard,
              sizeof(dashboard) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body, response.body_length, "Dashboard"),
          "management panel serves a head request with the same document");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              storage,
              sizeof(storage) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "UNAVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "No management API owner"),
          "management panel publishes an unowned section as UNAVAILABLE");
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              updates,
              sizeof(updates) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "DEGRADED") &&
              contains(response.body,
                       response.body_length,
                       "Two-phase confirmation"),
          "management panel publishes a mutation-only section as DEGRADED");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_UNSUPPORTED,
              system,
              sizeof(system) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) ==
              APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED &&
              response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "management panel refuses a method other than GET or HEAD");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              (const uint8_t *)"/panel/attack",
              13U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL,
          "management panel leaves an unpublished target to the route table");

    /*
     * ITEM-509 increment 8: the two Help requests above are served pages too,
     * so the count moved from four to six.
     */
    CHECK(appliance_management_panel_allowed_count(&fixture.panel) == 6U,
          "management panel counts allowed requests");
}

static void test_confirmation_and_escaping(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_confirmation confirmation;
    static const uint8_t updates[] = "/panel/updates";
    static const uint8_t network[] = "/panel/network";
    static const uint8_t system[] = "/panel/system";
    static const uint8_t account[] = "/panel/account";
    static const uint8_t certificates[] = "/panel/certificates";
    struct appliance_management_web_ui_account account_view;
    struct appliance_management_web_ui_certificates certificates_view;

    CHECK(panel_fixture_init(&fixture),
          "management panel view fixture signs in");

    memset(&view, 0, sizeof(view));
    view.hostname = "hfoc-appliance";
    view.mac_address = "52:54:00:12:34:56<script>";
    view.service_name = "httpsd\" onmouseover=\"alert(1)";
    view.log_line = "<img src=x onerror=alert(1)>\nsecond line";
    view.detail = "&amp; escaped";
    CHECK(appliance_management_panel_set_view(&fixture.panel, &view) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel publishes a dynamic view");

    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(4294967297);
    confirmation.mutation = "UPDATE_ACCEPT";
    confirmation.remaining_ticks = 271U;
    confirmation.single_use = true;
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              updates,
              sizeof(updates) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              &confirmation,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "Confirmation token: 4294967297") &&
              contains(response.body, response.body_length, "(single-use)") &&
              contains(response.body, response.body_length, "Expires in: 271") &&
              contains(response.body,
                       response.body_length,
                       "UPDATE_ACCEPT") &&
              contains(response.body,
                       response.body_length,
                       "hfoc-appliance"),
          "management panel publishes the two-phase confirmation token");
    CHECK(!contains(response.body,
                    response.body_length,
                    "<script>") &&
              !contains(response.body,
                        response.body_length,
                        "<img src=x") &&
              !contains(response.body,
                        response.body_length,
                        "onmouseover=\"alert(1)\"") &&
              contains(response.body,
                       response.body_length,
                       "&lt;script&gt;"),
          "management panel escapes every dynamic value it serves");

    /*
     * ITEM-505: the Network page carries the network form, not the generic
     * value dump, so a cleared view must leave it naming the missing owner
     * instead of rendering a blank.
     */
    CHECK(appliance_management_panel_clear_view(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_clear_network(&fixture.panel) ==
                  APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  network,
                  sizeof(network) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "No network owner is published") &&
              contains(response.body,
                       response.body_length,
                       "action=\"/panel/network\""),
          "management panel names a missing network reading instead of hiding "
          "it");

    /*
     * ITEM-505 increment 3: the System page carries the hostname form and the
     * two-phase confirmation form, so a cleared owner must leave it naming the
     * missing owner instead of rendering a blank.
     */
    CHECK(appliance_management_panel_clear_system(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  system,
                  sizeof(system) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "No system owner is published") &&
              contains(response.body,
                       response.body_length,
                       "action=\"/panel/system\"") &&
              contains(response.body,
                       response.body_length,
                       "name=\"hostname\""),
          "management panel names a missing system reading instead of hiding "
          "it");

    /*
     * ITEM-506 increment 6: the Account page carries the credential-change
     * form, so a cleared owner must leave it naming the missing owner instead
     * of rendering a blank.
     */
    CHECK(appliance_management_panel_clear_account(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  account,
                  sizeof(account) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "No account owner is published") &&
              contains(response.body,
                       response.body_length,
                       "action=\"/panel/account\"") &&
              contains(response.body,
                       response.body_length,
                       "name=\"current\""),
          "management panel names a missing account reading instead of hiding "
          "it");

    /*
     * ITEM-506 increment 6: a published account shows the stored login and the
     * change form, and never a password: the panel has no password to show.
     */
    memset(&account_view, 0, sizeof(account_view));
    account_view.published = true;
    account_view.login = "operator";
    CHECK(appliance_management_panel_set_account(
              &fixture.panel, &account_view) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  account,
                  sizeof(account) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "Login <code>operator</code>") &&
              contains(response.body,
                       response.body_length,
                       "type=\"password\"") &&
              contains(response.body,
                       response.body_length,
                       "signs out every session"),
          "management panel Account page shows the login and both forms");

    /*
     * ITEM-507 increment 5: the Certificates page carries the two name tables
     * and the mutation forms.  A cleared owner leaves the page naming the
     * missing owner instead of rendering a blank; a published one shows the
     * saved list, the default declaration and the reboot note, and never a
     * key.
     */
    CHECK(appliance_management_panel_clear_certificates(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  certificates,
                  sizeof(certificates) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "No certificate owner is published") &&
              contains(response.body,
                       response.body_length,
                       "action=\"/panel/certificates\"") &&
              contains(response.body,
                       response.body_length,
                       "name=\"certificate\""),
          "management panel names a missing certificate owner instead of "
          "hiding it");

    memset(&certificates_view, 0, sizeof(certificates_view));
    certificates_view.published = true;
    certificates_view.count = 1U;
    certificates_view.shown = 1U;
    certificates_view.active = "a.example (423)";
    certificates_view.active_default = "CERTIFICATE";
    certificates_view.active_source = "COMMITTED";
    certificates_view.saved = "a.example (423)";
    certificates_view.saved_default = "REFUSE";
    certificates_view.saved_source = "COMMITTED";
    certificates_view.applies = "reboot";
    CHECK(appliance_management_panel_set_certificates(
              &fixture.panel, &certificates_view) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  certificates,
                  sizeof(certificates) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "Saved <code>a.example (423)</code>") &&
              contains(response.body,
                       response.body_length,
                       "default <code>REFUSE</code>") &&
              contains(response.body,
                       response.body_length,
                       "applies=reboot") &&
              !contains(response.body,
                        response.body_length,
                        "PRIVATE KEY"),
          "management panel Certificates page shows both readings and never a "
          "key");
    CHECK(appliance_management_panel_clear_certificates(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel clears the certificate readings");

    /*
     * A section that still renders the generic value dump keeps publishing a
     * missing value as the literal `unavailable` rather than hiding it.
     * ITEM-505 increment 3: the System section now carries its own readings
     * and forms, so the generic dump is proven on Updates.
     */
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              updates,
              sizeof(updates) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
          contains(response.body,
                   response.body_length,
                   "<dd>unavailable</dd>"),
          "management panel publishes a missing value instead of hiding it");
}

static void test_throttling_and_expiry(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_admin_auth_proof wrong_proof = make_proof(0x99U);
    uint8_t throttled_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    static const uint8_t system[] = "/panel/system";
    size_t attempt;

    CHECK(panel_fixture_init(&fixture),
          "management panel throttle fixture signs in");
    fill(throttled_token, 0x77U, sizeof(throttled_token));

    for (attempt = 0U;
         attempt + 1U < APPLIANCE_ADMIN_AUTH_FAILURE_LIMIT;
         ++attempt) {
        enum appliance_management_panel_status status;
        uint32_t ignored = 0U;

        status = appliance_management_panel_login(
            &fixture.panel,
            &wrong_proof,
            throttled_token,
            0U,
            APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
            &ignored);
        CHECK(status == APPLIANCE_MANAGEMENT_PANEL_DENIED,
              "management panel sign-in failure is denied");
    }
    CHECK(appliance_management_panel_login(
              &fixture.panel,
              &wrong_proof,
              throttled_token,
              0U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              &session_id) == APPLIANCE_MANAGEMENT_PANEL_THROTTLED,
          "management panel sign-in throttles after repeated failures");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              system,
              sizeof(system) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              0U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_THROTTLED &&
              response.status == HTTP_STATUS_429_TOO_MANY_REQUESTS &&
              contains(response.body, response.body_length, "THROTTLED") &&
              contains(response.body, response.body_length, "100 ticks"),
          "management panel publishes the existing lockout window");
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              system,
              sizeof(system) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              APPLIANCE_ADMIN_AUTH_COOLDOWN_TICKS,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED,
          "management panel serves again after the lockout expires");
}

static void test_session_expiry(void)
{
    struct appliance_production_auth auth;
    struct appliance_management_panel panel;
    struct appliance_management_panel_response response;
    struct appliance_admin_auth_credential credential =
        make_credential(0x51U);
    struct appliance_admin_auth_proof proof = make_proof(0x51U);
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t bearer[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t bearer_length;
    uint32_t session_id = 0U;
    static const uint8_t system[] = "/panel/system";

    memset(&auth, 0, sizeof(auth));
    memset(&panel, 0, sizeof(panel));
    memset(&response, 0, sizeof(response));
    fill(token, 0x52U, sizeof(token));
    bearer_length = make_bearer(bearer, token);
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
          appliance_production_auth_publish_credential(
              &auth, &credential) == APPLIANCE_PRODUCTION_AUTH_OK &&
          appliance_management_panel_init(&panel, &auth) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel expiry fixture initializes");
    CHECK(appliance_management_panel_login(
              &panel,
              &proof,
              token,
              10U,
              5U,
              &session_id) == APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel issues a bounded session");
    CHECK(appliance_management_panel_dispatch(
              &panel,
              HTTP_METHOD_GET,
              system,
              sizeof(system) - 1U,
              bearer,
              bearer_length,
              14U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED,
          "management panel serves inside the session lifetime");
    CHECK(appliance_management_panel_dispatch(
              &panel,
              HTTP_METHOD_GET,
              system,
              sizeof(system) - 1U,
              bearer,
              bearer_length,
              15U,
              NULL,
              NULL,
              &response) ==
              APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED,
          "management panel refuses an expired session");
    CHECK(appliance_management_panel_status_string(
              APPLIANCE_MANAGEMENT_PANEL_OK) != NULL &&
              appliance_management_panel_status_string(
                  APPLIANCE_MANAGEMENT_PANEL_THROTTLED) != NULL &&
              appliance_management_panel_status_string(
                  (enum appliance_management_panel_status)99U) == NULL,
          "management panel status strings are exact");
}

static void test_licenses_page(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_web_ui_licenses licenses;
    static const uint8_t licenses_target[] = "/panel/about/licenses";

    CHECK(panel_fixture_init(&fixture),
          "management panel licenses fixture signs in");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED &&
              contains(response.body, response.body_length, "SIGNED OUT"),
          "management panel requires a session for the licenses page");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              response.content_type == HTTP_CONTENT_TYPE_HTML_UTF8 &&
              contains(response.body,
                       response.body_length,
                       "<h2>About and licenses</h2>") &&
              contains(response.body, response.body_length, "UNAVAILABLE"),
          "management panel serves the licenses page without an owner");

    memset(&licenses, 0, sizeof(licenses));
    licenses.declared = true;
    licenses.payload_complete = false;
    licenses.reason = "<script>alert(1)</script>";
    licenses.manifest = "LICENSES.sha256";
    licenses.payload_root = "licenses";
    licenses.declared_count = 4U;
    licenses.shipped_count = 2U;
    CHECK(appliance_management_panel_set_licenses(&fixture.panel, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "DEGRADED") &&
              contains(response.body, response.body_length, "Reason: ") &&
              !contains(response.body, response.body_length, "<script>") &&
              contains(response.body,
                       response.body_length,
                       "&lt;script&gt;"),
          "management panel publishes the licenses owner state and escapes it");

    CHECK(appliance_management_panel_publish_declared_licenses(
              &fixture.panel) == APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "AVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "AGPL-3.0-only OR LicenseRef-HFOC-Commercial") &&
              contains(response.body, response.body_length, "mbedTLS") &&
              contains(response.body, response.body_length, "Apache-2.0") &&
              contains(response.body, response.body_length, "BSD-3-Clause") &&
              contains(response.body, response.body_length, "BSD-2-Clause") &&
              contains(response.body,
                       response.body_length,
                       "notices/mbedtls-Apache-2.0-election.txt"),
          "management panel serves the declared license inventory with the "
          "mbedTLS election");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_UNSUPPORTED,
              licenses_target,
              sizeof(licenses_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) ==
              APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED &&
              response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "management panel refuses a method other than GET or HEAD on the "
          "licenses page");

    CHECK(appliance_management_panel_clear_licenses(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "UNAVAILABLE"),
          "management panel returns the licenses page to UNAVAILABLE when the "
          "owner is cleared");
}

/*
 * ITEM-445 step 2 / ITEM-506: the browser sign-in flow. The fixture binds a
 * credential boundary that accepts one literal login and password and issues
 * the fixture session through the same administrator boundary, exactly as the
 * production adapter binds the enrollment owner.  Both halves of the account
 * are checked, so a panel that stopped passing the username fails here.
 */
#define PANEL_BROWSER_LOGIN "admin"
#define PANEL_BROWSER_PASSWORD "correct horse battery"
#define PANEL_BROWSER_PASSWORD_LENGTH 21U

static enum appliance_management_panel_credential_status
panel_browser_authenticate(
    void *context,
    const uint8_t *user,
    uint32_t user_length,
    const uint8_t *secret,
    uint32_t secret_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    struct panel_fixture *fixture = context;
    struct appliance_admin_auth_proof proof = make_proof(0x42U);
    static const char fixture_login[] = PANEL_BROWSER_LOGIN;

    if (fixture == NULL || user == NULL || secret == NULL ||
        token_out == NULL || session_id_out == NULL ||
        user_length != sizeof(PANEL_BROWSER_LOGIN) - 1U ||
        memcmp(user, fixture_login, user_length) != 0 ||
        secret_length != PANEL_BROWSER_PASSWORD_LENGTH ||
        memcmp(secret, PANEL_BROWSER_PASSWORD,
               PANEL_BROWSER_PASSWORD_LENGTH) != 0) {
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED;
    }
    if (appliance_admin_auth_authenticate_local(
            &fixture->auth.admin,
            &proof,
            fixture->token,
            now_tick,
            lifetime_ticks,
            session_id_out) != APPLIANCE_ADMIN_AUTH_OK) {
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_UNAVAILABLE;
    }
    memcpy(token_out, fixture->token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK;
}

static void test_browser_targets(void)
{
    enum appliance_management_web_ui_section section =
        APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;

    CHECK(appliance_management_panel_target_kind(
              (const uint8_t *)"/", 1U, &section) ==
              APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
              appliance_management_panel_target_kind(
                  (const uint8_t *)"/panel/logout",
                  HTTP_POLICY_SIGN_OUT_TARGET_LENGTH, &section) ==
                  APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT &&
              !appliance_management_panel_target(
                  (const uint8_t *)"/", 1U, &section) &&
              !appliance_management_panel_target(
                  (const uint8_t *)"/panel/logout",
                  HTTP_POLICY_SIGN_OUT_TARGET_LENGTH, &section),
          "management panel classifies the two browser targets without "
          "publishing either as a section");
    CHECK(appliance_management_panel_target_kind(
              (const uint8_t *)"/panel/logout/", 14U, &section) ==
              APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE &&
              appliance_management_panel_target_kind(
                  (const uint8_t *)"/panel/logout?x=1", 17U, &section) ==
                  APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE &&
              appliance_management_panel_target_kind(
                  (const uint8_t *)"/Panel/logout", 13U, &section) ==
                  APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE,
          "management panel refuses a sign-out target that is not exact");
}

static void test_browser_sign_in(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_credential credential;
    struct appliance_management_panel_response response;
    struct appliance_management_panel_request request;
    static const uint8_t sign_in_target[] = "/";
    static const uint8_t sign_out_target[] = "/panel/logout";
    static const uint8_t dashboard[] = "/panel";
    static const uint8_t content_type[] =
        "application/x-www-form-urlencoded";
    static const uint8_t good_body[] =
        "username=admin&password=correct+horse+battery";
    static const uint8_t wrong_body[] =
        "username=admin&password=wrong";
    static const uint8_t missing_body[] = "username=admin";
    uint8_t cookie[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
    uint32_t cookie_length = 0U;

    CHECK(panel_fixture_init(&fixture),
          "management panel browser fixture signs in");
    memset(&credential, 0, sizeof(credential));
    credential.context = &fixture;
    credential.authenticate = panel_browser_authenticate;
    CHECK(appliance_management_panel_set_credential(
              &fixture.panel, &credential) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel binds the browser credential boundary");

    /* The form posts, and the answer is the one session cookie. */
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = good_body;
    request.body_length = (uint32_t)(sizeof(good_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_303_SEE_OTHER &&
              response.set_cookie != NULL &&
              response.set_cookie_length != 0U &&
              response.set_cookie_length <=
                  HTTP_RESPONSE_SET_COOKIE_MAX &&
              response.location != NULL &&
              response.location_length ==
                  APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH &&
              memcmp(response.location, "/panel",
                     APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH) ==
                  0,
          "management panel posts the form and answers 303 with a cookie");
    CHECK(response.set_cookie != NULL &&
              contains(response.set_cookie, response.set_cookie_length,
                       "hfoc_session=") &&
              contains(response.set_cookie, response.set_cookie_length,
                       "; HttpOnly; Secure; SameSite=Strict; Path=/; "
                       "Max-Age=900"),
          "management panel sets one HttpOnly Secure SameSite cookie");
    CHECK(appliance_management_panel_sign_in_count(&fixture.panel) == 1U,
          "management panel counts the browser sign-in");
    if (response.set_cookie_length != 0U) {
        memcpy(cookie, response.set_cookie, response.set_cookie_length);
        cookie_length = response.set_cookie_length;
    }

    /* The section accepts that cookie in place of the bearer header. */
    memset(&request, 0, sizeof(request));
    request.cookie_header = cookie;
    request.cookie_header_length = cookie_length;
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              dashboard,
              sizeof(dashboard) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body, response.body_length, "Dashboard"),
          "management panel accepts the session cookie for a section");

    /* A cookie that cannot be decoded honestly is not guessed at. */
    {
        static const uint8_t short_cookie[] =
            "hfoc_session=00112233";
        static const uint8_t duplicated_cookie[] =
            "hfoc_session=00112233445566778899aabbccddeeff0011223344556677"
            "8899aabbccddeeff; hfoc_session="
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

        memset(&request, 0, sizeof(request));
        request.cookie_header = short_cookie;
        request.cookie_header_length =
            (uint32_t)(sizeof(short_cookie) - 1U);
        memset(&response, 0, sizeof(response));
        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  NULL,
                  0U,
                  5U,
                  NULL,
                  &request,
                  &response) ==
                  APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
                  response.status == HTTP_STATUS_401_UNAUTHORIZED &&
                  response.set_cookie_length == 0U,
              "management panel refuses a short cookie without guessing");
        memset(&request, 0, sizeof(request));
        request.cookie_header = duplicated_cookie;
        request.cookie_header_length =
            (uint32_t)(sizeof(duplicated_cookie) - 1U);
        memset(&response, 0, sizeof(response));
        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  NULL,
                  0U,
                  5U,
                  NULL,
                  &request,
                  &response) ==
                  APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED,
              "management panel refuses two session cookies in one header");
    }

    /* No cookie at all is the published sign-in document. */
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              dashboard,
              sizeof(dashboard) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED &&
              contains(response.body, response.body_length, "SIGNED OUT") &&
              contains(response.body, response.body_length,
                       "<form method=\"post\" action=\"/\">") &&
              response.set_cookie_length == 0U,
          "management panel publishes the form to an unsigned browser");

    /* A wrong secret is denied and sets no cookie. */
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = wrong_body;
    request.body_length = (uint32_t)(sizeof(wrong_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_DENIED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED &&
              response.set_cookie_length == 0U &&
              contains(response.body, response.body_length, "AUTH_DENIED"),
          "management panel denies a wrong secret without a cookie");

    /* A partial form, an unbound content type and no body are refusals. */
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = missing_body;
    request.body_length = (uint32_t)(sizeof(missing_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_DENIED &&
              response.status == HTTP_STATUS_400_BAD_REQUEST &&
              response.set_cookie_length == 0U &&
              contains(response.body, response.body_length,
                       "FORM_MISSING_FIELD"),
          "management panel refuses a partial form");
    memset(&request, 0, sizeof(request));
    request.body = good_body;
    request.body_length = (uint32_t)(sizeof(good_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_DENIED &&
              response.status == HTTP_STATUS_400_BAD_REQUEST &&
              contains(response.body, response.body_length,
                       "FORM_CONTENT_TYPE"),
          "management panel refuses a form without its content type");
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_DENIED &&
              response.status == HTTP_STATUS_400_BAD_REQUEST &&
              contains(response.body, response.body_length, "FORM_MISSING"),
          "management panel refuses a sign-in with no form at all");

    /* A GET of `/` is not a panel request: the public document owns it. */
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL,
          "management panel leaves the public root document to the router");

    /* Sign-out expires the cookie and sends the browser back. */
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_out_target,
              sizeof(sign_out_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_303_SEE_OTHER &&
              response.set_cookie != NULL &&
              contains(response.set_cookie, response.set_cookie_length,
                       "hfoc_session=; HttpOnly; Secure; SameSite=Strict; "
                       "Path=/; Max-Age=0") &&
              response.location != NULL &&
              response.location_length ==
                  APPLIANCE_MANAGEMENT_PANEL_SIGN_OUT_LOCATION_LENGTH &&
              response.location[0] == (uint8_t)'/' &&
              appliance_management_panel_sign_out_count(&fixture.panel) ==
                  1U,
          "management panel sign-out expires the cookie immediately");
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              sign_out_target,
              sizeof(sign_out_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              NULL,
              &response) ==
              APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED &&
              response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "management panel refuses a GET on the sign-out target");

    /* The existing lockout is published, not cleared. */
    fixture.auth.admin.locked_until_tick = 200U;
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = good_body;
    request.body_length = (uint32_t)(sizeof(good_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              sign_in_target,
              sizeof(sign_in_target) - 1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_THROTTLED &&
              response.status == HTTP_STATUS_429_TOO_MANY_REQUESTS &&
              response.set_cookie_length == 0U &&
              contains(response.body, response.body_length, "THROTTLED"),
          "management panel publishes the existing lockout on the form");
}

static void test_browser_unbound_credential(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_panel_request request;
    static const uint8_t content_type[] =
        "application/x-www-form-urlencoded";
    static const uint8_t body[] = "username=admin&password=whatever";

    CHECK(panel_fixture_init(&fixture),
          "management panel unbound fixture signs in");
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = body;
    request.body_length = (uint32_t)(sizeof(body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              (const uint8_t *)"/",
              1U,
              NULL,
              0U,
              5U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE &&
              response.status == HTTP_STATUS_503_SERVICE_UNAVAILABLE &&
              response.set_cookie_length == 0U,
          "management panel answers UNAVAILABLE without a credential owner");
}

/*
 * ITEM-509 increment 10: the dispatcher renders the page in the language the
 * system record holds.
 *
 * The record is the one the System page publishes; `ru` must reach the
 * renderer (navigation, heading and `lang`), and a record that holds something
 * other than `en`/`ru` must fall back to the default English document instead
 * of inventing a language.  The panel's stored views stay untouched: the
 * language is applied to a copy at dispatch time.
 */
static void test_language_record(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_system system;
    static const uint8_t dashboard[] = "/panel";

    CHECK(panel_fixture_init(&fixture),
          "management panel language fixture signs in");

    memset(&view, 0, sizeof(view));
    view.hostname = "hfoc-appliance";
    CHECK(appliance_management_panel_set_view(&fixture.panel, &view) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language fixture publishes a view");

    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "hfoc-appliance";
    system.saved = "hfoc-appliance";
    system.language_active = "ru";
    system.language_saved = "ru";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language fixture publishes the system record");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              dashboard,
              sizeof(dashboard) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body,
                       response.body_length,
                       "<html lang=\"ru\">") &&
              contains(response.body, response.body_length, "<h2>Обзор</h2>") &&
              contains(response.body, response.body_length, ">Сеть</a>") &&
              contains(response.body, response.body_length, ">Справка</a>"),
          "ITEM-509: the panel renders the language the system record holds");

    /*
     * A record that is neither `en` nor `ru` is not a language the catalog
     * has: the page is the default English document, not a half-translated
     * one and not a guess.
     */
    system.language_active = "de";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body,
                       response.body_length,
                       "<html lang=\"en\">") &&
              contains(response.body,
                       response.body_length,
                       "<h2>Dashboard</h2>"),
          "ITEM-509: a record that is not en/ru renders the default English "
          "document");

    /* The verified English path is byte-for-byte the pre-increment document. */
    system.language_active = "en";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body,
                       response.body_length,
                       "<html lang=\"en\">") &&
              contains(response.body,
                       response.body_length,
                       "<h2>Dashboard</h2>"),
          "ITEM-509: the English record renders the English document");
}

/*
 * ITEM-513: the language is the browser's, not the machine's.
 *
 * The owner's decision (2026-10-10) is a per-browser switch: the nav's `EN |
 * RU` buttons post the one field `language=en|ru` to `/panel/language`, whose
 * answer is the one `hfoc_lang` cookie and a 303; the shared
 * `v1:system.language` record stays the machine default and is never written
 * by this path.  This proof drives the real dispatcher: a valid cookie wins
 * over the record in both directions, a cookie the reader refuses is a named
 * refusal answered by the record's language plus a clearing cookie, the switch
 * works for a browser session with no bearer header, a malformed body changes
 * nothing, and the page keeps the record and the browser's choice distinct.
 */
static void test_language_switch_and_cookie(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_panel_request request;
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_system system;
    static const uint8_t dashboard[] = "/panel";
    static const uint8_t system_target[] = "/panel/system";
    static const uint8_t switch_target[] = "/panel/language";
    static const uint8_t content_type[] =
        "application/x-www-form-urlencoded";
    static const uint8_t ru_body[] = "language=ru";
    static const uint8_t bad_body[] = "language=de";
    static const uint8_t two_field_body[] = "language=ru&hostname=x";
    uint8_t session_cookie[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX];
    uint32_t session_cookie_length = 0U;
    uint8_t cookies[APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_MAX +
                    APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_MAX + 4U];
    uint32_t cookies_length = 0U;

    CHECK(panel_fixture_init(&fixture),
          "management panel language switch fixture signs in");
    memset(&view, 0, sizeof(view));
    view.hostname = "hfoc-appliance";
    CHECK(appliance_management_panel_set_view(&fixture.panel, &view) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language switch fixture publishes a view");
    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "hfoc-appliance";
    system.saved = "hfoc-appliance";
    system.language_active = "en";
    system.language_saved = "en";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language switch fixture publishes the English "
          "record");
    CHECK(appliance_panel_login_cookie_set(
              fixture.token,
              APPLIANCE_PANEL_LOGIN_SESSION_SECONDS,
              session_cookie,
              (uint32_t)sizeof(session_cookie),
              &session_cookie_length) == APPLIANCE_PANEL_LOGIN_OK,
          "management panel language switch fixture builds the session "
          "cookie");

    /* 1. A valid Russian cookie beats the English record. */
    memcpy(cookies, session_cookie, session_cookie_length);
    memcpy(cookies + session_cookie_length, "; hfoc_lang=ru", 14U);
    cookies_length = session_cookie_length + 14U;
    memset(&request, 0, sizeof(request));
    request.cookie_header = cookies;
    request.cookie_header_length = cookies_length;
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              dashboard,
              sizeof(dashboard) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body,
                       response.body_length,
                       "<html lang=\"ru\">") &&
              contains(response.body, response.body_length, "<h2>Обзор</h2>"),
          "ITEM-513: a valid language cookie wins over the system record");

    /* 2. A valid English cookie beats the Russian record. */
    system.language_active = "ru";
    system.language_saved = "ru";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language switch fixture republishes the Russian "
          "record");
    {
        static const uint8_t english_cookie[] = "hfoc_lang=en";
        uint8_t combined[APPLIANCE_PANEL_LANGUAGE_COOKIE_VALUE_MAX + 32U];
        uint32_t combined_length = 0U;

        memcpy(combined, english_cookie, sizeof(english_cookie) - 1U);
        combined_length = (uint32_t)(sizeof(english_cookie) - 1U);
        memcpy(combined + combined_length, "; ", 2U);
        combined_length += 2U;
        memcpy(combined + combined_length, session_cookie,
               session_cookie_length);
        combined_length += session_cookie_length;
        memset(&request, 0, sizeof(request));
        request.cookie_header = combined;
        request.cookie_header_length = combined_length;
        memset(&response, 0, sizeof(response));
        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  dashboard,
                  sizeof(dashboard) - 1U,
                  NULL,
                  0U,
                  1U,
                  NULL,
                  &request,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
                  response.status == HTTP_STATUS_200_OK &&
                  contains(response.body,
                           response.body_length,
                           "<html lang=\"en\">") &&
                  contains(response.body,
                           response.body_length,
                           "<h2>Dashboard</h2>"),
              "ITEM-513: a valid English cookie beats the Russian record");
        CHECK(appliance_management_panel_language_cookie_refused_count(
                  &fixture.panel) == 0U,
              "ITEM-513: a valid cookie is not counted as refused");
    }

    /* 3. A refused cookie: the record renders whole, and the browser is told
     *    to drop the value by name. */
    {
        static const uint8_t garbage_cookie[] =
            "hfoc_session=00112233445566778899aabbccddeeff0011223344556677"
            "8899aabbccddeeff; hfoc_lang=xx";

        memset(&request, 0, sizeof(request));
        request.cookie_header = garbage_cookie;
        request.cookie_header_length =
            (uint32_t)(sizeof(garbage_cookie) - 1U);
        memset(&response, 0, sizeof(response));
        CHECK(appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  system_target,
                  sizeof(system_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
                  &request,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
                  response.status == HTTP_STATUS_200_OK &&
                  contains(response.body,
                           response.body_length,
                           "<html lang=\"ru\">") &&
                  contains(response.body,
                           response.body_length,
                           "<h2>Система</h2>") &&
                  contains(response.body, response.body_length, ">ru</code>") &&
                  response.set_cookie != NULL &&
                  response.set_cookie_length != 0U &&
                  contains(response.set_cookie,
                           response.set_cookie_length,
                           "hfoc_lang=;") &&
                  contains(response.set_cookie,
                           response.set_cookie_length,
                           "Max-Age=0"),
              "ITEM-513: a refused cookie renders the record's language and "
              "is cleared by name");
        CHECK(appliance_management_panel_language_cookie_refused_count(
                  &fixture.panel) == 1U &&
                  appliance_management_panel_language_count(&fixture.panel) ==
                      0U,
              "ITEM-513: the refused cookie is counted, the switch is not");
    }

    /* 4. The one-click switch itself: session cookie, one field, no token. */
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = ru_body;
    request.body_length = (uint32_t)(sizeof(ru_body) - 1U);
    request.cookie_header = session_cookie;
    request.cookie_header_length = session_cookie_length;
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              switch_target,
              sizeof(switch_target) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_303_SEE_OTHER &&
              response.location != NULL &&
              response.location_length ==
                  APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION_LENGTH &&
              response.set_cookie != NULL &&
              response.set_cookie_length ==
                  (uint32_t)strlen("hfoc_lang=ru; HttpOnly; Secure; "
                                   "SameSite=Strict; Path=/") &&
              memcmp(response.set_cookie,
                     "hfoc_lang=ru; HttpOnly; Secure; SameSite=Strict; "
                     "Path=/",
                     response.set_cookie_length) == 0,
          "ITEM-513: the switch answers one 303 and one language cookie");
    CHECK(appliance_management_panel_language_count(&fixture.panel) == 1U,
          "ITEM-513: the switch is counted");

    /* 5. A body that is not the drawn one changes nothing. */
    {
        static const uint8_t *const bad_bodies[] = {
            bad_body,
            two_field_body,
        };
        static const uint32_t bad_lengths[] = {
            sizeof(bad_body) - 1U,
            sizeof(two_field_body) - 1U,
        };
        size_t index;

        for (index = 0U; index < 2U; ++index) {
            memset(&request, 0, sizeof(request));
            request.content_type = content_type;
            request.content_type_length =
                (uint32_t)(sizeof(content_type) - 1U);
            request.body = bad_bodies[index];
            request.body_length = bad_lengths[index];
            request.cookie_header = session_cookie;
            request.cookie_header_length = session_cookie_length;
            memset(&response, 0, sizeof(response));
            CHECK(appliance_management_panel_dispatch(
                      &fixture.panel,
                      HTTP_METHOD_POST,
                      switch_target,
                      sizeof(switch_target) - 1U,
                      NULL,
                      0U,
                      1U,
                      NULL,
                      &request,
                      &response) == APPLIANCE_MANAGEMENT_PANEL_DENIED &&
                      response.status == HTTP_STATUS_400_BAD_REQUEST &&
                      contains(response.body,
                               response.body_length,
                               "PANEL_LANGUAGE_BAD_VALUE") &&
                      response.set_cookie_length == 0U,
                  "ITEM-513: a malformed switch body is a named refusal and "
                  "sets no cookie");
        }
        CHECK(appliance_management_panel_language_count(&fixture.panel) == 1U,
              "ITEM-513: a refused switch is not counted as a switch");
    }

    /* 6. The switch is POST only, and it needs a session. */
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              switch_target,
              sizeof(switch_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED &&
              response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "ITEM-513: a GET of the switch target is refused by name");
    memset(&request, 0, sizeof(request));
    request.content_type = content_type;
    request.content_type_length = (uint32_t)(sizeof(content_type) - 1U);
    request.body = ru_body;
    request.body_length = (uint32_t)(sizeof(ru_body) - 1U);
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_POST,
              switch_target,
              sizeof(switch_target) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED &&
              response.set_cookie_length == 0U,
          "ITEM-513: an unsigned switch is refused and sets no cookie");

    /* 7. The record and the browser's choice stay distinct on the page. */
    system.language_active = "en";
    system.language_saved = "en";
    CHECK(appliance_management_panel_set_system(&fixture.panel, &system) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "management panel language switch fixture restores the English "
          "record");
    memcpy(cookies, session_cookie, session_cookie_length);
    memcpy(cookies + session_cookie_length, "; hfoc_lang=ru", 14U);
    cookies_length = session_cookie_length + 14U;
    memset(&request, 0, sizeof(request));
    request.cookie_header = cookies;
    request.cookie_header_length = cookies_length;
    memset(&response, 0, sizeof(response));
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              system_target,
              sizeof(system_target) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              &request,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, ">en</code>") &&
              contains(response.body, response.body_length, ">Сеть</a>"),
          "ITEM-513: the page keeps the record's language and the browser's "
          "choice distinct");
}

/*
 * ITEM-657: every mutation that has a panel page must be able to show its
 * two-phase token.  The Certificates page renders the confirmation block, but
 * the serving path's pending list omitted CERTIFICATE_NAME, so a staged import,
 * delete or default answered 303 back to a page that could never show the
 * token - the operator saw no confirmation step and the saved table stayed
 * empty.  The list lives in one helper so the page and the gate cannot drift.
 */
static void test_certificate_name_confirmation_gate(void)
{
    struct panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_web_ui_confirmation confirmation;
    static const uint8_t certificates[] = "/panel/certificates";

    CHECK(appliance_management_authorization_mutation_has_panel_confirmation(
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME),
          "ITEM-657: the certificate-name mutation shows its panel token");
    CHECK(appliance_management_authorization_mutation_has_panel_confirmation(
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL) &&
              appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE),
          "ITEM-657: every panel mutation keeps its confirmation token");
    CHECK(!appliance_management_authorization_mutation_has_panel_confirmation(
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE) &&
              !appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION) &&
              !appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT) &&
              !appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION) &&
              !appliance_management_authorization_mutation_has_panel_confirmation(
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET),
          "ITEM-657: a mutation without a page is not given a token block");

    CHECK(panel_fixture_init(&fixture),
          "ITEM-657: management panel view fixture signs in");
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(657657);
    confirmation.mutation = "CERTIFICATE_NAME";
    confirmation.remaining_ticks = 120000U;
    confirmation.single_use = true;
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              certificates,
              sizeof(certificates) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              &confirmation,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length,
                       "CERTIFICATE_NAME token 657657") &&
              contains(response.body, response.body_length,
                       "CERTIFICATE_NAME"),
          "ITEM-657: the Certificates page renders the mutation's token");
}

int main(void)
{
    test_targets();
    test_browser_targets();
    test_browser_sign_in();
    test_browser_unbound_credential();
    test_lifecycle();
    test_unavailable_without_credential();
    test_session_flow();
    test_confirmation_and_escaping();
    /* ITEM-657: the certificate-name page must show its own token. */
    test_certificate_name_confirmation_gate();
    test_throttling_and_expiry();
    test_session_expiry();
    test_licenses_page();
    /* ITEM-509 increment 10: the page follows the system record's language. */
    test_language_record();
    /* ITEM-513: the per-browser language switch and its cookie priority. */
    test_language_switch_and_cookie();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance management panel host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Appliance management panel host test: ALL OK\n");
    return 0;
}

#else

typedef int appliance_management_panel_unused;

#endif
