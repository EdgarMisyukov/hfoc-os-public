#include "appliance_admin_credential.h"
#include "appliance_certificate_name_store.h"
#include "appliance_language.h"
#include "appliance_management_web_ui.h"
#include "appliance_persistent_config.h"
#include "appliance_production_panel_view.h"
#include "appliance_route_table.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static unsigned int count_text(const char *haystack, const char *needle);

/*
 * ITEM-647: every visible form control must be nested in a <label>, which is
 * the association a browser exposes to the operator.  A hidden field needs no
 * label: it carries a protocol value, not something the operator types.  The
 * audit walks the rendered document instead of a fixture list, so a control
 * added later cannot silently ship without its name.
 */
static bool text_at(const char *text, const char *needle)
{
    size_t needle_length = strlen(needle);

    return strncmp(text, needle, needle_length) == 0;
}

static bool tag_contains(const char *tag, size_t tag_length, const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0U || needle_length > tag_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= tag_length; ++index) {
        if (memcmp(tag + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void check_visible_controls_labelled(const char *page, const char *name)
{
    size_t index = 0U;
    unsigned int label_depth = 0U;

    while (page[index] != '\0') {
        if (text_at(page + index, "<label")) {
            ++label_depth;
            index += strlen("<label");
            continue;
        }
        if (text_at(page + index, "</label>")) {
            if (label_depth == 0U) {
                fprintf(stderr, "FAIL: %s closes an unopened label\n", name);
                ++failures;
            } else {
                --label_depth;
            }
            index += strlen("</label>");
            continue;
        }
        if (text_at(page + index, "<input") ||
            text_at(page + index, "<textarea") ||
            text_at(page + index, "<select")) {
            const char *end = strchr(page + index, '>');
            const char *control = page + index;
            size_t control_length;
            bool hidden;

            if (end == NULL) {
                fprintf(stderr, "FAIL: %s has an unterminated control\n", name);
                ++failures;
                return;
            }
            control_length = (size_t)(end - control) + 1U;
            hidden = text_at(control, "<input") &&
                     tag_contains(control, control_length,
                                  "type=\"hidden\"");
            if (!hidden && label_depth == 0U) {
                fprintf(stderr,
                        "FAIL: %s has a visible control without a label: "
                        "%.*s\n",
                        name, (int)control_length, control);
                ++failures;
            }
            index = (size_t)(end - page) + 1U;
            continue;
        }
        ++index;
    }
    if (label_depth != 0U) {
        fprintf(stderr, "FAIL: %s leaves %u label(s) open\n", name,
                label_depth);
        ++failures;
    }
}

/*
 * Measured asset budget. The recorded budget lives in the header and in
 * `docs/hfoc-management-web-ui.md`; this fixture measures every served page
 * and fails when a page grows past its recorded budget or when the recorded
 * table stops matching the measurement.
 */
static const char *const page_names[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT] = {
    "login",
    "dashboard",
    "network",
    "services",
    "storage",
    "system",
    "security",
    "diagnostics",
    "updates",
    "recovery",
    "routing",
    "account",
    "certificates",
    /* ITEM-509 increment 8: the help index and its topic pages. */
    "help",
    "licenses",
    "acme",
    /* ITEM-516: the management-gate page. */
    "gate",
};

static uint8_t measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT]
                       [APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];

static const char *const hostile_values[] = {
    "<script>alert(1)</script>",
    "\"><img src=x onerror=alert(1)>",
    "&amp;&lt;",
    "a'b\"c<d>e&f",
    "</main><form action=\"/panel\">",
    "hidden\abell",
    "line\nbreak",
};

static void test_section_strings(void)
{
    static const char *expected[] = {
        "Dashboard",
        "Network",
        "Services",
        "Storage",
        "System",
        "Security",
        "Diagnostics",
        "Updates",
        "Recovery",
        "Routing",
        "Account",
        "Certificates",
        "Help",
    };
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        const char *section = appliance_management_web_ui_section_string(
            (enum appliance_management_web_ui_section)index);

        CHECK(section != NULL &&
                  strcmp(section, expected[index]) == 0,
              "management web UI section string is exact");
    }
    CHECK(appliance_management_web_ui_section_string(
              (enum appliance_management_web_ui_section)99U) == NULL,
          "undefined management web UI section is rejected");
}

static void test_status_strings(void)
{
    static const char *expected[] = {
        "APPLIANCE_MANAGEMENT_WEB_UI_OK",
        "APPLIANCE_MANAGEMENT_WEB_UI_NULL",
        "APPLIANCE_MANAGEMENT_WEB_UI_BAD_STATE",
        "APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW",
        "APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        const char *status = appliance_management_web_ui_status_string(
            (enum appliance_management_web_ui_status)index);

        CHECK(status != NULL &&
                  strcmp(status, expected[index]) == 0,
              "management web UI status string is exact");
    }
    CHECK(appliance_management_web_ui_status_string(
              (enum appliance_management_web_ui_status)99U) == NULL,
          "undefined management web UI status is rejected");
}

static void test_section_ownership(void)
{
    static const bool expect_owner[] = {
        true, true, true, false, true, false,
        false, true, true, false, false,
        /* ITEM-507 increment 5: Certificates publishes no read owner. */
        false,
        /* ITEM-509 increment 8: Help publishes no read owner either. */
        false,
    };
    static const bool expect_confirmable[] = {
        /* ITEM-505 increment 3: System confirms the hostname mutation. */
        false, true, true, false, true, true,
        /* ITEM-506 increment 6: Account confirms the credential change. */
        false, true, true, true, true,
        /* ITEM-507 increment 5: Certificates confirms the name mutation. */
        true,
        /* ITEM-509 increment 8: Help has no mutation surface. */
        false,
    };
    static const enum appliance_management_web_ui_section_state
        expect_state[] = {
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED,
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED,
            /* ITEM-509 increment 8: the built-in help catalog is available. */
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
        };
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        enum appliance_management_web_ui_section section =
            (enum appliance_management_web_ui_section)index;
        const char *owner =
            appliance_management_web_ui_section_owner(section);

        CHECK((owner != NULL) == expect_owner[index],
              "management web UI section ownership is exact");
        if (owner != NULL) {
            CHECK(strncmp(owner, "/api/v1", 7U) == 0,
                  "management web UI section owner is a management API path");
        }
        CHECK(appliance_management_web_ui_section_state(section) ==
                  expect_state[index],
              "management web UI section state is exact");
        CHECK(appliance_management_web_ui_section_confirmable(section) ==
                  expect_confirmable[index],
              "management web UI section confirmation expectation is exact");
    }
    CHECK(appliance_management_web_ui_section_owner(
              (enum appliance_management_web_ui_section)99U) == NULL,
          "undefined management web UI section has no owner");
    CHECK(appliance_management_web_ui_section_state(
              (enum appliance_management_web_ui_section)99U) ==
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE,
          "undefined management web UI section is unavailable");
}

static void test_escape(void)
{
    char output[APPLIANCE_MANAGEMENT_WEB_UI_ESCAPE_MAX + 1U];
    size_t output_length = 0U;
    size_t index;

    struct {
        const char *input;
        const char *expected;
    } cases[] = {
        { "<script>", "&lt;script&gt;" },
        { "\"><img>", "&quot;&gt;&lt;img&gt;" },
        { "&amp;", "&amp;amp;" },
        { "a'b", "a&#39;b" },
        { "plain-31", "plain-31" },
        { "control\a", "control&#7;" },
    };

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        output_length = 0U;
        memset(output, 0, sizeof(output));
        CHECK(appliance_management_web_ui_escape(
                  cases[index].input,
                  strlen(cases[index].input),
                  output,
                  sizeof(output),
                  &output_length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strcmp(output, cases[index].expected) == 0 &&
              output_length == strlen(cases[index].expected),
              "management web UI escapes one hostile value");
    }

    CHECK(appliance_management_web_ui_escape(
              NULL, 0U, output, sizeof(output), &output_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL &&
          appliance_management_web_ui_escape(
              "x", 1U, NULL, sizeof(output), &output_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL &&
          appliance_management_web_ui_escape(
              "x", 1U, output, sizeof(output), NULL) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL,
          "management web UI escape rejects null inputs");
    output_length = 0U;
    CHECK(appliance_management_web_ui_escape(
              "<", 1U, output, 4U, &output_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW,
          "management web UI escape reports overflow");
    output_length = 0U;
    CHECK(appliance_management_web_ui_escape(
              "", 0U, output, 1U, &output_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              output[0] == '\0' && output_length == 0U,
          "management web UI escape renders the empty value");
}

static void test_shell_body(void)
{
    const uint8_t *body = appliance_management_web_ui_shell_body();
    const uint32_t length = appliance_management_web_ui_shell_length();
    char rendered[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t rendered_length = 0U;

    CHECK(body != NULL &&
              length > 0U &&
              length == (uint32_t)strlen((const char *)body) &&
              length <= APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN &&
              length + 1U < APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
          "management web UI shell body is bounded and recorded");
    CHECK(appliance_management_web_ui_render(
              rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              rendered_length == length &&
              memcmp(rendered, body, length) == 0 &&
              rendered[length] == '\0',
          "management web UI anonymous render matches the shell body");
    CHECK(appliance_management_web_ui_render(
              rendered, (size_t)length, &rendered_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW,
          "management web UI render requires NUL capacity");
    CHECK(appliance_management_web_ui_render(
              NULL, sizeof(rendered), &rendered_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL &&
          appliance_management_web_ui_render(
              rendered, sizeof(rendered), NULL) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL,
          "management web UI render rejects null inputs");
    /*
     * ITEM-454/ITEM-506: the owner reached this page in a browser and could
     * not sign in, because nothing on it said where the account is set.  The
     * page has to name the one command, and the removed `admin-provision`
     * placeholder must not survive anywhere in the shell.
     */
    CHECK(strstr((const char *)body,
                 "<p>Set the account on the machine console with "
                 "<code>credentials</code>") != NULL,
          "management web UI shell body tells the operator how to set the "
          "account");
    CHECK(strstr((const char *)body, "admin-provision") == NULL,
          "management web UI shell body no longer names the removed command");
}

static void test_login_page(void)
{
    struct appliance_management_web_ui_session session;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&session, 0, sizeof(session));
    CHECK(appliance_management_web_ui_render_login(
              NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              length == appliance_management_web_ui_shell_length(),
          "management web UI login page renders without a session");
    CHECK(strstr(page, "<h2>Management sign-in</h2>") != NULL &&
              strstr(page, "SIGNED OUT") != NULL &&
              strstr(page, "Dashboard Network Services Storage System "
                           "Security Diagnostics Updates Recovery Routing "
                           "Account")
                  != NULL,
          "management web UI login page publishes the sign-in state");
    CHECK(strstr(page,
                 "<p>Set the account on the machine console with "
                 "<code>credentials</code>") != NULL,
          "management web UI login page tells the operator how to set the "
          "account");
    CHECK(strstr(page, "the same login and password") != NULL,
          "management web UI login page says the form takes that account");

    memset(&session, 0, sizeof(session));
    session.authenticated = true;
    session.session_id = 4U;
    session.principal_id = 1U;
    session.notice = "AUTH_OK";
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "SIGNED IN") != NULL &&
              strstr(page, "session 4") != NULL,
          "management web UI login page publishes an authenticated session");

    memset(&session, 0, sizeof(session));
    session.notice = "AUTH_THROTTLED<script>";
    session.lockout_remaining_ticks = 97U;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "THROTTLED") != NULL &&
              strstr(page, "97") != NULL &&
              strstr(page, "AUTH_THROTTLED&lt;script&gt;") != NULL &&
              strstr(page, "AUTH_THROTTLED<script>") == NULL,
          "management web UI login page escapes the notice and shows the "
          "throttle window");
}

static void fill_view(
    struct appliance_management_web_ui_view *view,
    size_t index)
{
    memset(view, 0, sizeof(*view));
    view->hostname = hostile_values[index % 7U];
    view->mac_address = hostile_values[(index + 1U) % 7U];
    view->service_name = hostile_values[(index + 2U) % 7U];
    view->log_line = hostile_values[(index + 3U) % 7U];
    view->detail = hostile_values[(index + 4U) % 7U];
}

static void test_sections_render(void)
{
    struct appliance_management_web_ui_view view;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t index;
    size_t inner;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        enum appliance_management_web_ui_section section =
            (enum appliance_management_web_ui_section)index;
        const char *name =
            appliance_management_web_ui_section_string(section);
        const char *owner =
            appliance_management_web_ui_section_owner(section);
        size_t length = 0U;

        fill_view(&view, index);
        CHECK(appliance_management_web_ui_render_section(
                  section, &view, NULL, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page),
                  &length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                  strstr(page, "<!doctype html>") == page &&
                  strstr(page, "</html>") != NULL &&
                  strstr(page, "<h2>") != NULL,
              "management web UI section renders a complete document");
        CHECK(length <= APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX - 1U &&
                  strlen(page) == length,
              "management web UI section document is length exact");
        for (inner = 0U; inner < sizeof(hostile_values) /
                                      sizeof(hostile_values[0]); ++inner) {
            CHECK(strstr(page, hostile_values[inner]) == NULL,
                  "management web UI section escapes every hostile value");
        }
        if (section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES &&
            section != APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
            CHECK(strstr(page, "&lt;") != NULL &&
                      strstr(page, "<script>") == NULL,
                  "management web UI section renders escaped markup");
        }
        if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK) {
            /*
             * ITEM-505: the network page carries the form and the two
             * readings, not the generic value dump; with no owner published
             * it must still say so instead of rendering a blank.
             */
            CHECK(strstr(page, "Network configuration") != NULL &&
                      strstr(page, "No network owner is published") != NULL &&
                      strstr(page, "action=\"/panel/network\"") != NULL &&
                      strstr(page, "name=\"mode\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL,
                  "management web UI Network section publishes the network "
                  "form and names the missing owner");
        }
        if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM) {
            /*
             * ITEM-505 increment 3: the System page carries the hostname form
             * and the two readings, not the generic value dump; with no owner
             * published it must still say so instead of rendering a blank.
             */
            CHECK(strstr(page, "System configuration") != NULL &&
                      strstr(page, "No system owner is published") != NULL &&
                      strstr(page, "action=\"/panel/system\"") != NULL &&
                      strstr(page, "name=\"hostname\"") != NULL &&
              strstr(page, "name=\"language\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL,
                  "management web UI System section publishes the hostname "
                  "form and names the missing owner");
        }
        if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY) {
            /*
             * ITEM-503: Security has no read owner; its page publishes the
             * BYO certificate import form instead of the read-side value dump.
             */
            CHECK(strstr(page, "UNAVAILABLE") != NULL &&
                      strstr(page, "certificate import form") != NULL &&
                      strstr(page, "action=\"/panel/security\"") != NULL &&
                      strstr(page, "name=\"certificate\"") != NULL &&
                      strstr(page, "name=\"key\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL,
                  "management web UI Security section publishes the BYO "
                  "certificate import form");
        } else if (section ==
                   APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING) {
            /*
             * ITEM-504: Routing has no read owner either, but it is a
             * mutation-only section with a page of its own: it publishes
             * DEGRADED, names the applied table it was given (none here) and
             * offers the stage and confirmation forms.
             */
            CHECK(strstr(page, "DEGRADED") != NULL &&
                      strstr(page, "mutation-only surface") != NULL &&
                      strstr(page, "No route table is published") != NULL &&
                      strstr(page, "action=\"/panel/routing\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL &&
                      strstr(page, "Confirm") != NULL &&
                      strstr(page, "Cancel") != NULL,
                  "management web UI Routing section publishes the applied "
                  "table, the forms and the DEGRADED state");
        } else if (section ==
                   APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT) {
            /*
             * ITEM-506 increment 6: Account has no read-side owner either, but
             * it is a mutation-only section with a page of its own: it
             * publishes DEGRADED, names the missing owner and offers the
             * four-field change form and the confirmation form.
             */
            CHECK(strstr(page, "DEGRADED") != NULL &&
                      strstr(page, "mutation-only surface") != NULL &&
                      strstr(page, "No account owner is published") != NULL &&
                      strstr(page, "action=\"/panel/account\"") != NULL &&
                      strstr(page, "name=\"current\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL &&
                      strstr(page, "Confirm") != NULL &&
                      strstr(page, "Cancel") != NULL,
                  "management web UI Account section publishes the credential "
                  "form and names the missing owner");
        } else if (section ==
                   APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES) {
            /*
             * ITEM-507 increment 5: Certificates has no read-side owner either
             * and is a mutation-only section with a page of its own: it
             * publishes DEGRADED, names the missing owner and offers the name
             * forms and the confirmation form.
             */
            CHECK(strstr(page, "DEGRADED") != NULL &&
                      strstr(page, "mutation-only surface") != NULL &&
                      strstr(page,
                             "No certificate owner is published") != NULL &&
                      strstr(page,
                             "action=\"/panel/certificates\"") != NULL &&
                      strstr(page, "name=\"certificate\"") != NULL &&
                      strstr(page, "name=\"token\"") != NULL &&
                      strstr(page, "Confirm") != NULL &&
                      strstr(page, "Cancel") != NULL,
                  "management web UI Certificates section publishes the name "
                  "forms and names the missing owner");
        } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
            /*
             * ITEM-509 increment 8: Help has no read owner but it is not an
             * unavailable section either - the catalog it renders is compiled
             * into this build.  Without a help view (the generic render here)
             * the page publishes the named refusal, so "the panel cannot read
             * the language" is never mistaken for "there is no help".
             */
            CHECK(strstr(page, "AVAILABLE") != NULL &&
                      strstr(page, "built-in help catalog") != NULL &&
                      strstr(page, "Help is not available") != NULL &&
                      strstr(page, "LANGUAGE_UNAVAILABLE") != NULL,
                  "management web UI Help section names the built-in catalog "
                  "and refuses without a language");
        } else if (owner == NULL) {
            CHECK(strstr(page, "UNAVAILABLE") != NULL &&
                      strstr(page, "No management API owner") != NULL &&
                      strstr(page, "no read side is published") != NULL,
                  "management web UI unowned section is explicitly "
                  "UNAVAILABLE and names why nothing is published");
        } else if (appliance_management_web_ui_section_state(section) ==
                       APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED) {
            CHECK(strstr(page, "DEGRADED") != NULL &&
                      strstr(page, owner) != NULL &&
                      strstr(page, "mutation-only surface") != NULL,
                  "management web UI mutation-only section is explicitly "
                  "DEGRADED and names why the read state is missing");
        } else {
            CHECK(strstr(page, "AVAILABLE") != NULL &&
                      strstr(page, owner) != NULL,
                  "management web UI owned section names its owner");
        }
        CHECK(strstr(page, name) != NULL,
              "management web UI section document names the section");
    }

    CHECK(appliance_management_web_ui_render_section(
              (enum appliance_management_web_ui_section)99U,
              NULL, NULL, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &index) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION,
          "management web UI render rejects an unknown section");
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              NULL, NULL, NULL, NULL, NULL, NULL,NULL, NULL, NULL, sizeof(page), &index) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL,
          "management web UI render rejects a null buffer");
}

static void test_confirmation(void)
{
    struct appliance_management_web_ui_confirmation confirmation;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&confirmation, 0, sizeof(confirmation));
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_UPDATES,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No mutation confirmation is pending") != NULL,
          "management web UI publishes an empty confirmation state");

    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(1099511627776);
    confirmation.mutation = "UPDATE_ACCEPT<script>";
    confirmation.remaining_ticks = 271U;
    confirmation.single_use = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_UPDATES,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "1099511627776") != NULL &&
              strstr(page, "271") != NULL &&
              strstr(page, "single-use") != NULL &&
              strstr(page, "UPDATE_ACCEPT&lt;script&gt;") != NULL &&
              strstr(page, "UPDATE_ACCEPT<script>") == NULL,
          "management web UI publishes the two-phase confirmation token");

    /* A section without a mutation surface never shows a token. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STORAGE,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "1099511627776") == NULL,
          "management web UI hides confirmation outside a mutation surface");
}


/*
 * ITEM-504: the Routing page.  It is the first section whose page configures:
 * it lists the applied table its owner published, offers the stage and
 * confirmation forms, and shows the pending candidate.  The page never
 * invents a table it was not given and never renders an unescaped byte of
 * one.
 */
static void test_routing_page(void)
{
    struct appliance_management_web_ui_routes routes;
    struct appliance_management_web_ui_confirmation confirmation;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&routes, 0, sizeof(routes));
    routes.published = true;
    routes.count = 2U;
    routes.shown = 2U;
    snprintf(routes.text,
             sizeof(routes.text),
             "name=a.example backend=192.0.2.10:443 tls=on source=panel\n"
             "name=b.example backend=192.0.2.11:80 tls=off "
             "source=declaration\n");
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, NULL, &routes, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "2 routes applied") != NULL &&
              strstr(page, "name=a.example backend=192.0.2.10:443 tls=on "
                           "source=panel") != NULL &&
              strstr(page, "name=b.example backend=192.0.2.11:80 tls=off "
                           "source=declaration") != NULL &&
              strstr(page, "action=\"/panel/routing\"") != NULL &&
              strstr(page, "name=\"name\"") != NULL &&
              strstr(page, "name=\"backend\"") != NULL &&
              strstr(page, "name=\"port\"") != NULL &&
              strstr(page, "name=\"tls\"") != NULL &&
              strstr(page, "name=\"token\"") != NULL &&
              strstr(page, "Confirm") != NULL &&
              strstr(page, "Cancel") != NULL,
          "management web UI Routing page lists the applied table and both "
          "forms");
    CHECK(count_text(page, "name=\"action\" value=\"add\"") == 1U &&
              count_text(page, "name=\"action\" value=\"remove\"") == 1U &&
              count_text(page, "name=\"name\"") == 2U &&
              count_text(page, "name=\"backend\"") == 1U &&
              count_text(page, "name=\"port\"") == 1U &&
              count_text(page, "name=\"tls\"") == 1U &&
              strstr(page, "<label>Route name <input name=\"name\">") != NULL &&
              strstr(page,
                     "<label>Route name to remove <input name=\"name\">") !=
                  NULL &&
              strstr(page,
                     "<label>Server IPv4 <input name=\"backend\" "
                     "placeholder=\"192.0.2.10\">") != NULL &&
              strstr(page,
                     "<label>Server port <input name=\"port\" "
                     "placeholder=\"443\">") != NULL &&
              strstr(page,
                     "<label>TLS to server (off for HTTP) <select "
                     "name=\"tls\">") != NULL,
          "ITEM-647: the route forms label add and remove, and the remove "
          "body carries only action and name");
    check_visible_controls_labelled(page, "routing");

    /* An owner that published nothing is named, never rendered as empty. */
    memset(&routes, 0, sizeof(routes));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, NULL, &routes, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No route table is published") != NULL &&
              strstr(page, "route applied") == NULL,
          "management web UI Routing page names an unpublished table");

    /* A published, empty table is a real state and says what it means. */
    memset(&routes, 0, sizeof(routes));
    routes.published = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, NULL, &routes, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No route is applied") != NULL &&
              strstr(page, "every public name is refused") != NULL,
          "management web UI Routing page publishes an empty applied table "
          "honestly");

    /* A table too large for the page is counted and flagged, not cut. */
    memset(&routes, 0, sizeof(routes));
    routes.published = true;
    routes.count = 8U;
    routes.shown = 4U;
    routes.truncated = true;
    snprintf(routes.text, sizeof(routes.text),
             "name=a.example backend=192.0.2.10:443 tls=on source=panel\n");
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, NULL, &routes, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "8 routes applied, 4 of them listed here") != NULL &&
              strstr(page, "<h3>Add route</h3>") != NULL &&
              strstr(page, "<h3>Remove route</h3>") != NULL &&
              strstr(page, "<h3>Two-phase confirmation</h3>") != NULL,
          "management web UI Routing page counts a table it cannot list "
          "whole and keeps both forms distinct");

    /* A hostile table cannot inject markup into the page. */
    memset(&routes, 0, sizeof(routes));
    routes.published = true;
    routes.count = 1U;
    routes.shown = 1U;
    snprintf(routes.text, sizeof(routes.text),
             "name=<script>alert(1)</script>\n");
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, NULL, &routes, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "&lt;script&gt;") != NULL,
          "management web UI Routing page escapes the table it lists");

    /* The pending candidate is named with its token and its window unit. */
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(424242);
    confirmation.mutation = "ROUTING_CONFIG";
    confirmation.remaining_ticks = 299999U;
    confirmation.single_use = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "ROUTING_CONFIG") != NULL &&
              strstr(page, "424242") != NULL &&
              strstr(page, "299999") != NULL &&
              strstr(page, "ms left; single-use") != NULL &&
              strstr(page, "<h3>Add route</h3>") != NULL &&
              strstr(page, "<h3>Remove route</h3>") != NULL &&
              strstr(page, "<h3>Two-phase confirmation</h3>") != NULL,
          "management web UI Routing page publishes the pending token, its "
          "mutation and its window beside distinct forms");
}

static void test_network_page(void)
{
    struct appliance_management_web_ui_network network;
    struct appliance_management_web_ui_confirmation confirmation;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&network, 0, sizeof(network));
    network.published = true;
    network.active = "mode=DHCP";
    network.saved =
        "mode=STATIC address=10.0.2.15/24 gateway=10.0.2.2 dns=10.0.2.3";
    network.source = "COMMITTED";
    network.applies = "reboot";
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              NULL, NULL, NULL, &network, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Active <code>mode=DHCP</code>") != NULL &&
              strstr(page, "Saved <code>mode=STATIC address=10.0.2.15/24 "
                           "gateway=10.0.2.2 dns=10.0.2.3</code>") != NULL &&
              strstr(page, "source=COMMITTED") != NULL &&
              strstr(page, "applies=reboot") != NULL &&
              strstr(page, "Saved applies at the next boot.") != NULL &&
              strstr(page, "action=\"/panel/network\"") != NULL &&
              strstr(page, "name=\"mode\"") != NULL &&
              strstr(page, "name=\"address\"") != NULL &&
              strstr(page, "name=\"prefix\"") != NULL &&
              strstr(page, "name=\"gateway\"") != NULL &&
              strstr(page, "name=\"dns\"") != NULL &&
              strstr(page, "name=\"token\"") != NULL &&
              strstr(page, "Confirm") != NULL &&
              strstr(page, "Cancel") != NULL,
          "management web UI Network page shows both readings and both forms");

    /* An owner that published nothing is named, never rendered as blank. */
    memset(&network, 0, sizeof(network));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              NULL, NULL, NULL, &network, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No network owner is published") != NULL &&
              strstr(page, "Active <code>") == NULL,
          "management web UI Network page names an unpublished owner");

    /* A hostile reading cannot inject markup into the page. */
    memset(&network, 0, sizeof(network));
    network.published = true;
    network.active = "<script>alert(1)</script>";
    network.saved = "mode=DHCP\"><img src=x>";
    network.source = "COMMITTED";
    network.applies = "reboot";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              NULL, NULL, NULL, &network, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "<img src=x") == NULL &&
              strstr(page, "&lt;script&gt;") != NULL,
          "management web UI Network page escapes the readings it shows");

    /* The pending candidate is named with its token and its window unit. */
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(424243);
    confirmation.mutation = "NETWORK_CONFIG";
    confirmation.remaining_ticks = 299999U;
    confirmation.single_use = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "NETWORK_CONFIG") != NULL &&
              strstr(page, "424243") != NULL &&
              strstr(page, "299999") != NULL &&
              strstr(page, "ms left; single-use") != NULL,
          "management web UI Network page publishes the pending token, its "
          "mutation and its window");
}

static void test_system_page(void)
{
    struct appliance_management_web_ui_system system;
    struct appliance_management_web_ui_confirmation confirmation;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "hfoc-appliance";
    system.saved = "node-505";
    system.source = "COMMITTED";
    system.applies = "reboot";
    /*
     * ITEM-509 increment 5: the same page carries the interface language pair,
     * read from the same system record.  The panel renders what its owner
     * published - the active language and the saved one - and never invents
     * either.
     */
    system.language_active = "ru";
    system.language_saved = "ru";
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              NULL, NULL, NULL, NULL, &system, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Active <code>hfoc-appliance</code>") != NULL &&
              strstr(page, "Saved <code>node-505</code>") != NULL &&
              strstr(page, "source=COMMITTED") != NULL &&
              strstr(page, "applies=reboot") != NULL &&
              strstr(page, "Saved applies at the next boot.") != NULL &&
              strstr(page, "Language active <code>ru</code>") != NULL &&
              strstr(page, "Saved <code>ru</code>") != NULL &&
              strstr(page, "action=\"/panel/system\"") != NULL &&
              strstr(page, "name=\"hostname\"") != NULL &&
              strstr(page, "name=\"token\"") != NULL &&
              strstr(page, "Confirm") != NULL &&
              strstr(page, "Cancel") != NULL,
          "management web UI System page shows both readings, the language and "
          "both forms");
    check_visible_controls_labelled(page, "system");
    CHECK(strstr(page, "<label>Hostname <input name=\"hostname\"></label>") !=
                  NULL &&
              strstr(page,
                     "<label>Panel language <input name=\"language\"></label>") !=
                  NULL &&
              strstr(page,
                     "<label>Confirmation code <input name=\"token\"") != NULL,
          "ITEM-647: the English System form names all three controls");

    /* A record the owner could not read is named, not guessed. */
    system.language_active = NULL;
    system.language_saved = NULL;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              NULL, NULL, NULL, NULL, &system, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Language active <code>UNAVAILABLE</code>") != NULL &&
              strstr(page, "Saved <code>UNAVAILABLE</code>") != NULL,
          "management web UI System page names an unreadable language");
    system.language_active = "ru";
    system.language_saved = "ru";

    /* An owner that published nothing is named, never rendered as blank. */
    memset(&system, 0, sizeof(system));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              NULL, NULL, NULL, NULL, &system, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No system owner is published") != NULL &&
              strstr(page, "Active <code>") == NULL,
          "management web UI System page names an unpublished owner");

    /* A hostile reading cannot inject markup into the page. */
    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "<script>alert(1)</script>";
    system.saved = "node\"><img src=x>";
    system.source = "COMMITTED";
    system.applies = "reboot";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              NULL, NULL, NULL, NULL, &system, NULL,NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "<img src=x") == NULL &&
              strstr(page, "&lt;script&gt;") != NULL,
          "management web UI System page escapes the readings it shows");

    /* The pending candidate is named with its token and its window unit. */
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(424244);
    confirmation.mutation = "SYSTEM_HOSTNAME";
    confirmation.remaining_ticks = 299998U;
    confirmation.single_use = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "SYSTEM_HOSTNAME") != NULL &&
              strstr(page, "424244") != NULL &&
              strstr(page, "299998") != NULL &&
              strstr(page, "ms left; single-use") != NULL,
          "management web UI System page publishes the pending token, its "
          "mutation and its window");
}

/*
 * ITEM-506 increment 6: the Account page.  It shows the stored login and the
 * four-field change form; it never renders a password, and an owner that
 * published nothing is named instead of a blank.
 */
static void test_account_page(void)
{
    struct appliance_management_web_ui_account account;
    struct appliance_management_web_ui_confirmation confirmation;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&account, 0, sizeof(account));
    account.published = true;
    account.login = "operator";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              NULL, NULL, NULL, NULL, NULL, &account,NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Login <code>operator</code>") != NULL &&
              strstr(page, "action=\"/panel/account\"") != NULL &&
              strstr(page, "name=\"current\"") != NULL &&
              strstr(page, "name=\"login\"") != NULL &&
              strstr(page, "name=\"password\"") != NULL &&
              strstr(page, "name=\"repeat\"") != NULL &&
              strstr(page, "type=\"password\"") != NULL &&
              strstr(page, "name=\"token\"") != NULL &&
              strstr(page, "Confirm") != NULL &&
              strstr(page, "Cancel") != NULL &&
              strstr(page, "signs out every session") != NULL,
          "management web UI Account page shows the login and both forms");

    /* An owner that published nothing is named, never rendered as blank. */
    memset(&account, 0, sizeof(account));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              NULL, NULL, NULL, NULL, NULL, &account,NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "No account owner is published") != NULL &&
              strstr(page, "Login <code>") == NULL,
          "management web UI Account page names an unpublished owner");

    /* A hostile login cannot inject markup into the page. */
    memset(&account, 0, sizeof(account));
    account.published = true;
    account.login = "<script>alert(1)</script>";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              NULL, NULL, NULL, NULL, NULL, &account,NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "&lt;script&gt;") != NULL,
          "management web UI Account page escapes the login it shows");

    /* The pending candidate is named with its token and its window unit. */
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = UINT64_C(515151);
    confirmation.mutation = "ACCOUNT_CREDENTIAL";
    confirmation.remaining_ticks = 299997U;
    confirmation.single_use = true;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              NULL, &confirmation, NULL, NULL, NULL, NULL,NULL, NULL, page,
              sizeof(page), &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "ACCOUNT_CREDENTIAL") != NULL &&
              strstr(page, "515151") != NULL &&
              strstr(page, "299997") != NULL &&
              strstr(page, "ms left; single-use") != NULL,
          "management web UI Account page publishes the pending token, its "
          "mutation and its window");
}

/*
 * ITEM-499: link integrity, proven on the rendered documents rather than on the
 * source.  The panel menu sent the browser to `/panel//panel/network`, because
 * the writer printed the `/panel` prefix and then the section's own absolute
 * path; the page opened and every menu entry except Dashboard answered 404.
 * These helpers walk the documents the panel actually serves, extract every
 * target they publish and require each target to be one the machine answers -
 * the section table the router matches and the about/licenses page, nothing
 * else.  A document may not reference a target the machine does not serve.
 */
static bool target_served(const char *target, size_t length)
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        const char *path = appliance_management_web_ui_section_path(
            (enum appliance_management_web_ui_section)index);

        if (path != NULL && strlen(path) == length &&
            memcmp(path, target, length) == 0) {
            return true;
        }
    }
    if (length == strlen(APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH) &&
        memcmp(APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH, target,
               length) == 0) {
        return true;
    }
    /*
     * ITEM-516: the management-gate page is a served panel page of its own,
     * linked from the Security section.
     */
    return length == strlen(APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH) &&
           memcmp(APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH, target, length) == 0;
}

static bool form_target_served(const char *target, size_t length)
{
    return (length == HTTP_POLICY_FORM_TARGET_LENGTH &&
            memcmp(HTTP_POLICY_FORM_TARGET, target, length) == 0) ||
           (length == HTTP_POLICY_SIGN_OUT_TARGET_LENGTH &&
            memcmp(HTTP_POLICY_SIGN_OUT_TARGET, target, length) == 0);
}

static void check_published_targets(
    const char *page,
    const char *attribute,
    bool form_target)
{
    const char *cursor = page;
    const size_t attribute_length = strlen(attribute);

    while ((cursor = strstr(cursor, attribute)) != NULL) {
        const char *target = cursor + attribute_length;
        const char *end = strchr(target, '"');
        char message[192];
        size_t target_length;

        if (end == NULL) {
            CHECK(false,
                  "management web UI terminates every published target");
            return;
        }
        target_length = (size_t)(end - target);
        snprintf(message, sizeof(message),
                 "management web UI %s %.*s is one the machine serves",
                 form_target ? "form target" : "link",
                 (int)target_length, target);
        CHECK(form_target ? form_target_served(target, target_length)
                          : target_served(target, target_length),
              message);
        cursor = end + 1U;
    }
}

static size_t count_hrefs(const char *page)
{
    const char *cursor = page;
    size_t count = 0U;

    while ((cursor = strstr(cursor, "href=\"")) != NULL) {
        ++count;
        cursor += 6U;
    }
    return count;
}

static bool document_links_every_section(const char *page)
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        const char *path = appliance_management_web_ui_section_path(
            (enum appliance_management_web_ui_section)index);
        char link[96];

        if (path == NULL) {
            return false;
        }
        snprintf(link, sizeof(link), "href=\"%s\"", path);
        if (strstr(page, link) == NULL) {
            return false;
        }
    }
    return true;
}

static bool read_file(const char *path, char *buffer, size_t capacity);

static void fill_licenses(
    struct appliance_management_web_ui_licenses *licenses,
    bool declared,
    bool complete);

static void test_link_integrity(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_licenses licenses;
    struct appliance_management_web_ui_session session;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t index;
    size_t length = 0U;

    memset(&view, 0, sizeof(view));
    memset(&session, 0, sizeof(session));
    fill_licenses(&licenses, true, true);

    /*
     * The menu is published by every section document and by the licenses
     * page; each of them has to reach every section and to name no target the
     * router refuses.
     */
    for (index = 0U;
         index <= APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        length = 0U;
        if (index == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT) {
            CHECK(appliance_management_web_ui_render_licenses(
                      &licenses, page, sizeof(page), &length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI renders the licenses page for the menu");
        } else {
            CHECK(appliance_management_web_ui_render_section(
                      (enum appliance_management_web_ui_section)index,
                      &view, NULL, NULL, NULL, NULL, NULL,NULL, NULL, page, sizeof(page), &length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI renders every section for the menu");
        }
        check_published_targets(page, "href=\"", false);
        {
            /*
             * ITEM-516: the Security page carries one service link beside the
             * menu - the management-gate page - so its document has one more
             * href than the menu alone.
             */
            size_t expected =
                APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT + 1U;

            if ((enum appliance_management_web_ui_section)index ==
                APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY) {
                ++expected;
            }
            CHECK(count_hrefs(page) == expected,
                  "management web UI menu publishes one link per served target");
        }
        CHECK(document_links_every_section(page),
              "management web UI menu reaches every section");
    }

    /*
     * The two browser forms publish their own targets: the sign-in form posts
     * to `/` and the sign-out form to `/panel/logout`.  Both are panel
     * targets, and neither may be a target the machine refuses.
     */
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "management web UI renders the sign-in document for its form");
    check_published_targets(page, "action=\"", true);

    session.authenticated = true;
    session.session_id = 4U;
    session.principal_id = 1U;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "action=\"/panel/logout\"") != NULL,
          "management web UI signs out through its own declared target");
    check_published_targets(page, "action=\"", true);
}

static bool read_file(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    size_t length;

    if (file == NULL) {
        return false;
    }
    length = fread(buffer, 1U, capacity - 1U, file);
    fclose(file);
    buffer[length] = '\0';
    return true;
}

/*
 * ITEM-382 about/licenses page. The declared inventory is rendered from the
 * ITEM-382 kernel mirror; the state comes from the owner that read the shipped
 * payload. An unpublished inventory is UNAVAILABLE with a reason and no list,
 * an incomplete payload is DEGRADED with its reason, and every dynamic value is
 * escaped.
 */
static void fill_licenses(
    struct appliance_management_web_ui_licenses *licenses,
    bool declared,
    bool complete)
{
    memset(licenses, 0, sizeof(*licenses));
    licenses->declared = declared;
    licenses->payload_complete = complete;
    licenses->declared_count = 4U;
    licenses->shipped_count = complete ? 4U : 1U;
    if (declared) {
        licenses->manifest = "LICENSES.sha256";
        licenses->payload_root = "licenses";
    }
}

static void test_licenses_page(void)
{
    struct appliance_management_web_ui_licenses licenses;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    CHECK(appliance_management_web_ui_render_licenses(
              NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<!doctype html>") == page &&
              strstr(page, "<h2>About and licenses</h2>") != NULL &&
              strstr(page, "UNAVAILABLE") != NULL &&
              strstr(page, "LICENSE_INVENTORY_UNPUBLISHED") != NULL &&
              strstr(page, "<table>") == NULL &&
              strstr(page, "AGPL-3.0-only OR LicenseRef-HFOC-Commercial") !=
                  NULL,
          "management web UI licenses page publishes UNAVAILABLE with a "
          "reason and no invented list");

    fill_licenses(&licenses, true, false);
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "DEGRADED") != NULL &&
              strstr(page, "LICENSE_PAYLOAD_INCOMPLETE") != NULL &&
              strstr(page, "<table>") != NULL,
          "management web UI licenses page publishes an incomplete payload as "
          "DEGRADED");

    fill_licenses(&licenses, true, true);
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "AVAILABLE") != NULL &&
              strstr(page, "shipped notices 4 of 4 declared") != NULL &&
              strstr(page, "lwIP") != NULL &&
              strstr(page, "STABLE-2_2_1_RELEASE") != NULL &&
              strstr(page, "BSD-3-Clause") != NULL &&
              strstr(page, "BSD-2-Clause") != NULL &&
              strstr(page, "mbedTLS election <code>Apache-2.0</code>") !=
                  NULL &&
              strstr(page,
                     "from <code>Apache-2.0 OR GPL-2.0-or-later</code>") !=
                  NULL &&
              strstr(page,
                     "notices/mbedtls-Apache-2.0-election.txt") !=
                  NULL &&
              strstr(page, "third_party/mbedtls/LICENSE") != NULL &&
              strstr(page, "third_party/lwip/COPYING") != NULL &&
              strstr(page, "third_party/limine/COPYING") != NULL &&
              strstr(page, "LICENSES.sha256") != NULL &&
              strstr(page, "/panel/about/licenses") != NULL,
          "management web UI licenses page lists the shipped inventory with "
          "the mbedTLS election");
    /* The elected license is what the component row states; the offered
     * disjunction is published only as the election source. */
    CHECK(strstr(page, "<td>Apache-2.0 OR GPL-2.0-or-later</td>") == NULL,
          "management web UI licenses page states the elected license only");

    fill_licenses(&licenses, true, false);
    licenses.reason = "<script>alert(1)</script>";
    licenses.manifest = "LICENSES.sha256\"><img src=x onerror=alert(1)>";
    licenses.payload_root = "</main><form action=\"/panel\">";
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "<img src=x") == NULL &&
              /*
               * ITEM-513: every panel page now carries the nav's language
               * form, so `strstr(page, "<form")` no longer proves the hostile
               * value was escaped.  The hostile value here is
               * `</main><form action="/panel">`; its raw bytes must not
               * appear, while its escaped text must.
               */
              strstr(page, "<form action=\"/panel\">") == NULL &&
              strstr(page, "&lt;/main&gt;") != NULL &&
              strstr(page, "&lt;script&gt;") != NULL,
          "management web UI licenses page escapes every dynamic value");

    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, NULL, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL &&
          appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), NULL) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL,
          "management web UI licenses page rejects null buffers");
    CHECK(appliance_management_web_ui_page_budget(
              APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES,
          "management web UI licenses page budget is recorded");
}

/*
 * ITEM-505 increment 2: the worst case the live production panel view can
 * publish.  The composition bounds every field, so this is the largest value
 * set a real appliance can render; the recorded budget has to cover it, not
 * only the short fixture values above.  The detail line is filled to the
 * longest `state=` value that still fits its own bound, because that is the
 * longest detail the module can publish.
 */
static void fill_worst_case_view(
    struct appliance_production_panel_view_storage *storage)
{
    static const uint8_t mac[APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH] = {
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    };
    static char hostname[APPLIANCE_PRODUCTION_PANEL_VIEW_HOSTNAME_MAX];
    static char service[APPLIANCE_PRODUCTION_PANEL_VIEW_SERVICE_MAX];
    static char state[APPLIANCE_PRODUCTION_PANEL_VIEW_DETAIL_MAX];
    static char log_line[APPLIANCE_PRODUCTION_PANEL_VIEW_LOG_MAX];
    struct appliance_production_panel_view_owners owners;
    size_t index;

    for (index = 0U; index + 1U < sizeof(hostname); ++index) {
        hostname[index] = 'h';
    }
    hostname[sizeof(hostname) - 1U] = '\0';
    for (index = 0U; index + 1U < sizeof(service); ++index) {
        service[index] = 's';
    }
    service[sizeof(service) - 1U] = '\0';
    for (index = 0U; index < sizeof(state) - 7U; ++index) {
        state[index] = 'd';
    }
    state[sizeof(state) - 7U] = '\0';
    for (index = 0U; index + 1U < sizeof(log_line); ++index) {
        log_line[index] = 'l';
    }
    log_line[sizeof(log_line) - 1U] = '\0';

    memset(&owners, 0, sizeof(owners));
    owners.version = APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION;
    owners.hostname = hostname;
    owners.mac_address = mac;
    owners.service_name = service;
    owners.serving_state = state;
    /*
     * No reason: the longest detail the module publishes is the longest
     * `state=` value alone that fits the detail bound.  Adding a reason would
     * push the line past the bound and publish no detail at all, which is the
     * shorter page.
     */
    owners.serving_reason = NULL;
    owners.recent_log = log_line;
    if (appliance_production_panel_view_compose(&owners, storage) !=
        APPLIANCE_PRODUCTION_PANEL_VIEW_OK) {
        memset(storage, 0, sizeof(*storage));
    }
}

/*
 * Measure every page, compare it with the recorded budget, and require the
 * recorded table in the documentation to quote the measurement. Any growth
 * past the recorded budget fails here; any change inside the budget fails
 * until the documentation is updated with the new measurement.
 */
/*
 * ITEM-508 increment 32: the Dashboard's read-only ACME state block.  The
 * block is a reading: it names the service state, the account verdict, the
 * renewal decision, the expiry and the attempt count, and it carries no form
 * and no action - enabling, disabling and issuance stay two-phase operator
 * mutations.
 */
static void test_dashboard_acme_block(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_certificates certificates;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char block[512];
    size_t length = 0U;
    const char *acme;
    const char *end;

    memset(&view, 0, sizeof(view));
    memset(&certificates, 0, sizeof(certificates));
    certificates.acme.published = true;
    certificates.acme.state = "BOUND";
    certificates.acme.account = "bound";
    certificates.acme.renewal = "NOT_DUE";
    certificates.acme.expires = "1800000000";
    certificates.acme.attempts = "0";
    certificates.acme.order = "RUNNING/ORDER";
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD, &view, NULL,
              NULL, NULL, NULL, NULL, &certificates, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "the dashboard renders with the ACME state published");
    acme = strstr(page, "<h3>ACME</h3>");
    end = acme != NULL ? strstr(acme, "</p>") : NULL;
    CHECK(acme != NULL && end != NULL && acme < end &&
              (size_t)(end - acme) < sizeof(block),
          "the ACME block is one bounded paragraph");
    if (acme != NULL && end != NULL && acme < end &&
        (size_t)(end - acme) < sizeof(block)) {
        memcpy(block, acme, (size_t)(end - acme));
        block[(size_t)(end - acme)] = '\0';
        CHECK(strstr(block, "state BOUND") != NULL &&
                  strstr(block, "account bound") != NULL &&
                  strstr(block, "renewal NOT_DUE") != NULL &&
                  strstr(block, "expires 1800000000") != NULL &&
                  strstr(block, "attempts 0") != NULL &&
                  strstr(block, "order RUNNING/ORDER") != NULL,
              "the ACME block names the whole state the point asks for, "
              "including the order's own step");
        CHECK(strstr(block, "<form") == NULL &&
                  strstr(block, "action=") == NULL,
              "the ACME block is a reading, not a mutation");
    }
    certificates.acme.state = "\"><script>alert(1)</script>";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD, &view, NULL,
              NULL, NULL, NULL, NULL, &certificates, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "&lt;") != NULL,
          "the ACME block escapes a hostile value");
    memset(&certificates.acme, 0, sizeof(certificates.acme));
    certificates.acme.published = false;
    certificates.acme.reason = "NO_ACME_STATE";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD, &view, NULL,
              NULL, NULL, NULL, NULL, &certificates, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h3>ACME</h3>") != NULL &&
              strstr(page, "NO_ACME_STATE") != NULL,
          "an unpublished ACME block names its reason instead of vanishing");
}

/*
 * ITEM-508 increment 34: the ACME control page.  It must show the durable
 * setting, offer exactly the three two-phase actions and the confirmation
 * form, escape every dynamic value, and name its reason instead of vanishing
 * when no record is published.  The page never carries certificate, key or
 * token material of its own - the only token on it is the plane's single-use
 * confirmation field.
 */
static void test_acme_control_page(void)
{
    static char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    struct appliance_management_web_ui_acme_control control;
    struct appliance_management_web_ui_confirmation confirmation;
    size_t length = 0U;

    memset(&control, 0, sizeof(control));
    control.published = true;
    control.enabled = "ENABLED";
    control.requested = "REQUESTED";
    control.requested_at = "1800000042";
    control.source = "COMMITTED";
    control.applies = "service";
    memset(&confirmation, 0, sizeof(confirmation));
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, &confirmation, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>ACME control</h2>") != NULL &&
              strstr(page, "ENABLED") != NULL &&
              strstr(page, "REQUESTED") != NULL &&
              strstr(page, "1800000042") != NULL &&
              strstr(page, "COMMITTED") != NULL &&
              strstr(page, "applies=service") != NULL,
          "the ACME control page shows the committed setting");
    CHECK(strstr(page, "action=\"/panel/acme\"") != NULL &&
              strstr(page, "value=\"enable\"") != NULL &&
              strstr(page, "value=\"disable\"") != NULL &&
              strstr(page, "value=\"request\"") != NULL &&
              strstr(page, "name=\"token\"") != NULL &&
              strstr(page, "name=\"confirm\"") != NULL &&
              strstr(page, "name=\"cancel\"") != NULL,
          "the ACME control page offers the three actions and the "
          "confirmation form");

    /* A hostile value is escaped and never reaches the document raw. */
    control.enabled = hostile_values[0];
    control.source = hostile_values[1];
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<script>") == NULL &&
              strstr(page, "<img") == NULL &&
              strstr(page, "&lt;") != NULL,
          "the ACME control page escapes every dynamic value");

    memset(&control, 0, sizeof(control));
    control.published = false;
    control.reason = "NO_ACME_CONTROL_RECORD";
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "NO_ACME_CONTROL_RECORD") != NULL &&
              /*
               * ITEM-513: the nav's language form is on every page; the
               * claim is that the ACME control offers no action form of its
               * own, which its declared target names.
               */
              strstr(page, "/panel/acme\"") == NULL,
          "an unpublished ACME control page names its reason and offers no "
          "action");

    CHECK(appliance_management_web_ui_render_acme_control(
              NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "/panel/acme\"") == NULL,
          "a NULL ACME control page renders the honest unavailable document");
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, NULL, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_NULL,
          "the ACME control page rejects a null output");
}

/*
 * ITEM-509 increment 8: the Help section.  One content, two renderers: the
 * console proof walks the same catalog, and this proof requires every topic on
 * the Help index and on its own page - all four fields - in both languages.
 */
static void test_help_page(void)
{
    struct appliance_management_web_ui_help help;
    struct appliance_management_web_ui_view view;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char line[256];
    size_t length;
    size_t index;
    enum appliance_language language;

    CHECK(strcmp(appliance_management_web_ui_section_string(
                     APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP),
                 "Help") == 0 &&
              strcmp(appliance_management_web_ui_section_path(
                         APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP),
                     "/panel/help") == 0 &&
              appliance_management_web_ui_section_owner(
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) == NULL &&
              appliance_management_web_ui_section_state(
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED &&
              !appliance_management_web_ui_section_confirmable(
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP),
          "the Help section is one declared section with its own path");

    for (language = APPLIANCE_LANGUAGE_EN;
         language < APPLIANCE_LANGUAGE_COUNT;
         ++language) {
        const char *code = appliance_language_code(language);

        /*
         * The index: every topic links to its page and carries its summary, in
         * the language the machine is set to.
         */
        memset(&help, 0, sizeof(help));
        help.published = true;
        help.language = code;
        length = 0U;
        CHECK(appliance_management_web_ui_render_section(
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
                  NULL, NULL, NULL, NULL, NULL, NULL, NULL, &help,
                  page, sizeof(page), &length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                  length > 0U,
              "the Help index renders");
        for (index = 0U;
             index < appliance_help_topic_count();
             ++index) {
            snprintf(line, sizeof(line),
                     "href=\"/panel/help/%s\"",
                     appliance_help_topic_id(index));
            CHECK(strstr(page, line) != NULL &&
                      strstr(page, appliance_help_topic_string(
                                       index,
                                       APPLIANCE_HELP_FIELD_TITLE,
                                       language)) != NULL &&
                      strstr(page, appliance_help_topic_string(
                                       index,
                                       APPLIANCE_HELP_FIELD_SUMMARY,
                                       language)) != NULL,
                  "the Help index lists every topic with title and summary");
        }

        /*
         * ITEM-509 increment 25: the index sentence and the way back are
         * operator prose from the catalog; only the `<code>help</code>` markup
         * between the sentence halves stays a literal.
         */
        {
            const char *intro = appliance_language_string(
                language, "panel.help.index_intro");
            const char *intro_tail = appliance_language_string(
                language, "panel.help.index_intro_tail");

            CHECK(intro != NULL && intro_tail != NULL &&
                      strstr(page, intro) != NULL &&
                      strstr(page, intro_tail) != NULL &&
                      strstr(page, "<code>help</code>") != NULL,
                  "the Help index explains itself in the chosen language");
            if (language != APPLIANCE_LANGUAGE_EN) {
                CHECK(strstr(page, "from the built-in catalog") == NULL,
                      "the English Help index sentence is gone from a "
                      "translated page");
            }
        }

        /*
         * Every topic page: title, summary, the whole body (line by line, so
         * the author's breaks become `<br>`) and the related fields.
         */
        for (index = 0U;
             index < appliance_help_topic_count();
             ++index) {
            const char *body = appliance_help_topic_string(
                index, APPLIANCE_HELP_FIELD_BODY, language);
            size_t offset = 0U;
            size_t breaks = 0U;
            size_t expected_breaks = 0U;

            memset(&help, 0, sizeof(help));
            help.published = true;
            help.language = code;
            help.topic = appliance_help_topic_id(index);
            length = 0U;
            CHECK(appliance_management_web_ui_render_section(
                      APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
                      NULL, NULL, NULL, NULL, NULL, NULL, NULL, &help,
                      page, sizeof(page), &length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                      length > 0U,
                  "the Help topic page renders");
            CHECK(strstr(page, appliance_help_topic_string(
                                   index,
                                   APPLIANCE_HELP_FIELD_TITLE,
                                   language)) != NULL &&
                      strstr(page, appliance_help_topic_string(
                                       index,
                                       APPLIANCE_HELP_FIELD_SUMMARY,
                                       language)) != NULL &&
                      strstr(page, appliance_help_topic_string(
                                       index,
                                       APPLIANCE_HELP_FIELD_RELATED,
                                       language)) != NULL,
                  "the Help topic page carries title, summary and related");
            while (body[offset] != '\0') {
                const size_t start = offset;
                char escaped_line[
                    APPLIANCE_MANAGEMENT_WEB_UI_ESCAPE_MAX + 1U];
                size_t escaped_length = 0U;

                while (body[offset] != '\0' && body[offset] != '\n') {
                    ++offset;
                }
                /*
                 * The page escapes the catalog text, so the proof compares the
                 * escaped line: an apostrophe in an English body must appear
                 * as `&#39;`, never as raw markup.
                 */
                CHECK(appliance_management_web_ui_escape(
                          body + start,
                          offset - start,
                          escaped_line,
                          sizeof(escaped_line),
                          &escaped_length) ==
                          APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                          escaped_length > 0U,
                      "a Help body line escapes into the test buffer");
                escaped_line[escaped_length] = '\0';
                CHECK(strstr(page, escaped_line) != NULL,
                      "the Help topic page carries every body line");
                if (body[offset] == '\n') {
                    ++expected_breaks;
                    ++offset;
                }
            }
            {
                const char *cursor = page;

                while ((cursor = strstr(cursor, "<br>\n")) != NULL) {
                    ++breaks;
                    cursor += 5;
                }
            }
            CHECK(breaks == expected_breaks,
                  "the Help topic page keeps the author's body breaks");
            {
                char back_link[128];
                const char *label = appliance_language_string(
                    language, "panel.help.all_topics");

                snprintf(back_link, sizeof(back_link),
                         "<a href=\"/panel/help\">%s</a>",
                         label != NULL ? label : "");
                CHECK(strstr(page, "</html>") != NULL &&
                          strstr(page, back_link) != NULL,
                      "the Help topic page is a complete document with a way "
                      "back in the chosen language");
                if (language != APPLIANCE_LANGUAGE_EN) {
                    CHECK(strstr(page, "All help topics") == NULL,
                          "the English way back is gone from a translated "
                          "Help topic page");
                }
            }
        }
    }

    /* An unknown id is named, not hidden. */
    memset(&help, 0, sizeof(help));
    help.published = true;
    help.language = "en";
    help.topic = "no-such-topic";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
              NULL, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "NO_SUCH_HELP_TOPIC") != NULL,
          "an unknown Help topic is refused by name");

    /* A language the catalog cannot parse is named, not guessed. */
    memset(&help, 0, sizeof(help));
    help.published = true;
    help.language = "de";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
              NULL, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "LANGUAGE_UNAVAILABLE") != NULL &&
              strstr(page, "help.topic.") == NULL,
          "an unparseable Help language is refused by name");

    /* A record that never loaded says so instead of rendering nothing. */
    memset(&help, 0, sizeof(help));
    help.published = false;
    help.reason = "SYSTEM_LANGUAGE_UNAVAILABLE";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
              NULL, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "SYSTEM_LANGUAGE_UNAVAILABLE") != NULL,
          "an unpublished Help language names its reason");

    /* The Dashboard carries the index with the same links. */
    fill_view(&view, 0U);
    memset(&help, 0, sizeof(help));
    help.published = true;
    help.language = "en";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Help index") != NULL,
          "the Dashboard carries the help index");
    for (index = 0U; index < appliance_help_topic_count(); ++index) {
        snprintf(line, sizeof(line), "href=\"/panel/help/%s\"",
                 appliance_help_topic_id(index));
        CHECK(strstr(page, line) != NULL,
              "the Dashboard help index links every topic");
    }

    /*
     * Without a published language the Dashboard block refuses by name too,
     * and the rest of the Dashboard still renders.
     */
    memset(&help, 0, sizeof(help));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>Dashboard</h2>") != NULL &&
              strstr(page, "LANGUAGE_UNAVAILABLE") != NULL,
          "an unpublished language leaves the Dashboard standing and names "
          "the refusal");
}

/*
 * ITEM-509 increment 10: the language framework.
 *
 * The rendered document speaks the language the caller asked for: the
 * navigation, the page heading, the footer and the `lang` attribute all read
 * the one catalog.  The English document is the default (the field's zero
 * value) and the pre-increment bytes; the Russian document is the same page in
 * the other catalog column; a language outside the enum is refused with the
 * named status instead of being served in English in silence.
 */
static void test_language_framework(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_help help;
    struct appliance_management_web_ui_session session;
    struct appliance_management_web_ui_licenses licenses;
    struct appliance_management_web_ui_acme_control control;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char russian[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;
    size_t russian_length = 0U;

    memset(&view, 0, sizeof(view));
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"en\">") != NULL &&
              strstr(page, "<h2>Dashboard</h2>") != NULL &&
              strstr(page, ">Dashboard</a>") != NULL,
          "ITEM-509: the default view renders the English document");
    {
        /* The explicit English field is the same document as the zero one. */
        struct appliance_management_web_ui_view english;
        size_t english_length = 0U;

        memset(&english, 0, sizeof(english));
        english.language = APPLIANCE_LANGUAGE_EN;
        CHECK(appliance_management_web_ui_render_section(
                  APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
                  &english, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                  russian, sizeof(russian), &english_length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                  english_length == length &&
                  memcmp(page, russian, length) == 0,
              "ITEM-509: English is byte-for-byte the pre-increment document");
    }

    view.language = APPLIANCE_LANGUAGE_RU;
    russian_length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
              russian, sizeof(russian), &russian_length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(russian, "<html lang=\"ru\">") != NULL &&
              strstr(russian, "<h2>Обзор</h2>") != NULL &&
              strstr(russian, ">Сеть</a>") != NULL &&
              strstr(russian, ">Справка</a>") != NULL &&
              strstr(russian, "Панель управления HFOC Appliance") != NULL &&
              strstr(russian, "Dashboard") == NULL,
          "ITEM-509: the Russian view publishes the Russian navigation, "
          "heading, footer and lang");

    /*
     * The Help page renders its body from `help->language`; the document's
     * `lang` attribute comes from the same string, so the page cannot claim
     * one language and print the other.
     */
    memset(&view, 0, sizeof(view));
    memset(&help, 0, sizeof(help));
    help.published = true;
    help.language = "ru";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, &help,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL &&
              strstr(page, "<h2>Справка</h2>") != NULL &&
              strstr(page, "<html lang=\"en\">") == NULL,
          "ITEM-509: the Help page takes its language from the same record");

    memset(&session, 0, sizeof(session));
    session.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL &&
              strstr(page, "Справка</nav>") != NULL,
          "ITEM-509: the sign-in document takes the session language");

    memset(&licenses, 0, sizeof(licenses));
    fill_licenses(&licenses, true, true);
    licenses.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL,
          "ITEM-509: the licenses page renders in the requested language");

    memset(&control, 0, sizeof(control));
    control.published = true;
    control.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL,
          "ITEM-509: the ACME control page renders in the requested "
          "language");

    /*
     * A language outside the enum has no catalog column: the render is
     * refused by name, and the refusal has a status string, so the panel can
     * answer a named 503 instead of publishing an English page as if the
     * machine were set to English.
     */
    memset(&view, 0, sizeof(view));
    view.language = (enum appliance_language)7;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION &&
              appliance_management_web_ui_status_string(
                  APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION) != NULL,
          "ITEM-509: an unknown language is refused by name, not served in "
          "English");
    memset(&session, 0, sizeof(session));
    session.language = (enum appliance_language)7;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION,
          "ITEM-509: the sign-in document refuses an unknown language too");
}

/*
 * ITEM-509 increment 12: the forms, the two-phase confirmations and the
 * licenses page render their prose from the catalog.  The English fragments
 * below are the literals the renderers printed before the increment, so a
 * document that still carries them byte for byte is unchanged; the Russian
 * fragments are the catalog column the operator selected.
 */
static void test_form_language(void)
{
    static const char longest_active[] =
        "mode=STATIC address=10.0.2.15/24 gateway=10.0.2.1 dns=10.0.2.3";
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_confirmation confirmation;
    struct appliance_management_web_ui_network network;
    struct appliance_management_web_ui_system system;
    struct appliance_management_web_ui_account account;
    struct appliance_management_web_ui_session session;
    struct appliance_management_web_ui_licenses licenses;
    struct appliance_management_web_ui_acme_control control;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&view, 0, sizeof(view));
    memset(&confirmation, 0, sizeof(confirmation));
    confirmation.pending = true;
    confirmation.token = 4242U;
    confirmation.mutation = "NETWORK_CONFIG";
    confirmation.remaining_ticks = 90000U;
    confirmation.single_use = true;
    memset(&network, 0, sizeof(network));
    network.published = true;
    network.active = longest_active;
    network.saved = longest_active;
    network.source = "COMMITTED";
    network.applies = "reboot";
    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "appliance";
    system.saved = "appliance";
    system.source = "COMMITTED";
    system.applies = "reboot";
    memset(&account, 0, sizeof(account));
    account.published = true;
    account.login = "operator";

    /* English: the network form and its pending confirmation. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              &view, &confirmation, NULL, &network, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page,
                     "<option value=\"static\">Static</option>") != NULL &&
              strstr(page, "<button>Stage</button>") != NULL &&
              strstr(page,
                     "name=\"confirm\" value=\"1\">Confirm</button>") != NULL &&
              strstr(page,
                     "name=\"cancel\" value=\"1\">Cancel</button>") != NULL &&
              strstr(page, "<h3>Pending</h3>") != NULL &&
              strstr(page, " token 4242") != NULL &&
              strstr(page, " ms left; single-use.</p>") != NULL,
          "ITEM-509: the English network form and confirmation are byte for "
          "byte the pre-increment document");
    check_visible_controls_labelled(page, "network-en");


    /* English: the operator certificate import form and its confirmation. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h3>Import operator certificate</h3>") != NULL &&
              strstr(page, "placeholder=\"certificate PEM\"") != NULL &&
              strstr(page, "placeholder=\"chain PEM (optional)\"") != NULL &&
              strstr(page, "placeholder=\"key PEM\"") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Stage</button>") != NULL &&
              strstr(page, "<label>Certificate PEM<br>") != NULL &&
              strstr(page, "<label>Private key PEM<br>") != NULL &&
              strstr(page,
                     "<label>Confirmation code <input name=\"token\"") != NULL &&
              strstr(page, "Appears after the change is staged.") != NULL &&
              strstr(page,
                     "<button type=\"submit\" name=\"confirm\" "
                     "value=\"1\">Confirm</button>") != NULL &&
              strstr(page,
                     "<button type=\"submit\" name=\"cancel\" "
                     "value=\"1\">Cancel</button>") != NULL &&
              strstr(page, "<h3>Pending import</h3>") != NULL &&
              strstr(page, ", token 4242") != NULL &&
              strstr(page, ", expires in 90000") != NULL &&
              strstr(page,
                     " ticks; single-use. The certificate is served after "
                     "the next reboot.</p>") != NULL,
          "ITEM-647: the English certificate import form names its fields and "
          "confirmation");
    check_visible_controls_labelled(page, "security-en");


    /* English: the route, account and certificate forms. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h3>Add route</h3>") != NULL &&
              strstr(page, "<h3>Remove route</h3>") != NULL &&
              strstr(page, "<h3>Two-phase confirmation</h3>") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Stage addition</button>") !=
                  NULL &&
              strstr(page,
                     "<button type=\"submit\">Stage removal</button>") !=
                  NULL &&
              strstr(page, "name=\"action\" value=\"add\"") != NULL,
          "ITEM-651: the English route forms are visibly separate and the "
          "remove field names what it removes");
    check_visible_controls_labelled(page, "routing-en");

    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              &view, &confirmation, NULL, NULL, NULL, &account, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "name=\"current\" type=\"password\"") != NULL &&
              strstr(page, "name=\"repeat\" type=\"password\"") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Stage</button>") != NULL,
          "ITEM-509: the English account form is byte for byte the "
          "pre-increment document");
    check_visible_controls_labelled(page, "account-en");

    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "name=\"action\" value=\"import\"") != NULL &&
              strstr(page, "<button>Import</button>") != NULL &&
              strstr(page,
                     "<button name=\"confirm\" value=\"1\">Confirm</button>") !=
                  NULL,
          "ITEM-509: the English certificate form is byte for byte the "
          "pre-increment document");
    check_visible_controls_labelled(page, "certificates-en");


    /* English: the sign-in document with every served block. */
    memset(&session, 0, sizeof(session));
    session.authenticated = true;
    session.session_id = 7U;
    session.principal_id = 9U;
    session.notice = "AUTH_THROTTLED";
    session.lockout_remaining_ticks = 11U;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>Management sign-in</h2>") != NULL &&
              strstr(page,
                     "<label>Login <input type=\"text\" name=\"username\" "
                     "autocomplete=\"username\">") != NULL &&
              strstr(page,
                     "<label>Password <input type=\"password\" "
                     "name=\"password\" "
                     "autocomplete=\"current-password\">") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Sign in</button>") != NULL &&
              strstr(page,
                     "<code>credentials</code>, then sign in here with the "
                     "same login and password.") != NULL &&
              strstr(page,
                     "<code>Authorization: Bearer</code> request header. A "
                     "sign-in answers one <code>HttpOnly</code>, "
                     "<code>Secure</code>, <code>SameSite=Strict</code> "
                     "session cookie with a short lifetime.") != NULL &&
              strstr(page, "Session: session 7, principal 9") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Sign out</button>") != NULL &&
              strstr(page,
                     "<p>Throttle: 11 ticks remaining before another sign-in "
                     "is attempted.</p>") != NULL &&
              strstr(page,
                     "Management API discovery: <code>/api/v1</code>") != NULL,
          "ITEM-509: the English sign-in document is byte for byte the "
          "pre-increment document");
    check_visible_controls_labelled(page, "login-en");


    /* Russian: the same surfaces read the catalog column. */
    view.language = APPLIANCE_LANGUAGE_RU;
    confirmation.mutation = "NETWORK_CONFIG";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK,
              &view, &confirmation, NULL, &network, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL &&
              strstr(page, ">Статический</option>") != NULL &&
              strstr(page, "<button>Подготовить</button>") != NULL &&
              strstr(page, ">Подтвердить</button>") != NULL &&
              strstr(page, ">Отмена</button>") != NULL &&
              strstr(page, "<h3>Ожидает</h3>") != NULL &&
              strstr(page, " токен 4242") != NULL &&
              strstr(page, " мс осталось; одноразовый.</p>") != NULL &&
              strstr(page, ">Static</option>") == NULL &&
              strstr(page, ">Stage</button>") == NULL,
          "ITEM-509: the Russian network form and confirmation come from the "
          "catalog");
    check_visible_controls_labelled(page, "network-ru");


    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>Безопасность</h2>") != NULL &&
              strstr(page, "Импорт сертификата оператора") != NULL &&
              strstr(page, "placeholder=\"сертификат PEM\"") != NULL &&
              strstr(page, "placeholder=\"цепочка PEM (необязательно)\"") !=
                  NULL &&
              strstr(page, "placeholder=\"ключ PEM\"") != NULL &&
              strstr(page, ">Подготовить</button>") != NULL &&
              strstr(page, "<h3>Ожидает импорта</h3>") != NULL &&
              strstr(page, ", токен 4242") != NULL &&
              strstr(page, ", истекает через 90000") != NULL &&
              strstr(page,
                     " тиков; одноразовый. Сертификат начнёт обслуживаться "
                     "после следующей перезагрузки.</p>") != NULL,
          "ITEM-509: the Russian certificate import form comes from the "
          "catalog");
    check_visible_controls_labelled(page, "security-ru");


    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h3>Добавить маршрут</h3>") != NULL &&
              strstr(page, "<h3>Удалить маршрут</h3>") != NULL &&
              strstr(page,
                     "<label>Имя маршрута для удаления <input name=\"name\">") !=
                  NULL &&
              strstr(page,
                     "<button type=\"submit\">Подготовить добавление"
                     "</button>") != NULL &&
              strstr(page,
                     "<button type=\"submit\">Подготовить удаление"
                     "</button>") != NULL,
          "ITEM-651: the Russian route forms are visibly separate and the "
          "remove field names what it removes");
    check_visible_controls_labelled(page, "routing-ru");


    memset(&session, 0, sizeof(session));
    session.authenticated = true;
    session.session_id = 7U;
    session.principal_id = 9U;
    session.notice = "AUTH_THROTTLED";
    session.lockout_remaining_ticks = 11U;
    session.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              &session, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>Вход в управление</h2>") != NULL &&
              strstr(page, "<label>Логин <input") != NULL &&
              strstr(page, "<label>Пароль <input") != NULL &&
              strstr(page, ">Войти</button>") != NULL &&
              strstr(page, ">Выйти</button>") != NULL &&
              strstr(page,
                     "Задайте учётную запись в консоли машины командой "
                     "<code>credentials</code>, затем войдите здесь с тем же "
                     "логином и паролем.") != NULL &&
              strstr(page, "Панель требует сеанса администратора.") != NULL &&
              strstr(page, "Сессия: сессия 7, субъект 9") != NULL &&
              strstr(page, "Ограничение: 11") != NULL &&
              strstr(page,
                     " тиков осталось до следующей попытки входа.") != NULL &&
              strstr(page, "Обнаружение Management API: ") != NULL &&
              strstr(page, "Management sign-in") == NULL,
          "ITEM-509: the Russian sign-in document comes from the catalog");
    check_visible_controls_labelled(page, "login-ru");


    memset(&licenses, 0, sizeof(licenses));
    fill_licenses(&licenses, true, false);
    licenses.reason = "LICENSE_PAYLOAD_INCOMPLETE";
    licenses.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>О системе и лицензиях</h2>") != NULL &&
              strstr(page, "<p>Аппарат <code>") != NULL &&
              strstr(page, "<p>Полезная нагрузка <code>") != NULL &&
              strstr(page, " уведомлений поставлено ") != NULL &&
              strstr(page, " объявлено.</p>") != NULL &&
              strstr(page,
                     "<th>Компонент</th><th>Версия</th><th>Лицензия</th>"
                     "<th>Уведомление</th>") != NULL &&
              strstr(page, "<p>Причина: ") != NULL &&
              strstr(page, "<p>Выбор mbedTLS <code>") != NULL &&
              strstr(page, "About and licenses") == NULL,
          "ITEM-509: the Russian licenses page comes from the catalog");

    memset(&control, 0, sizeof(control));
    control.published = true;
    control.language = APPLIANCE_LANGUAGE_RU;
    control.enabled = "DISABLED";
    control.requested = "requested_at";
    control.requested_at = "2026-10-08T00:00:00Z";
    control.source = "PANEL";
    control.applies = "reboot";
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, &confirmation, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<h2>Управление ACME</h2>") != NULL &&
              strstr(page, "<button>Включить</button>") != NULL &&
              strstr(page, "<button>Отключить</button>") != NULL &&
              strstr(page, "<button>Запросить выпуск</button>") != NULL &&
              strstr(page, ">Подтвердить</button>") != NULL &&
              strstr(page, ">Отмена</button>") != NULL &&
              strstr(page, "запрошено ") != NULL &&
              strstr(page, " источник ") != NULL &&
              strstr(page, ", applies=reboot") != NULL &&
              strstr(page, "ACME control") == NULL,
          "ITEM-509: the Russian ACME control page comes from the catalog");
    check_visible_controls_labelled(page, "acme-ru");


    memset(&control, 0, sizeof(control));
    control.published = false;
    control.reason = "NO_ACME_CONTROL_RECORD";
    control.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "Запись управления ACME не опубликована") != NULL &&
              strstr(page,
                     "она ничего не выдумывает.  Ничего не включено и выпуск "
                     "не запрошен.") != NULL &&
              strstr(page, "NO_ACME_CONTROL_RECORD") != NULL,
          "ITEM-509: the unpublished Russian ACME control page comes from "
          "the catalog");

    /* ITEM-647: the Russian account, certificate and system forms name every
     * control from the Russian column; the English label is absent. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT,
              &view, &confirmation, NULL, NULL, NULL, &account, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<label>Текущий пароль <input") != NULL &&
              strstr(page, "<label>Новый логин <input") != NULL &&
              strstr(page, "<label>Новый пароль <input") != NULL &&
              strstr(page, "<label>Повтор нового пароля <input") != NULL &&
              strstr(page, "Код подтверждения") != NULL &&
              strstr(page, "Current password") == NULL &&
              strstr(page, "New login") == NULL,
          "ITEM-647: the Russian account form names every control");
    check_visible_controls_labelled(page, "account-ru");
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES,
              &view, &confirmation, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<label>Имя сертификата <input") != NULL &&
              strstr(page, "<label>Сертификат PEM<br>") != NULL &&
              strstr(page, "<label>Закрытый ключ PEM<br>") != NULL &&
              strstr(page, "Сертификат по умолчанию") != NULL &&
              strstr(page, "Certificate name") == NULL,
          "ITEM-647: the Russian certificate forms name every control");
    check_visible_controls_labelled(page, "certificates-ru");
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              &view, &confirmation, NULL, NULL, &system, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<label>Имя устройства <input") != NULL &&
              strstr(page, "<label>Язык панели <input") != NULL &&
              strstr(page, "Hostname") == NULL,
          "ITEM-647: the Russian System form names every control");
    check_visible_controls_labelled(page, "system-ru");
}

/*
 * ITEM-513: the nav's one-click language switch.  The owner's decision
 * (2026-10-10) puts the `EN | RU` buttons on every panel page: one POST form
 * with two submit buttons, no script, no hidden field and no token.  The form
 * posts the one field `language=en|ru` to `/panel/language`, whose answer sets
 * the browser's `hfoc_lang` cookie; the buttons' labels come from the catalog
 * like every other word the panel prints, and the values are the catalog's own
 * codes.  The signed-out sign-in shell offers no switch: there is no session to
 * carry it and its index nav stays byte-for-byte as it was.
 */
static unsigned int count_text(const char *haystack, const char *needle)
{
    unsigned int count = 0U;
    size_t needle_length = strlen(needle);
    const char *cursor = haystack;

    if (needle_length == 0U) {
        return 0U;
    }
    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        cursor += needle_length;
    }
    return count;
}

/* True when [range, range + range_length) contains `needle`. */
static bool range_contains(
    const char *range,
    size_t range_length,
    const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0U || needle_length > range_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= range_length; ++index) {
        if (memcmp(range + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_language_switch(void)
{
    static const char switch_form[] =
        "<form method=\"post\" action=\"/panel/language\" "
        "style=\"display:inline\">";
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_licenses licenses;
    struct appliance_management_web_ui_acme_control control;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;

    memset(&view, 0, sizeof(view));
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, switch_form) != NULL &&
              count_text(page, "/panel/language") == 1U &&
              strstr(page,
                     "<button name=\"language\" value=\"en\">EN</button>") !=
                  NULL &&
              strstr(page,
                     "<button name=\"language\" value=\"ru\">RU</button>") !=
                  NULL,
          "ITEM-513: the panel nav carries the one-click EN | RU switch");

    /* The switch form carries no hidden field and no script. */
    {
        const char *open = strstr(page, switch_form);
        const char *close = open != NULL ? strstr(open, "</form>") : NULL;

        CHECK(open != NULL && close != NULL &&
                  !range_contains(open, (size_t)(close - open), "<input") &&
                  strstr(page, "<script") == NULL,
              "ITEM-513: the switch form is two buttons and nothing else");
    }

    /* The Russian page carries the same switch and the Russian chrome. */
    view.language = APPLIANCE_LANGUAGE_RU;
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              &view, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "<html lang=\"ru\">") != NULL &&
              strstr(page, switch_form) != NULL &&
              strstr(page,
                     "<button name=\"language\" value=\"en\">EN</button>") !=
                  NULL &&
              strstr(page,
                     "<button name=\"language\" value=\"ru\">RU</button>") !=
                  NULL,
          "ITEM-513: the switch reads the same in the Russian document");

    /* The licenses page and the ACME control page carry it too. */
    memset(&licenses, 0, sizeof(licenses));
    fill_licenses(&licenses, true, false);
    length = 0U;
    CHECK(appliance_management_web_ui_render_licenses(
              &licenses, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, switch_form) != NULL &&
              count_text(page, "/panel/language") == 1U,
          "ITEM-513: the licenses page carries the switch");
    memset(&control, 0, sizeof(control));
    control.published = true;
    control.enabled = "DISABLED";
    control.requested = "requested";
    control.requested_at = "now";
    control.source = "PANEL";
    control.applies = "reboot";
    length = 0U;
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, switch_form) != NULL &&
              count_text(page, "/panel/language") == 1U,
          "ITEM-513: the ACME control page carries the switch");

    /* A signed-out browser is offered no switch. */
    length = 0U;
    CHECK(appliance_management_web_ui_render_login(
              NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK &&
              strstr(page, "/panel/language") == NULL,
          "ITEM-513: the signed-out sign-in shell offers no switch");
}

/*
 * ITEM-513: the System page draws the bodies the stage reads.  ITEM-509
 * increment 6 drew `hostname` and `language` in one form, but the stage parses
 * exactly one field per body and refuses a second, so the form a browser
 * actually submitted could never pass - the defect this item closes.  The page
 * now draws one stage form per field; this proof walks the rendered document,
 * finds every form that posts to the declared target, and requires each stage
 * form to carry exactly one named field, so the drawn body and the accepted
 * body are the same shape and cannot drift apart again.
 */
static void test_system_form_bodies(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_system system;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    size_t length = 0U;
    unsigned int target_forms = 0U;
    unsigned int hostname_forms = 0U;
    unsigned int language_forms = 0U;
    const char *cursor = NULL;

    memset(&view, 0, sizeof(view));
    memset(&system, 0, sizeof(system));
    system.published = true;
    system.active = "appliance";
    system.saved = "appliance";
    system.source = "COMMITTED";
    system.applies = "reboot";
    length = 0U;
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM,
              &view, NULL, NULL, NULL, &system, NULL, NULL, NULL,
              page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-513: the System page renders for the form walk");

    for (cursor = page;;) {
        const char *open = strstr(cursor, "<form ");
        const char *close;
        size_t form_length;
        bool hostname_input;
        bool language_input;
        bool token_input;

        if (open == NULL) {
            break;
        }
        close = strstr(open, "</form>");
        if (close == NULL) {
            break;
        }
        form_length = (size_t)(close - open);
        if (!range_contains(
                open, form_length, "action=\"/panel/system\"")) {
            cursor = close + 7;
            continue;
        }
        ++target_forms;
        hostname_input =
            range_contains(open, form_length, "name=\"hostname\"");
        language_input =
            range_contains(open, form_length, "name=\"language\"");
        token_input = range_contains(open, form_length, "name=\"token\"");
        if (hostname_input) {
            ++hostname_forms;
        }
        if (language_input) {
            ++language_forms;
        }
        CHECK(!(hostname_input && language_input),
              "ITEM-513: no System form submits both stage fields");
        CHECK(!(hostname_input && token_input) &&
                  !(language_input && token_input),
              "ITEM-513: a stage form is not the confirmation form");
        if (hostname_input || language_input) {
            /* The drawn stage body is exactly one field. */
            unsigned int named_inputs = 0U;

            if (hostname_input) {
                ++named_inputs;
            }
            if (language_input) {
                ++named_inputs;
            }
            CHECK(named_inputs == 1U &&
                      range_contains(open, form_length,
                                     "<button type=\"submit\">"),
                  "ITEM-513: each stage form is one field and one button");
        }
        cursor = close + 7;
    }
    CHECK(target_forms == 3U && hostname_forms == 1U &&
              language_forms == 1U,
          "ITEM-513: the System page draws two one-field stage forms and the "
          "confirmation form");
}

static void test_asset_budget(const char *document_path)
{
    static char document[65536];
    char row[128];
    size_t index;
    size_t length = 0U;

    CHECK(document_path != NULL,
          "management web UI asset budget needs the documentation path");
    if (document_path == NULL) {
        return;
    }
    CHECK(read_file(document_path, document, sizeof(document)),
          "management web UI asset budget documentation is readable");
    if (failures != 0) {
        return;
    }

    CHECK(appliance_management_web_ui_page_budget(
              APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LOGIN) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN &&
          appliance_management_web_ui_page_budget(
              APPLIANCE_MANAGEMENT_WEB_UI_PAGE_UPDATES) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_UPDATES &&
          appliance_management_web_ui_page_budget(
              APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES &&
          appliance_management_web_ui_page_budget(
              APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME) ==
              APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACME &&
          appliance_management_web_ui_page_budget(
              (enum appliance_management_web_ui_page)99U) == 0U,
          "management web UI page budget table is exact");

    CHECK(appliance_management_web_ui_render_login(
              NULL,
              (char *)measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LOGIN],
              APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "management web UI measures the login page");

    /*
     * ITEM-509 increment 12: the served sign-in document is not only the
     * anonymous shell.  An authenticated browser also gets the sign-out form,
     * the session pair, a notice and the throttle line, and the page renders
     * in the machine's language; the recorded budget covers the longest of
     * those shapes in both languages, not the shell alone.
     */
    {
        struct appliance_management_web_ui_session session;
        char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
        enum appliance_language language;

        for (language = APPLIANCE_LANGUAGE_EN;
             language < APPLIANCE_LANGUAGE_COUNT;
             ++language) {
            size_t page_length = 0U;

            memset(&session, 0, sizeof(session));
            session.authenticated = true;
            session.session_id = UINT32_MAX;
            session.principal_id = UINT32_MAX;
            session.notice = hostile_values[0];
            session.lockout_remaining_ticks = UINT64_MAX;
            session.language = language;
            CHECK(appliance_management_web_ui_render_login(
                      &session, page, sizeof(page), &page_length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI measures the signed-in login page");
            if (page_length >
                strlen((const char *)measured[
                    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LOGIN])) {
                memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LOGIN],
                       page, page_length + 1U);
            }
        }
    }

    /*
     * ITEM-382: the licenses page is measured with hostile owner values, so its
     * recorded budget covers the worst case of the document shape too.
     */
    {
        struct appliance_management_web_ui_licenses licenses;

        fill_licenses(&licenses, true, false);
        licenses.reason = hostile_values[0];
        licenses.manifest = hostile_values[1];
        licenses.payload_root = hostile_values[2];
        length = 0U;
        CHECK(appliance_management_web_ui_render_licenses(
                  &licenses,
                  (char *)measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES],
                  APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
                  &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures the licenses page");
        /*
         * ITEM-509 increment 12: the same worst case in Russian, whose prose
         * is the longer column, so the recorded budget covers the language the
         * machine can select and not only the default one.
         */
        {
            char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
            size_t page_length = 0U;

            licenses.language = APPLIANCE_LANGUAGE_RU;
            CHECK(appliance_management_web_ui_render_licenses(
                      &licenses, page, sizeof(page), &page_length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI measures the Russian licenses page");
            if (page_length >
                strlen((const char *)measured[
                    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES])) {
                memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES],
                       page, page_length + 1U);
            }
        }
    }

    /*
     * ITEM-508 increment 34: the ACME control page is measured with every value
     * at the bound the renderer enforces and a pending confirmation, so the
     * recorded budget covers the worst case of the page that carries the
     * setting next to four forms.
     */
    {
        struct appliance_management_web_ui_acme_control control;
        struct appliance_management_web_ui_confirmation confirmation;
        static char longest_acme_control[
            APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX];
        static char unpublished[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
        size_t unpublished_length = 0U;

        /*
         * The unpublished document is the first boot of a fresh machine; it is
         * shorter than the published one, so both are measured and the longer
         * rendering is the recorded budget.
         */
        memset(&control, 0, sizeof(control));
        control.published = false;
        control.reason = "NO_ACME_CONTROL_RECORD";
        CHECK(appliance_management_web_ui_render_acme_control(
                  &control,
                  NULL,
                  unpublished,
                  sizeof(unpublished),
                  &unpublished_length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                  strstr(unpublished, "NO_ACME_CONTROL_RECORD") != NULL,
              "management web UI renders the unpublished ACME control page");
        memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME], unpublished,
               unpublished_length + 1U);

        memset(longest_acme_control, 'a', sizeof(longest_acme_control) - 1U);
        memset(&control, 0, sizeof(control));
        control.published = true;
        control.enabled = longest_acme_control;
        control.requested = longest_acme_control;
        control.requested_at = longest_acme_control;
        control.source = longest_acme_control;
        control.applies = longest_acme_control;
        memset(&confirmation, 0, sizeof(confirmation));
        confirmation.pending = true;
        confirmation.token = UINT64_MAX;
        confirmation.mutation = "ACCOUNT_CREDENTIAL";
        confirmation.remaining_ticks = 300000U;
        confirmation.single_use = true;
        length = 0U;
        CHECK(appliance_management_web_ui_render_acme_control(
                  &control,
                  &confirmation,
                  (char *)measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME],
                  APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
                  &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures the ACME control page");
        /*
         * ITEM-509 increment 12: both ACME shapes in Russian, where the
         * control prose and the three action buttons are longer; the recorded
         * budget keeps the longer document.
         */
        {
            char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
            size_t page_length = 0U;

            control.language = APPLIANCE_LANGUAGE_RU;
            CHECK(appliance_management_web_ui_render_acme_control(
                      &control, &confirmation, page, sizeof(page),
                      &page_length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI measures the Russian ACME page");
            if (page_length > strlen((const char *)measured[
                    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME])) {
                memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME],
                       page, page_length + 1U);
            }
        }
        {
            char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
            size_t page_length = 0U;

            control.published = false;
            CHECK(appliance_management_web_ui_render_acme_control(
                      &control, NULL, page, sizeof(page), &page_length) ==
                      APPLIANCE_MANAGEMENT_WEB_UI_OK,
                  "management web UI measures the unpublished Russian ACME "
                  "page");
            if (page_length > strlen((const char *)measured[
                    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME])) {
                memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME],
                       page, page_length + 1U);
            }
        }
    }

    /*
     * ITEM-516: the management-gate page is measured with every value at the
     * bound the renderer enforces, a refusal and a pending confirmation, in
     * both languages; the recorded budget covers the worst case of the page
     * that carries the warning next to the close-only forms.
     */
    {
        struct appliance_management_web_ui_gate gate;
        struct appliance_management_web_ui_confirmation confirmation;
        static char longest_gate[APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX];
        char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
        size_t page_length = 0U;

        memset(&gate, 0, sizeof(gate));
        gate.published = false;
        gate.reason = "NO_SYSTEM_RECORD";
        CHECK(appliance_management_web_ui_render_gate(
                  &gate, NULL, page, sizeof(page), &page_length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                  strstr(page, "NO_SYSTEM_RECORD") != NULL,
              "management web UI renders the unpublished gate page");
        memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE], page,
               page_length + 1U);

        memset(longest_gate, 'g', sizeof(longest_gate) - 1U);
        memset(&gate, 0, sizeof(gate));
        gate.published = true;
        gate.setting = longest_gate;
        gate.source = longest_gate;
        gate.applies = longest_gate;
        gate.refusal = "GATE_LOCAL_SECRET_REQUIRED";
        memset(&confirmation, 0, sizeof(confirmation));
        confirmation.pending = true;
        confirmation.token = UINT64_MAX;
        confirmation.mutation = "PANEL_GATE";
        confirmation.remaining_ticks = 300000U;
        confirmation.single_use = true;
        length = 0U;
        CHECK(appliance_management_web_ui_render_gate(
                  &gate, &confirmation,
                  (char *)measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE],
                  APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
                  &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures the gate page");
        check_visible_controls_labelled(
            (const char *)measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE],
            "gate");
        CHECK(strstr((const char *)measured[
                         APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE],
                     "<label>Confirmation code <input name=\"token\"") != NULL,
              "ITEM-647: the English gate confirmation names its code field");
        gate.language = APPLIANCE_LANGUAGE_RU;
        page_length = 0U;
        CHECK(appliance_management_web_ui_render_gate(
                  &gate, &confirmation, page, sizeof(page),
                  &page_length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures the Russian gate page");
        if (page_length > strlen((const char *)measured[
                APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE])) {
            memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE],
                   page, page_length + 1U);
        }
        gate.language = APPLIANCE_LANGUAGE_DEFAULT;
        gate.published = false;
        page_length = 0U;
        CHECK(appliance_management_web_ui_render_gate(
                  &gate, NULL, page, sizeof(page), &page_length) ==
                  APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures the unpublished gate page in Russian");
        if (page_length > strlen((const char *)measured[
                APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE])) {
            memcpy(measured[APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE],
                   page, page_length + 1U);
        }
    }


    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        enum appliance_management_web_ui_page page =
            (enum appliance_management_web_ui_page)(index + 1U);
        struct appliance_management_web_ui_view view;
        struct appliance_management_web_ui_confirmation confirmation;
        struct appliance_management_web_ui_routes routes;
        struct appliance_management_web_ui_network network;
        struct appliance_management_web_ui_system system;
        struct appliance_management_web_ui_account account;
        struct appliance_management_web_ui_certificates certificates;
        struct appliance_management_web_ui_help help;
        const struct appliance_management_web_ui_routes *routes_arg = NULL;
        const struct appliance_management_web_ui_network *network_arg = NULL;
        const struct appliance_management_web_ui_system *system_arg = NULL;
        const struct appliance_management_web_ui_account *account_arg = NULL;
        const struct appliance_management_web_ui_certificates
            *certificates_arg = NULL;
        const struct appliance_management_web_ui_help *help_arg = NULL;

        fill_view(&view, index);
        memset(&confirmation, 0, sizeof(confirmation));
        confirmation.pending =
            appliance_management_web_ui_section_confirmable(
                (enum appliance_management_web_ui_section)index);
        /*
         * The measured case is the worst case: the longest token the plane can
         * issue, the longest remaining window, the longest mutation name any
         * confirmable page can render (the plane's pending candidate is shown
         * on every confirmable page) and - for the Routing page - a full
         * bounded table.  ITEM-506 increment 6 replaced the previous "OWNED"
         * placeholder, which under-measured every pending page.
         */
        confirmation.token = UINT64_MAX;
        confirmation.mutation = "ACCOUNT_CREDENTIAL";
        confirmation.remaining_ticks = 300000U;
        confirmation.single_use = true;
        /*
         * ITEM-504: the Routing page is measured with a full bounded table and
         * a pending candidate, so the recorded budget covers the worst case of
         * the one page that lists a value instead of a form alone.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING) {
            memset(&routes, 0, sizeof(routes));
            routes.published = true;
            routes.count = APPLIANCE_ROUTE_TABLE_MAX;
            routes.shown = APPLIANCE_ROUTE_TABLE_MAX;
            memset(routes.text, 'n', sizeof(routes.text) - 2U);
            routes.text[sizeof(routes.text) - 2U] = '\n';
            routes_arg = &routes;
        }
        /*
         * ITEM-505: the Network page is measured with both readings at the
         * longest rendering the owner can publish (a static configuration
         * with every octet at 255) and a pending candidate, so the recorded
         * budget covers the page that carries two values next to its forms.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK) {
            static const char longest_active[] =
                "mode=STATIC address=255.255.255.255/32 "
                "gateway=255.255.255.255 dns=255.255.255.255";

            memset(&network, 0, sizeof(network));
            network.published = true;
            network.active = longest_active;
            network.saved = longest_active;
            network.source = "DEFAULT_INVALID";
            network.applies = "reboot";
            network_arg = &network;
        }
        /*
         * ITEM-505 increment 3: the System page is measured with both
         * hostnames at the longest value the configuration model accepts and
         * a pending candidate, so the recorded budget covers the page that
         * carries two values next to its forms.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM) {
            static char longest_hostname[
                APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];

            memset(longest_hostname, 'n',
                   sizeof(longest_hostname) - 1U);
            memset(&system, 0, sizeof(system));
            system.published = true;
            system.active = longest_hostname;
            system.saved = longest_hostname;
            system.source = "DEFAULT_INVALID";
            system.applies = "reboot";
            system_arg = &system;
        }
        /*
         * ITEM-506 increment 6: the Account page is measured with the longest
         * login the credential policy accepts and a pending candidate.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT) {
            static char longest_login[
                APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];

            memset(longest_login, 'n', sizeof(longest_login) - 1U);
            memset(&account, 0, sizeof(account));
            account.published = true;
            account.login = longest_login;
            account_arg = &account;
        }
        /*
         * ITEM-508 increment 32: the Dashboard carries the read-only ACME
         * block, so its page is measured with the longest value the block can
         * be handed; the renderer bounds each ACME value, and the budget below
         * covers the bounded worst case.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD) {
            static char longest_acme[
                APPLIANCE_MANAGEMENT_WEB_UI_ACME_TEXT_MAX];

            memset(longest_acme, 'a', sizeof(longest_acme) - 1U);
            memset(&certificates, 0, sizeof(certificates));
            certificates.published = true;
            certificates.acme.published = true;
            certificates.acme.state = longest_acme;
            certificates.acme.account = longest_acme;
            certificates.acme.renewal = longest_acme;
            certificates.acme.expires = longest_acme;
            certificates.acme.attempts = longest_acme;
            /* ITEM-508 increment 37: the order value is measured too. */
            certificates.acme.order = longest_acme;
            certificates_arg = &certificates;
        }
        /*
         * ITEM-507 increment 5: the Certificates page is measured with both
         * table renderings at the longest value the owner can publish and a
         * pending candidate, so the recorded budget covers the page that
         * carries two lists next to four forms.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES) {
            static char longest_names[
                APPLIANCE_MANAGEMENT_WEB_UI_CERTIFICATES_TEXT_MAX];

            memset(longest_names, 'n', sizeof(longest_names) - 1U);
            memset(&certificates, 0, sizeof(certificates));
            certificates.published = true;
            certificates.count = APPLIANCE_CERTIFICATE_NAME_MAX_COUNT;
            certificates.shown = APPLIANCE_CERTIFICATE_NAME_MAX_COUNT;
            certificates.active = longest_names;
            certificates.active_default = "CERTIFICATE";
            certificates.active_source = "DEFAULT_UNAVAILABLE";
            certificates.saved = longest_names;
            certificates.saved_default = "CERTIFICATE";
            certificates.saved_source = "DEFAULT_UNAVAILABLE";
            certificates.applies = "reboot";
            certificates_arg = &certificates;
        }
        /*
         * ITEM-509 increment 8: the Help section's budget covers both shapes
         * the section serves - the table of contents and a topic page - so the
         * measurement uses the Russian catalog, whose text is the longest in
         * the document, and keeps the longer of the renderings.
         */
        if ((enum appliance_management_web_ui_section)index ==
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
            memset(&help, 0, sizeof(help));
            help.published = true;
            help.language = "ru";
            help_arg = &help;
        }
        length = 0U;
        CHECK(appliance_management_web_ui_render_section(
                  (enum appliance_management_web_ui_section)index,
                  &view,
                  &confirmation,
                  routes_arg, network_arg, system_arg, account_arg,
                    certificates_arg,
                  help_arg,
                  (char *)measured[page],
                  APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX,
                  &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
              "management web UI measures every section page");
        /*
         * ITEM-505 increment 2: measure the same page with the worst case the
         * live production view can publish, and record the longer rendering.
         * A page whose budget only covered the short fixture values would
         * overflow the moment a real appliance published its own hostname and
         * MAC.
         */
        {
            struct appliance_production_panel_view_storage production;
            char production_page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
            size_t production_length = 0U;

            fill_worst_case_view(&production);
            if (appliance_management_web_ui_render_section(
                    (enum appliance_management_web_ui_section)index,
                    &production.view,
                    &confirmation,
                    routes_arg, network_arg, system_arg, account_arg,
                    certificates_arg,
                    help_arg,
                    production_page, sizeof(production_page),
                    &production_length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                production_length > length) {
                memcpy(measured[page], production_page,
                       production_length + 1U);
                length = production_length;
            }
            /*
             * ITEM-509 increment 11: the same worst case in Russian.  The read
             * pages now render their prose from the catalog, and the Russian
             * column is longer than the English on several of them, so the
             * recorded budget must cover the language the machine can actually
             * select, not only the default one.
             */
            production.view.language = APPLIANCE_LANGUAGE_RU;
            production_length = 0U;
            if (appliance_management_web_ui_render_section(
                    (enum appliance_management_web_ui_section)index,
                    &production.view,
                    &confirmation,
                    routes_arg, network_arg, system_arg, account_arg,
                    certificates_arg,
                    help_arg,
                    production_page, sizeof(production_page),
                    &production_length) == APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                production_length > length) {
                memcpy(measured[page], production_page,
                       production_length + 1U);
                length = production_length;
            }
            /*
             * ITEM-509 increment 8: the longest topic page is part of the
             * Help section's recorded budget, so the measurement walks every
             * topic in the catalog instead of assuming which body is longest.
             */
            if ((enum appliance_management_web_ui_section)index ==
                APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
                size_t topic;

                for (topic = 0U;
                     topic < appliance_help_topic_count();
                     ++topic) {
                    struct appliance_management_web_ui_help topic_help =
                        help;
                    size_t topic_length = 0U;

                    topic_help.topic = appliance_help_topic_id(topic);
                    if (appliance_management_web_ui_render_section(
                            (enum appliance_management_web_ui_section)index,
                            &production.view,
                            &confirmation,
                            routes_arg, network_arg, system_arg, account_arg,
                            certificates_arg,
                            &topic_help,
                            production_page, sizeof(production_page),
                            &topic_length) ==
                            APPLIANCE_MANAGEMENT_WEB_UI_OK &&
                        topic_length > length) {
                        memcpy(measured[page], production_page,
                               topic_length + 1U);
                        length = topic_length;
                    }
                }
            }
        }
    }

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT;
         ++index) {
        const uint32_t bytes =
            (uint32_t)strlen((const char *)measured[index]);
        const uint32_t budget = appliance_management_web_ui_page_budget(
            (enum appliance_management_web_ui_page)index);

        printf("ASSET BUDGET %-12s bytes=%u budget=%u\n",
               page_names[index],
               (unsigned)bytes,
               (unsigned)budget);
        CHECK(budget > 0U &&
                  budget <= APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_MAX &&
                  bytes <= budget,
              "management web UI page stays inside its recorded budget");
        snprintf(row,
                 sizeof(row),
                 "| %s | %u | %u |",
                 page_names[index],
                 (unsigned)bytes,
                 (unsigned)budget);
        CHECK(strstr(document, row) != NULL,
              "management web UI documents the measured asset budget row");
    }
}

int main(int argc, char **argv)
{
    const char *document_path = NULL;

    test_section_strings();
    test_status_strings();
    test_section_ownership();
    test_escape();
    test_shell_body();
    test_login_page();
    test_sections_render();
    test_confirmation();
    test_routing_page();
    test_network_page();
    test_system_page();
    test_account_page();
    test_dashboard_acme_block();
    /* ITEM-509 increment 8: one catalog, the panel's second help renderer. */
    test_help_page();
    /* ITEM-509 increment 10: the document follows the machine's language. */
    test_language_framework();
    /* ITEM-509 increment 12: the forms and the licenses page as well. */
    test_form_language();
    /* ITEM-513: the nav's one-click switch and the System page's one-field
     * stage forms. */
    test_language_switch();
    test_system_form_bodies();
    test_link_integrity();
    test_licenses_page();
    test_acme_control_page();
    if (argc > 1) {
        document_path = argv[1];
    }
    test_asset_budget(document_path);
    if (failures != 0) {
        fprintf(stderr,
                "Appliance management web UI host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Appliance management web UI host test: ALL OK\n");
    return 0;
}
