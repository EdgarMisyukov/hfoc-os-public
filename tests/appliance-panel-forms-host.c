/*
 * ITEM-515: the panel's forms, proven with the bodies a browser sends.
 *
 * ITEM-513 closed this class once for the System page: the panel's proofs
 * posted hand-written bodies while a browser submits every control the page
 * draws, empty ones included, so a form could be drawn that its own stage
 * could never accept.  The network form was the second instance and it reached
 * the hardware.  This proof walks the rendered documents, derives each form's
 * body from the controls actually drawn - a select submits its selected
 * option, a text input its value or the empty string, a checkbox only when
 * checked, a clicked button only when clicked - and drives the parser the
 * serving path drives for that target, so the drawn body and the accepted body
 * cannot drift apart again.
 *
 * The proof is also the named-refusal proof: every network refusal carries the
 * name of the field that failed (`NETWORK_NO_PREFIX`, `NETWORK_EXTRA_FIELD`,
 * ...) instead of one generic word.
 */
#include "appliance_language.h"
#include "appliance_management_web_ui.h"
#include "appliance_panel_gate.h"
#include "appliance_panel_form.h"
#include "appliance_panel_login.h"
#include "appliance_route_config.h"

#include <stdio.h>
#include <string.h>

static unsigned int failures = 0U;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", (message)); \
            ++failures; \
        } \
    } while (0)

#define FORM_MAX_CONTROLS 10U
#define FORM_NAME_MAX 40U
#define FORM_VALUE_MAX 96U
#define FORM_ACTION_MAX 64U

struct drawn_control {
    char name[FORM_NAME_MAX];
    char value[FORM_VALUE_MAX];
    bool is_button;
    bool is_checkbox;
    bool is_select;
    bool checked;
};

struct drawn_form {
    char action[FORM_ACTION_MAX];
    struct drawn_control controls[FORM_MAX_CONTROLS];
    unsigned int count;
};

struct value_override {
    const char *name;
    const char *value;
};

/* ------------------------------------------------------------- the HTML --- */

static bool attribute_value(
    const char *tag,
    const char *tag_end,
    const char *name,
    char *output,
    size_t capacity)
{
    char pattern[48];
    const char *found;
    const char *cursor;
    size_t written = 0U;

    (void)snprintf(pattern, sizeof(pattern), "%s=\"", name);
    found = strstr(tag, pattern);
    if (found == NULL || found >= tag_end) {
        return false;
    }
    cursor = found + strlen(pattern);
    while (cursor < tag_end && *cursor != '"' && written + 1U < capacity) {
        output[written++] = *cursor++;
    }
    if (cursor >= tag_end || *cursor != '"') {
        return false;
    }
    output[written] = '\0';
    return true;
}

static bool range_contains(const char *start, size_t length, const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;

    if (length < needle_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= length; ++index) {
        if (memcmp(start + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static const char *find_tag(
    const char *cursor,
    const char *close,
    const char *name)
{
    char pattern[32];
    const char *found;

    (void)snprintf(pattern, sizeof(pattern), "<%s", name);
    found = strstr(cursor, pattern);
    if (found == NULL || found >= close) {
        return NULL;
    }
    return found;
}

static bool control_from_tag(
    const char *tag,
    const char *tag_end,
    const char *close,
    const char *tag_name,
    struct drawn_control *control)
{
    char type[24];

    memset(control, 0, sizeof(*control));
    if (!attribute_value(tag, tag_end, "name", control->name,
                         sizeof(control->name))) {
        return false;
    }
    control->is_button = strcmp(tag_name, "button") == 0;
    control->is_checkbox = false;
    control->is_select = strcmp(tag_name, "select") == 0;
    control->checked = false;
    if (strcmp(tag_name, "input") == 0) {
        if (attribute_value(tag, tag_end, "type", type, sizeof(type)) &&
            strcmp(type, "checkbox") == 0) {
            control->is_checkbox = true;
        }
        if (range_contains(tag, (size_t)(tag_end - tag), " checked")) {
            control->checked = true;
        }
        (void)attribute_value(tag, tag_end, "value", control->value,
                              sizeof(control->value));
    } else if (control->is_select) {
        const char *option = strstr(tag, "<option ");
        const char *select_close = strstr(tag, "</select>");

        if (select_close != NULL && select_close < close) {
            close = select_close;
        }
        if (option != NULL && option < close) {
            const char *option_end = strchr(option, '>');

            if (option_end != NULL) {
                (void)attribute_value(option, option_end, "value",
                                      control->value,
                                      sizeof(control->value));
            }
        }
    } else {
        (void)attribute_value(tag, tag_end, "value", control->value,
                              sizeof(control->value));
    }
    return true;
}

static bool next_form(
    const char **cursor,
    const char *page_end,
    struct drawn_form *form)
{
    const char *open;
    const char *close;
    const char *scan;

    memset(form, 0, sizeof(*form));
    open = strstr(*cursor, "<form ");
    if (open == NULL || open >= page_end) {
        return false;
    }
    close = strstr(open, "</form>");
    if (close == NULL) {
        return false;
    }
    *cursor = close + 7;
    if (!attribute_value(open, close, "action", form->action,
                         sizeof(form->action))) {
        return next_form(cursor, page_end, form);
    }
    scan = open;
    while (scan < close) {
        const char *tag = NULL;
        const char *tag_name = NULL;
        const char *tag_end;
        static const char *const names[] = {"input", "select", "textarea",
                                            "button"};
        size_t index;

        for (index = 0U; index < 4U; ++index) {
            const char *candidate = find_tag(scan, close, names[index]);

            if (candidate != NULL && (tag == NULL || candidate < tag)) {
                tag = candidate;
                tag_name = names[index];
            }
        }
        if (tag == NULL) {
            break;
        }
        tag_end = strchr(tag, '>');
        if (tag_end == NULL || tag_end >= close) {
            break;
        }
        if (form->count < FORM_MAX_CONTROLS &&
            control_from_tag(tag, tag_end, close, tag_name,
                             &form->controls[form->count])) {
            ++form->count;
        }
        scan = tag_end + 1;
    }
    return true;
}

static const char *override_value(
    const char *name,
    const struct value_override *overrides,
    unsigned int override_count)
{
    unsigned int index;

    for (index = 0U; index < override_count; ++index) {
        if (strcmp(overrides[index].name, name) == 0) {
            return overrides[index].value;
        }
    }
    return NULL;
}

/*
 * The body a browser sends for this form when the named button is clicked:
 * every control a browser submits, in document order, with the override value
 * when the proof fills one in.  Only the first button with that name is
 * submitted, as a browser submits only the clicked one.
 */
static bool build_body(
    const struct drawn_form *form,
    const char *clicked_button,
    const struct value_override *overrides,
    unsigned int override_count,
    char *output,
    size_t capacity)
{
    size_t length = 0U;
    unsigned int index;
    bool clicked = false;

    if (capacity == 0U) {
        return false;
    }
    output[0] = '\0';
    for (index = 0U; index < form->count; ++index) {
        const struct drawn_control *control = &form->controls[index];
        const char *override;
        const char *value;

        if (control->name[0] == '\0') {
            continue;
        }
        if (control->is_button) {
            if (clicked_button == NULL ||
                strcmp(control->name, clicked_button) != 0 || clicked) {
                continue;
            }
            clicked = true;
        }
        override = override_value(control->name, overrides, override_count);
        if (control->is_checkbox && !control->checked && override == NULL) {
            continue;
        }
        value = override != NULL ? override : control->value;
        if (length != 0U) {
            if (length + 1U >= capacity) {
                return false;
            }
            output[length++] = '&';
        }
        if (length + strlen(control->name) + strlen(value) + 1U >= capacity) {
            return false;
        }
        length += (size_t)snprintf(output + length, capacity - length, "%s=%s",
                                   control->name, value);
    }
    return true;
}

static bool form_has_field(const struct drawn_form *form, const char *name)
{
    unsigned int index;

    for (index = 0U; index < form->count; ++index) {
        if (strcmp(form->controls[index].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static const struct drawn_form *find_form(
    const struct drawn_form *forms,
    unsigned int count,
    const char *action,
    bool (*predicate)(const struct drawn_form *))
{
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        if (strcmp(forms[index].action, action) == 0 &&
            (predicate == NULL || predicate(&forms[index]))) {
            return &forms[index];
        }
    }
    return NULL;
}

static unsigned int collect_forms(
    const char *page,
    struct drawn_form *forms,
    unsigned int capacity)
{
    const char *cursor = page;
    const char *end = page + strlen(page);
    unsigned int count = 0U;

    while (count < capacity && next_form(&cursor, end, &forms[count])) {
        ++count;
    }
    return count;
}

static bool form_has_mode(const struct drawn_form *form)
{
    return form_has_field(form, "mode") && form_has_field(form, "address");
}

static bool form_has_token(const struct drawn_form *form)
{
    return form_has_field(form, "token");
}

static bool form_has_hostname(const struct drawn_form *form)
{
    return form_has_field(form, "hostname");
}

static bool form_has_language(const struct drawn_form *form)
{
    return form_has_field(form, "language");
}

static bool form_has_action(const struct drawn_form *form)
{
    return form_has_field(form, "action");
}

static bool form_has_current(const struct drawn_form *form)
{
    return form_has_field(form, "current");
}

static bool form_has_certificate(const struct drawn_form *form)
{
    return form_has_field(form, "certificate");
}

static bool is_declared_target(const char *action)
{
    static const char *const targets[] = {
        "/",
        "/panel/language",
        "/panel/network",
        "/panel/system",
        "/panel/account",
        "/panel/routing",
        "/panel/acme",
        "/panel/certificates",
        "/panel/security",
        /* ITEM-516: the management-gate page's close-only form. */
        "/panel/gate",
        "/panel/logout",
    };
    size_t index;

    for (index = 0U; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        if (strcmp(action, targets[index]) == 0) {
            return true;
        }
    }
    return false;
}

static void check_targets(const struct drawn_form *forms, unsigned int count)
{
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        CHECK(is_declared_target(forms[index].action),
              "ITEM-515: every drawn form posts to a declared panel target");
    }
}

/* ------------------------------------------------------------ the models --- */

static void fill_view(struct appliance_management_web_ui_view *view)
{
    memset(view, 0, sizeof(*view));
    view->language = APPLIANCE_LANGUAGE_EN;
}

static void fill_network(struct appliance_management_web_ui_network *network)
{
    memset(network, 0, sizeof(*network));
    network->published = true;
    network->active = "mode=DHCP";
    network->saved =
        "mode=STATIC address=10.0.2.15/24 gateway=10.0.2.2 dns=10.0.2.3";
    network->source = "COMMITTED";
    network->applies = "reboot";
}

static void fill_system(struct appliance_management_web_ui_system *system)
{
    memset(system, 0, sizeof(*system));
    system->published = true;
    system->active = "appliance";
    system->saved = "appliance";
    system->source = "COMMITTED";
    system->applies = "reboot";
    system->language_active = "en";
    system->language_saved = "en";
}

static void fill_account(struct appliance_management_web_ui_account *account)
{
    memset(account, 0, sizeof(*account));
    account->published = true;
    account->login = "operator";
}

static void fill_routes(struct appliance_management_web_ui_routes *routes)
{
    memset(routes, 0, sizeof(*routes));
    routes->published = true;
    routes->count = 1U;
    routes->shown = 1U;
    (void)snprintf(routes->text, sizeof(routes->text),
                   "name=app backend=10.0.2.9:443 tls=on source=panel\n");
}

static void fill_certificates(
    struct appliance_management_web_ui_certificates *certificates)
{
    memset(certificates, 0, sizeof(*certificates));
    certificates->published = true;
    certificates->count = 1U;
    certificates->shown = 1U;
    certificates->active = "appliance.example (1024)";
    certificates->active_default = "appliance.example";
    certificates->active_source = "COMMITTED";
    certificates->saved = "appliance.example (1024)";
    certificates->saved_default = "appliance.example";
    certificates->saved_source = "COMMITTED";
    certificates->applies = "reboot";
}

static void fill_acme(struct appliance_management_web_ui_acme_control *control)
{
    memset(control, 0, sizeof(*control));
    control->published = true;
    control->enabled = "ENABLED";
    control->requested = "NONE";
    control->requested_at = "0";
    control->source = "service";
    control->applies = "live";
    control->language = APPLIANCE_LANGUAGE_EN;
}

/* ------------------------------------------------- network: drawn bodies --- */

static void test_network_drawn_body(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_network network;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *stage;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[512];
    size_t length = 0U;
    struct appliance_panel_form_network parsed;
    static const struct value_override static_values[] = {
        {"mode", "static"},
        {"address", "10.0.2.15"},
        {"prefix", "24"},
        {"gateway", "10.0.2.2"},
        {"dns", "10.0.2.3"},
    };

    fill_view(&view);
    fill_network(&network);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK, &view, NULL, NULL,
              &network, NULL, NULL, NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Network page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    stage = find_form(forms, count, "/panel/network", form_has_mode);
    CHECK(stage != NULL, "ITEM-515: the Network page draws its stage form");
    if (stage == NULL) {
        return;
    }

    /*
     * The browser body of the drawn form: the select's first option is DHCP
     * and the four optional inputs arrive present and empty.  This literal is
     * the body the hardware sent; before this item the stage refused it.
     */
    CHECK(build_body(stage, NULL, NULL, 0U, body, sizeof(body)) &&
              strcmp(body, "mode=dhcp&address=&prefix=&gateway=&dns=") == 0,
          "ITEM-515: the drawn Network body is the browser body");
    CHECK(appliance_panel_form_network_parse(
              (const uint8_t *)body, (uint32_t)strlen(body), &parsed) ==
              APPLIANCE_PANEL_FORM_OK &&
              parsed.mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
          "ITEM-515: the drawn DHCP body is accepted");

    /* The filled static form, still derived from the drawn controls. */
    CHECK(build_body(stage, NULL, static_values,
                     (unsigned int)(sizeof(static_values) /
                                    sizeof(static_values[0])),
                     body, sizeof(body)) &&
              appliance_panel_form_network_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body), &parsed) ==
                  APPLIANCE_PANEL_FORM_OK &&
              parsed.mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
              parsed.static_config.prefix_length == 24U,
          "ITEM-515: the filled static body is accepted and parsed");

    /* Every field is named on the page, not left to its position. */
    CHECK(range_contains(page, strlen(page), ">Address") &&
              range_contains(page, strlen(page), ">Prefix") &&
              range_contains(page, strlen(page), ">Gateway") &&
              range_contains(page, strlen(page), ">DNS"),
          "ITEM-515: the four network fields carry visible labels");
}

static void test_network_refusal_names(void)
{
    static const struct {
        const char *body;
        enum appliance_panel_form_status status;
        const char *name;
    } cases[] = {
        {"mode=dhcp&address=&prefix=&gateway=&dns=",
         APPLIANCE_PANEL_FORM_OK, "OK"},
        {"mode=dhcp&address=10.0.2.15&prefix=&gateway=&dns=",
         APPLIANCE_PANEL_FORM_EXTRA_FIELD, "NETWORK_EXTRA_FIELD"},
        {"mode=dhcp&address=&prefix=24&gateway=&dns=",
         APPLIANCE_PANEL_FORM_EXTRA_FIELD, "NETWORK_EXTRA_FIELD"},
        {"mode=tunnel&address=&prefix=&gateway=&dns=",
         APPLIANCE_PANEL_FORM_MODE_UNKNOWN, "NETWORK_MODE_UNKNOWN"},
        {"address=10.0.2.15&prefix=24&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_MODE_MISSING, "NETWORK_MODE_MISSING"},
        {"", APPLIANCE_PANEL_FORM_EMPTY_BODY, "NETWORK_EMPTY_BODY"},
        {"mode=static&prefix=24&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_ADDRESS_MISSING, "NETWORK_NO_ADDRESS"},
        {"mode=static&address=10.0.2.15&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_PREFIX_MISSING, "NETWORK_NO_PREFIX"},
        {"mode=static&address=10.0.2.15&prefix=24&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_GATEWAY_MISSING, "NETWORK_NO_GATEWAY"},
        {"mode=static&address=10.0.2.15&prefix=24&gateway=10.0.2.2",
         APPLIANCE_PANEL_FORM_DNS_MISSING, "NETWORK_NO_DNS"},
        {"mode=static&address=not-an-address&prefix=24"
         "&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_ADDRESS_INVALID, "NETWORK_ADDRESS_INVALID"},
        {"mode=static&address=10.0.2.15&prefix=99"
         "&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_PREFIX_INVALID, "NETWORK_PREFIX_INVALID"},
        {"mode=static&address=10.0.2.15&prefix=abc"
         "&gateway=10.0.2.2&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_PREFIX_INVALID, "NETWORK_PREFIX_INVALID"},
        {"mode=static&address=10.0.2.15&prefix=24"
         "&gateway=not-an-address&dns=10.0.2.3",
         APPLIANCE_PANEL_FORM_GATEWAY_INVALID, "NETWORK_GATEWAY_INVALID"},
        {"mode=static&address=10.0.2.15&prefix=24"
         "&gateway=10.0.2.2&dns=not-an-address",
         APPLIANCE_PANEL_FORM_DNS_INVALID, "NETWORK_DNS_INVALID"},
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        struct appliance_panel_form_network parsed;
        enum appliance_panel_form_status status =
            appliance_panel_form_network_parse(
                (const uint8_t *)cases[index].body,
                (uint32_t)strlen(cases[index].body), &parsed);

        CHECK(status == cases[index].status &&
                  strcmp(appliance_panel_form_status_string(status),
                         cases[index].name) == 0,
              "ITEM-515: a network refusal names the field that failed");
        CHECK(strcmp(appliance_panel_form_status_string(status),
                     cases[index].name) == 0,
              "ITEM-515: the refusal string is the named one");
    }
}

/* -------------------------------------------------- system: one field ------ */

static void test_system_drawn_bodies(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_system system;
    struct drawn_form forms[8];
    unsigned int count;
    unsigned int index;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[256];
    size_t length = 0U;
    unsigned int stage_forms = 0U;

    fill_view(&view);
    fill_system(&system);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM, &view, NULL, NULL,
              NULL, &system, NULL, NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the System page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    for (index = 0U; index < count; ++index) {
        const struct drawn_form *form = &forms[index];
        const char *field;
        struct value_override override;
        enum appliance_panel_form_system_field parsed_field =
            APPLIANCE_PANEL_FORM_SYSTEM_NONE;
        const uint8_t *value = NULL;
        uint32_t value_length = 0U;

        if (strcmp(form->action, "/panel/system") != 0) {
            continue;
        }
        field = form_has_hostname(form) ? "hostname"
                                        : (form_has_language(form) ? "language"
                                                                   : NULL);
        if (field == NULL) {
            CHECK(form_has_token(form),
                  "ITEM-515: a System form is a stage form or the "
                  "confirmation form");
            continue;
        }
        ++stage_forms;
        override.name = field;
        override.value = strcmp(field, "hostname") == 0 ? "appliance" : "ru";
        CHECK(build_body(form, NULL, &override, 1U, body, sizeof(body)),
              "ITEM-515: the System stage body is built from the drawn form");
        CHECK(appliance_panel_form_system_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body),
                  &parsed_field, &value, &value_length) ==
                  APPLIANCE_PANEL_FORM_OK &&
                  value_length == strlen(override.value) &&
                  memcmp(value, override.value, value_length) == 0,
              "ITEM-515: the drawn System stage body is accepted");
        CHECK((strcmp(field, "hostname") == 0 &&
               parsed_field == APPLIANCE_PANEL_FORM_SYSTEM_HOSTNAME) ||
                  (strcmp(field, "language") == 0 &&
                   parsed_field == APPLIANCE_PANEL_FORM_SYSTEM_LANGUAGE),
              "ITEM-515: the parsed System field is the drawn one");
    }
    CHECK(stage_forms == 2U,
          "ITEM-515: the System page draws both one-field stage forms");
}

/* ------------------------------------------------------- login/language --- */

static void test_login_drawn_body(void)
{
    struct drawn_form forms[4];
    unsigned int count;
    const struct drawn_form *form;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[256];
    size_t length = 0U;
    struct appliance_panel_login_form parsed;
    static const struct value_override values[] = {
        {"username", "admin"},
        {"password", "operator-secret"},
    };

    CHECK(appliance_management_web_ui_render_login(
              NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the sign-in document renders");
    count = collect_forms(page, forms, 4U);
    form = find_form(forms, count, "/", NULL);
    CHECK(count == 1U && form != NULL &&
              form_has_field(form, "username") &&
              form_has_field(form, "password"),
          "ITEM-515: the sign-in form draws its two fields");
    if (form == NULL) {
        return;
    }
    CHECK(build_body(form, NULL, values,
                     (unsigned int)(sizeof(values) / sizeof(values[0])),
                     body, sizeof(body)) &&
              appliance_panel_login_form_parse(
                  (const uint8_t *)APPLIANCE_PANEL_LOGIN_CONTENT_TYPE,
                  APPLIANCE_PANEL_LOGIN_CONTENT_TYPE_LENGTH,
                  (const uint8_t *)body, (uint32_t)strlen(body), &parsed) ==
                  APPLIANCE_PANEL_LOGIN_OK &&
              parsed.user_present && parsed.password_present &&
              parsed.user_length == strlen("admin") &&
              parsed.password_length == strlen("operator-secret"),
          "ITEM-515: the drawn sign-in body is accepted");
}

static void test_language_drawn_body(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_network network;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *nav;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[128];
    size_t length = 0U;
    unsigned int buttons = 0U;
    unsigned int index;

    fill_view(&view);
    fill_network(&network);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK, &view, NULL, NULL,
              &network, NULL, NULL, NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Network page carries the navigation");
    count = collect_forms(page, forms, 8U);
    nav = find_form(forms, count, "/panel/language", NULL);
    CHECK(nav != NULL, "ITEM-515: the navigation draws its switch form");
    if (nav == NULL) {
        return;
    }
    for (index = 0U; index < nav->count; ++index) {
        const struct drawn_control *control = &nav->controls[index];
        struct value_override override;
        enum appliance_language parsed = APPLIANCE_LANGUAGE_DEFAULT;

        if (!control->is_button || strcmp(control->name, "language") != 0) {
            continue;
        }
        ++buttons;
        /*
         * Each button draws its own `language` value; the built body is the
         * one that button submits, byte for byte.
         */
        override.name = "language";
        override.value = control->value;
        CHECK(build_body(nav, "language", &override, 1U, body, sizeof(body)) &&
                  appliance_panel_login_language_form_parse(
                      (const uint8_t *)body, (uint32_t)strlen(body),
                      &parsed) == APPLIANCE_PANEL_LOGIN_OK &&
                  appliance_language_code(parsed) != NULL &&
                  strcmp(appliance_language_code(parsed), control->value) == 0,
              "ITEM-515: the clicked navigation body is the drawn language");
    }
    CHECK(buttons == 2U,
          "ITEM-515: the navigation draws both language buttons");
}

/* ------------------------------------------------------------ account ----- */

static void test_account_drawn_body(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_account account;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *form;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[512];
    size_t length = 0U;
    struct appliance_panel_account_form parsed;
    static const struct value_override values[] = {
        {"current", "operator-secret"},
        {"login", "operator"},
        {"password", "new-operator-secret"},
        {"repeat", "new-operator-secret"},
    };

    fill_view(&view);
    fill_account(&account);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT, &view, NULL, NULL,
              NULL, NULL, &account, NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Account page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    form = find_form(forms, count, "/panel/account", form_has_current);
    CHECK(form != NULL, "ITEM-515: the Account page draws its stage form");
    if (form == NULL) {
        return;
    }
    CHECK(build_body(form, NULL, values,
                     (unsigned int)(sizeof(values) / sizeof(values[0])),
                     body, sizeof(body)) &&
              appliance_panel_account_form_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body), &parsed) ==
                  APPLIANCE_PANEL_LOGIN_OK &&
              parsed.current_present && parsed.login_present &&
              parsed.password_present && parsed.repeat_present &&
              parsed.password_length == parsed.repeat_length,
          "ITEM-515: the drawn account body is accepted");
}

/* ------------------------------------------------------------ routing ----- */

static void test_routing_drawn_body(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_routes routes;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *form;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[512];
    size_t length = 0U;
    struct appliance_route_config config;
    static const struct value_override tls_on[] = {
        {"name", "app"},
        {"backend", "10.0.2.9"},
        {"port", "443"},
        {"tls", "on"},
    };

    fill_view(&view);
    fill_routes(&routes);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING, &view, NULL,
              &routes, NULL, NULL, NULL, NULL, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Routing page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    form = find_form(forms, count, "/panel/routing", form_has_action);
    CHECK(form != NULL && form_has_field(form, "action"),
          "ITEM-515: the Routing page draws its stage form");
    if (form == NULL) {
        return;
    }
    /*
     * The routing form draws `tls` as a text control whose value is `on`, so
     * the body a browser sends always carries it.  A body that leaves the
     * field out - what a browser submits for an unticked checkbox - is the
     * parser's default and must be accepted too.
     */
    CHECK(build_body(form, NULL, tls_on,
                     (unsigned int)(sizeof(tls_on) / sizeof(tls_on[0])),
                     body, sizeof(body)) &&
              strstr(body, "tls=on") != NULL,
          "ITEM-515: the drawn routing body carries the tls control");
    appliance_route_config_init(&config);
    CHECK(appliance_route_config_stage(
              &config,
              (const uint8_t *)APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE,
              APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH,
              (const uint8_t *)body, (uint32_t)strlen(body)) ==
              APPLIANCE_ROUTE_CONFIG_OK &&
              appliance_route_config_staged(&config),
          "ITEM-515: the drawn routing body is accepted");
    appliance_route_config_rollback(&config);
    CHECK(appliance_route_config_stage(
              &config,
              (const uint8_t *)APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE,
              APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH,
              (const uint8_t *)"action=add&name=app&backend=10.0.2.9&port=443",
              (uint32_t)strlen(
                  "action=add&name=app&backend=10.0.2.9&port=443")) ==
              APPLIANCE_ROUTE_CONFIG_OK &&
              appliance_route_config_staged(&config),
          "ITEM-515: a routing body without tls is accepted");
    appliance_route_config_rollback(&config);
}

/* --------------------------------------------------------------- ACME ----- */

static void test_acme_drawn_bodies(void)
{
    struct appliance_management_web_ui_acme_control control;
    struct drawn_form forms[8];
    unsigned int count;
    unsigned int index;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[128];
    size_t length = 0U;
    unsigned int actions = 0U;

    fill_acme(&control);
    CHECK(appliance_management_web_ui_render_acme_control(
              &control, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the ACME control page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    for (index = 0U; index < count; ++index) {
        const struct drawn_form *form = &forms[index];
        uint8_t action[16];
        uint32_t action_length = 0U;
        const struct drawn_control *field;
        unsigned int control_index;

        if (strcmp(form->action, "/panel/acme") != 0) {
            continue;
        }
        if (!form_has_action(form)) {
            CHECK(form_has_token(form),
                  "ITEM-515: an ACME form carries an action or the token");
            continue;
        }
        ++actions;
        field = NULL;
        for (control_index = 0U; control_index < form->count;
             ++control_index) {
            if (strcmp(form->controls[control_index].name, "action") == 0) {
                field = &form->controls[control_index];
            }
        }
        CHECK(field != NULL && field->value[0] != '\0',
              "ITEM-515: the ACME action form draws its action value");
        CHECK(build_body(form, NULL, NULL, 0U, body, sizeof(body)) &&
                  appliance_panel_form_action_parse(
                      (const uint8_t *)body, (uint32_t)strlen(body), action,
                      (uint32_t)sizeof(action), &action_length) ==
                      APPLIANCE_PANEL_FORM_OK &&
                  action_length == strlen(field->value) &&
                  memcmp(action, field->value, action_length) == 0,
              "ITEM-515: the drawn ACME action body is accepted");
    }
    CHECK(actions == 3U,
          "ITEM-515: the ACME control page draws its three action forms");
}

/* ------------------------------------------------------- management gate -- */

static void test_gate_drawn_bodies(void)
{
    struct appliance_management_web_ui_gate gate;
    struct drawn_form forms[8];
    unsigned int count;
    unsigned int index;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[128];
    size_t length = 0U;
    unsigned int actions = 0U;

    memset(&gate, 0, sizeof(gate));
    gate.published = true;
    gate.setting = "CLOSED_AFTER_BOOT";
    gate.source = "COMMITTED";
    gate.applies = "reboot";
    CHECK(appliance_management_web_ui_render_gate(
              &gate, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-516: the gate page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    /*
     * The page can only ever close: every action form it draws carries the
     * safe word, and no control on the page submits `open`.
     */
    for (index = 0U; index < count; ++index) {
        const struct drawn_form *form = &forms[index];
        uint8_t action[16];
        uint32_t action_length = 0U;
        const struct drawn_control *field;
        unsigned int control_index;

        if (strcmp(form->action, "/panel/gate") != 0) {
            continue;
        }
        if (!form_has_action(form)) {
            CHECK(form_has_token(form),
                  "ITEM-516: a gate form carries the action or the token");
            continue;
        }
        ++actions;
        field = NULL;
        for (control_index = 0U; control_index < form->count;
             ++control_index) {
            if (strcmp(form->controls[control_index].name, "action") == 0) {
                field = &form->controls[control_index];
            }
        }
        CHECK(field != NULL && strcmp(field->value, "close") == 0,
              "ITEM-516: the gate action form can only submit close");
        CHECK(build_body(form, NULL, NULL, 0U, body, sizeof(body)) &&
                  appliance_panel_form_action_parse(
                      (const uint8_t *)body, (uint32_t)strlen(body), action,
                      (uint32_t)sizeof(action), &action_length) ==
                      APPLIANCE_PANEL_FORM_OK &&
                  action_length == 5U &&
                  memcmp(action, "close", 5U) == 0,
              "ITEM-516: the drawn gate body is accepted");
    }
    CHECK(actions == 1U,
          "ITEM-516: the gate page draws exactly one action form");
    CHECK(strstr(page, "value=\"open\"") == NULL,
          "ITEM-516: the gate page draws no control that opens the gate");
    /*
     * The refusal an attempt to open answers with is a stable name: it is
     * what a script compares against and what the panel renders.
     */
    CHECK(strcmp(APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED,
                 "GATE_LOCAL_SECRET_REQUIRED") == 0,
          "ITEM-516: the panel refusal name is stable");
}

/* ------------------------------------------------------- certificates ----- */

static void test_certificates_drawn_bodies(void)
{
    struct appliance_management_web_ui_view view;
    struct appliance_management_web_ui_certificates certificates;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *import_form;
    const struct drawn_form *delete_form;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[512];
    size_t length = 0U;
    uint8_t action[16];
    uint32_t action_length = 0U;
    char certificate_name[32];
    uint32_t certificate_name_length = 0U;
    static const struct value_override import_values[] = {
        {"name", "appliance.example"},
        {"certificate", "CERTIFICATE"},
        {"key", "KEY"},
    };
    static const struct value_override checked_default[] = {
        {"name", "appliance.example"},
        {"certificate", "CERTIFICATE"},
        {"key", "KEY"},
        {"default", "1"},
    };
    static const struct value_override delete_values[] = {
        {"name", "appliance.example"},
    };

    fill_view(&view);
    fill_certificates(&certificates);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES, &view, NULL,
              NULL, NULL, NULL, NULL, &certificates, NULL, page, sizeof(page),
              &length) == APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Certificates page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    import_form = find_form(forms, count, "/panel/certificates",
                            form_has_certificate);
    delete_form = find_form(forms, count, "/panel/certificates",
                            form_has_token);
    CHECK(import_form != NULL && delete_form != NULL,
          "ITEM-515: the Certificates page draws its two forms");
    if (import_form == NULL || delete_form == NULL) {
        return;
    }

    /* The drawn import body: the unchecked `default` box is not submitted. */
    CHECK(build_body(import_form, "action", import_values,
                     (unsigned int)(sizeof(import_values) /
                                    sizeof(import_values[0])),
                     body, sizeof(body)) &&
              strstr(body, "default=") == NULL &&
              appliance_panel_form_action_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body), action,
                  (uint32_t)sizeof(action), &action_length) ==
                  APPLIANCE_PANEL_FORM_OK &&
              action_length == strlen("import") &&
              memcmp(action, "import", action_length) == 0,
          "ITEM-515: the drawn certificate import body is accepted");
    CHECK(appliance_panel_form_text(
              (const uint8_t *)body, (uint32_t)strlen(body), "name",
              (uint8_t *)certificate_name, sizeof(certificate_name),
              &certificate_name_length) &&
              certificate_name_length == strlen("appliance.example"),
          "ITEM-515: the drawn certificate name is readable");
    {
        uint8_t certificate[32];
        uint32_t certificate_length = 0U;
        uint8_t key[16];
        uint32_t key_length = 0U;

        CHECK(appliance_panel_form_text(
                  (const uint8_t *)body, (uint32_t)strlen(body),
                  "certificate", certificate,
                  (uint32_t)sizeof(certificate), &certificate_length) &&
                  certificate_length == strlen("CERTIFICATE") &&
                  appliance_panel_form_text(
                      (const uint8_t *)body, (uint32_t)strlen(body), "key",
                      key, (uint32_t)sizeof(key), &key_length) &&
                  key_length == strlen("KEY"),
              "ITEM-515: the drawn certificate and key are readable");
    }
    CHECK(build_body(import_form, "action", checked_default,
                     (unsigned int)(sizeof(checked_default) /
                                    sizeof(checked_default[0])),
                     body, sizeof(body)) &&
              appliance_panel_form_flag(
                  (const uint8_t *)body, (uint32_t)strlen(body), "default") &&
              appliance_panel_form_action_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body), action,
                  (uint32_t)sizeof(action), &action_length) ==
                  APPLIANCE_PANEL_FORM_OK,
          "ITEM-515: a ticked default checkbox is submitted and accepted");

    /* The delete form stages `action=delete&name=…` and submits the token. */
    CHECK(build_body(delete_form, "confirm", delete_values,
                     (unsigned int)(sizeof(delete_values) /
                                    sizeof(delete_values[0])),
                     body, sizeof(body)) &&
              strstr(body, "action=delete") != NULL &&
              strstr(body, "token=") != NULL &&
              appliance_panel_form_action_parse(
                  (const uint8_t *)body, (uint32_t)strlen(body), action,
                  (uint32_t)sizeof(action), &action_length) ==
                  APPLIANCE_PANEL_FORM_OK &&
              action_length == strlen("delete") &&
              memcmp(action, "delete", action_length) == 0,
          "ITEM-515: the drawn certificate delete body is accepted");
}

/* ------------------------------------------------------------ security ---- */

static void test_security_drawn_fields(void)
{
    struct appliance_management_web_ui_view view;
    struct drawn_form forms[8];
    unsigned int count;
    const struct drawn_form *form;
    char page[APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX];
    char body[512];
    size_t length = 0U;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;

    fill_view(&view);
    CHECK(appliance_management_web_ui_render_section(
              APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY, &view, NULL, NULL,
              NULL, NULL, NULL, NULL, NULL, page, sizeof(page), &length) ==
              APPLIANCE_MANAGEMENT_WEB_UI_OK,
          "ITEM-515: the Security page renders");
    count = collect_forms(page, forms, 8U);
    check_targets(forms, count);
    form = find_form(forms, count, "/panel/security",
                     form_has_certificate);
    CHECK(form != NULL, "ITEM-515: the Security page draws its import form");
    if (form == NULL) {
        return;
    }
    /*
     * The browser body carries the drawn textareas even when empty; the chain
     * is optional, so `chain=` is an accepted drawn value.  The full upload
     * parse of this body is `test-appliance-certificate-upload-host`, which
     * owns the certificate fixtures.
     */
    CHECK(build_body(form, NULL, NULL, 0U, body, sizeof(body)) &&
              strstr(body, "certificate=&chain=&key=") != NULL,
          "ITEM-515: the drawn Security body carries all three textareas");
    CHECK(appliance_panel_form_field(
              (const uint8_t *)body, (uint32_t)strlen(body), "chain", &value,
              &value_length) &&
              value_length == 0U,
          "ITEM-515: the empty optional chain is readable as empty");
}

int main(void)
{
    test_network_drawn_body();
    test_network_refusal_names();
    test_system_drawn_bodies();
    test_login_drawn_body();
    test_language_drawn_body();
    test_account_drawn_body();
    test_routing_drawn_body();
    test_acme_drawn_bodies();
    test_certificates_drawn_bodies();
    test_security_drawn_fields();
    test_gate_drawn_bodies();

    if (failures != 0U) {
        fprintf(stderr, "appliance-panel-forms-host: %u failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance-panel-forms-host: OK\n");
    return 0;
}
