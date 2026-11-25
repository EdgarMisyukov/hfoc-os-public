#include "appliance_management_web_ui.h"

#include "appliance_language.h"
#include "appliance_license_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * HTML renderer that never allocates.
 *
 * Every append goes through a bounded buffer that owns no storage, keeps the
 * output NUL terminated, and latches an overflow flag instead of writing out
 * of bounds. Dynamic values are escaped before they are appended, so the
 * panel cannot be used to inject markup into the management origin.
 */

struct appliance_management_web_ui_buffer {
    char *output;
    size_t capacity;
    size_t length;
    bool overflow;
    /*
     * ITEM-509 increment 10: a key the catalog cannot answer in the requested
     * language latches this flag, and the renderer answers with the named
     * `APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION` instead of publishing a
     * half-translated document.  It is not an overflow: the buffer is intact,
     * the text simply must not be invented.
     */
    bool missing_translation;
};

static void appliance_management_web_ui_buffer_init(
    struct appliance_management_web_ui_buffer *buffer,
    char *output,
    size_t capacity)
{
    buffer->output = output;
    buffer->capacity = capacity;
    buffer->length = 0U;
    buffer->overflow = capacity == 0U || output == NULL;
    buffer->missing_translation = false;
    if (!buffer->overflow) {
        output[0] = '\0';
    }
}

static void appliance_management_web_ui_buffer_byte(
    struct appliance_management_web_ui_buffer *buffer,
    char value)
{
    if (buffer->overflow) {
        return;
    }
    if (buffer->length + 1U >= buffer->capacity) {
        buffer->overflow = true;
        buffer->length = buffer->capacity - 1U;
        buffer->output[buffer->length] = '\0';
        return;
    }
    buffer->output[buffer->length] = value;
    ++buffer->length;
    buffer->output[buffer->length] = '\0';
}

static void appliance_management_web_ui_buffer_text(
    struct appliance_management_web_ui_buffer *buffer,
    const char *text)
{
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        appliance_management_web_ui_buffer_byte(buffer, *text);
        ++text;
    }
}

/*
 * ITEM-509 increment 10: append one catalog value in the requested language.
 * A key the catalog cannot answer - an unknown key, an empty translation or a
 * language outside the enum - latches `missing_translation`; the caller's
 * renderer then refuses the whole page by name.  The catalog value is trusted
 * text that carries no markup (the host proof checks that), so it is appended
 * as it is; dynamic values keep going through the escapers.
 */
static void appliance_management_web_ui_buffer_language(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const char *key)
{
    const char *text = appliance_language_string(language, key);

    if (text == NULL) {
        buffer->missing_translation = true;
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, text);
}

static void appliance_management_web_ui_buffer_decimal(
    struct appliance_management_web_ui_buffer *buffer,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U);

    while (count != 0U) {
        --count;
        appliance_management_web_ui_buffer_byte(buffer, digits[count]);
    }
}

static void appliance_management_web_ui_buffer_hex_decimal(
    struct appliance_management_web_ui_buffer *buffer,
    uint8_t value)
{
    static const char prefix[] = "&#";

    if (value >= 100U) {
        appliance_management_web_ui_buffer_text(buffer, prefix);
        appliance_management_web_ui_buffer_decimal(buffer, value);
        appliance_management_web_ui_buffer_text(buffer, ";");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, prefix);
    appliance_management_web_ui_buffer_decimal(buffer, value);
    appliance_management_web_ui_buffer_text(buffer, ";");
}

enum appliance_management_web_ui_status
appliance_management_web_ui_escape(
    const char *input,
    size_t input_length,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    size_t index;

    if (input == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    for (index = 0U; index < input_length; ++index) {
        const unsigned char value = (unsigned char)input[index];

        switch (value) {
        case (unsigned char)'&':
            appliance_management_web_ui_buffer_text(&buffer, "&amp;");
            break;
        case (unsigned char)'<':
            appliance_management_web_ui_buffer_text(&buffer, "&lt;");
            break;
        case (unsigned char)'>':
            appliance_management_web_ui_buffer_text(&buffer, "&gt;");
            break;
        case (unsigned char)'"':
            appliance_management_web_ui_buffer_text(&buffer, "&quot;");
            break;
        case (unsigned char)'\'':
            appliance_management_web_ui_buffer_text(&buffer, "&#39;");
            break;
        default:
            if (value < 0x20U || value == 0x7FU) {
                appliance_management_web_ui_buffer_hex_decimal(
                    &buffer, value);
            } else {
                appliance_management_web_ui_buffer_byte(
                    &buffer, (char)value);
            }
            break;
        }
    }
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}

/*
 * ITEM-509 increment 8: escape a bounded span byte by byte into the page
 * buffer.  A catalog body is rendered line by line without being copied into a
 * temporary, and the one-byte escape buffer is large enough for the longest
 * expansion the escaper produces (`&#NN;`).
 */
static void appliance_management_web_ui_buffer_escaped_span(
    struct appliance_management_web_ui_buffer *buffer,
    const char *text,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        const char single[2] = { text[index], '\0' };
        char escaped[7];
        size_t escaped_length = 0U;

        if (appliance_management_web_ui_escape(
                single,
                1U,
                escaped,
                sizeof(escaped),
                &escaped_length) != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            buffer->overflow = true;
            return;
        }
        appliance_management_web_ui_buffer_text(buffer, escaped);
    }
}

/*
 * Escape one bounded dynamic value directly into the page buffer. A null
 * value is published as the literal `unavailable`: a section is never hidden
 * and a missing value is never silently dropped.  `bound` is the caller's
 * value bound; the generic sections use the renderer's own
 * `APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX`, while a block that renders several
 * values on a nearly full page (the Dashboard's ACME state, ITEM-508
 * increment 32) names a smaller bound so the page cannot overflow.  The
 * bounded helper itself is the ITEM-504 one below; it is declared here so the
 * generic value renderer can share it.
 */
static void appliance_management_web_ui_buffer_value_bounded(
    struct appliance_management_web_ui_buffer *buffer,
    const char *value,
    size_t max_characters);

static void appliance_management_web_ui_buffer_value(
    struct appliance_management_web_ui_buffer *buffer,
    const char *value)
{
    appliance_management_web_ui_buffer_value_bounded(
        buffer, value, (size_t)APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX);
}

/*
 * ITEM-509 increment 10: the section names are catalog keys, not literals.
 *
 * Until this increment the renderer carried its own `section_names[]` table
 * next to the catalog's `panel.section.*` entries - two sources of truth for
 * the same thirteen words, the ITEM-499 failure mode in a different place.
 * The table now holds only the keys; `section_string()`, the navigation and the
 * page heading all read the one catalog through them, so the English document
 * is byte-for-byte what it was (the English values are the old literals) and
 * the Russian document cannot name a section differently from the navigation.
 */
static const char *const
appliance_management_web_ui_section_keys[] = {
    "panel.section.dashboard",
    "panel.section.network",
    "panel.section.services",
    "panel.section.storage",
    "panel.section.system",
    "panel.section.security",
    "panel.section.diagnostics",
    "panel.section.updates",
    "panel.section.recovery",
    /* ITEM-504: the first section whose page configures instead of reads. */
    "panel.section.routing",
    /* ITEM-506 increment 6: the one account's credential change. */
    "panel.section.account",
    /* ITEM-507 increment 5: the name -> certificate/key declarations. */
    "panel.section.certificates",
    /*
     * ITEM-509 increment 8: the help index, added to the catalog in
     * increment 10 when the navigation started reading labels from it.
     */
    "panel.section.help",
};

/*
 * ITEM-513: the two nav buttons that switch the browser's language.  The
 * labels are catalog entries like every other word the panel prints; the
 * values are the catalog's own codes (`en`/`ru`), written by the renderer
 * through `appliance_language_code()`, so the button a browser submits and the
 * value the cookie reader accepts cannot drift.
 */
static const char *const
appliance_management_web_ui_language_keys[] = {
    "panel.language.button_en",
    "panel.language.button_ru",
};

static const char *const
appliance_management_web_ui_section_paths[] = {
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
    "/panel/account",
    "/panel/certificates",
    /* ITEM-509 increment 8: the help index; topic pages are `/panel/help/<id>`. */
    APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH,
};

static const char *const
appliance_management_web_ui_section_owners[] = {
    "/api/v1",
    "/api/v1/network",
    "/api/v1/service",
    NULL,
    "/api/v1/system",
    NULL,
    NULL,
    "/api/v1/system/update",
    "/api/v1/system/recovery",
    /*
     * ITEM-504: routing publishes no read-side owner path.  The applied table
     * is the section's own state and the page lists it; the mutation target
     * `/api/v1/system/routes` is a write surface, not a read owner.
     */
    NULL,
    /*
     * ITEM-506 increment 6: the account publishes no read-side owner path.
     * The stored login is the section's own state and the page reads it; the
     * mutation target `/api/v1/system/account` is a write surface, not a read
     * owner.
     */
    NULL,
    /*
     * ITEM-507 increment 5: the certificates page publishes no read-side
     * owner path.  The two tables are the section's own state and the page
     * reads them; the mutation target `/api/v1/system/certificate/name` is a
     * write surface, not a read owner.
     */
    NULL,
    /*
     * ITEM-509 increment 8: the help content is compiled into this machine,
     * so the page is its own read owner and no management API path exists for
     * it; `write_state` says so instead of naming a missing owner.
     */
    NULL,
};

static const enum appliance_management_web_ui_section_state
appliance_management_web_ui_section_states[] = {
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
    /*
     * ITEM-509 increment 8: Help is OWNED even though it names no management
     * API owner, because the catalog it renders is compiled into this build;
     * `write_state` explains that instead of claiming a missing read side.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED,
};

static const bool appliance_management_web_ui_confirmable_table[] = {
    false,
    true,
    true,
    false,
    /* ITEM-505 increment 3: System confirms the hostname mutation. */
    true,
    /* ITEM-503: Security confirms the BYO certificate import. */
    true,
    false,
    true,
    true,
    /* ITEM-504: Routing confirms the route-table mutation. */
    true,
    /* ITEM-506 increment 6: Account confirms the credential change. */
    true,
    /* ITEM-507 increment 5: Certificates confirms the name mutation. */
    true,
    /* ITEM-509 increment 8: Help has no mutation surface. */
    false,
};

static const uint32_t appliance_management_web_ui_page_budgets[] = {
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_NETWORK,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SERVICES,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_STORAGE,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SECURITY,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DIAGNOSTICS,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_UPDATES,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_RECOVERY,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ROUTING,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACCOUNT,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_CERTIFICATES,
    /* ITEM-509 increment 8: the help index and its topic pages. */
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES,
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACME,
    /* ITEM-516: the management-gate page. */
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_GATE,
};

static const char appliance_management_web_ui_login_body[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>HFOC Appliance</title>\n"
    "<style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:60rem;line-height:1.45}nav a{margin-right:.6rem}code{background:#eee;padding:0 .2rem}.state{font-weight:700}.notice{color:#a00;font-weight:700}footer{margin-top:2rem;color:#555;font-size:.9rem}</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>HFOC Appliance</h1>\n"
    "<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>\n"
    "<main>\n"
    "<h2>Management sign-in</h2>\n"
    "<p class=\"state\">SIGNED OUT</p>\n"
    "<form method=\"post\" action=\"/\">\n"
    "<p><label>Login <input type=\"text\" name=\"username\" autocomplete=\"username\"></label></p>\n"
    "<p><label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\"></label></p>\n"
    "<p><button type=\"submit\">Sign in</button></p>\n"
    "</form>\n"
    "<p>Set the account on the machine console with <code>credentials</code>, then sign in here with the same login and password.</p>\n"
    "<p>The panel requires an administrator session. Sign in above with the login and password set on the local console, or present the issued token in the <code>Authorization: Bearer</code> request header. A sign-in answers one <code>HttpOnly</code>, <code>Secure</code>, <code>SameSite=Strict</code> session cookie with a short lifetime. The panel loads no browser script and no external asset.</p>\n"
    "<p>Session: none</p>\n"
    "<p>Management API discovery: <code>/api/v1</code></p>\n"
    "</main>\n"
    "<footer>HFOC Appliance management panel</footer>\n"
    "</body>\n"
    "</html>\n";

static bool appliance_management_web_ui_section_valid(
    enum appliance_management_web_ui_section section)
{
    return (uint32_t)section <
           (uint32_t)APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
}

static void appliance_management_web_ui_write_head(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    const char *code = appliance_language_code(language);

    appliance_management_web_ui_buffer_text(
        buffer,
        "<!doctype html>\n"
        "<html lang=\"");
    if (code == NULL) {
        /*
         * A language outside the enum has no honest code to publish.  The
         * document is refused by the named status anyway; print the default
         * code so the buffer stays a well-formed document for the refusal.
         */
        buffer->missing_translation = true;
        code = appliance_language_code(APPLIANCE_LANGUAGE_DEFAULT);
    }
    appliance_management_web_ui_buffer_text(buffer, code);
    appliance_management_web_ui_buffer_text(
        buffer,
        "\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">\n"
        "<title>HFOC Appliance</title>\n"
        "<style>"
        "body{font-family:system-ui,sans-serif;margin:2rem;"
        "max-width:60rem;line-height:1.45}"
        "nav a{margin-right:.6rem}"
        "code{background:#eee;padding:0 .2rem}"
        ".state{font-weight:700}"
        ".notice{color:#a00;font-weight:700}"
        "footer{margin-top:2rem;color:#555;font-size:.9rem}"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>HFOC Appliance</h1>\n");
}

static void appliance_management_web_ui_write_index_nav(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    size_t index;

    /*
     * The canonical section index, in the order pinned by scripts/check.sh.
     * In the English default the rendered document is exactly
     * `<nav>Dashboard Network Services Storage System Security Diagnostics
     * Updates Recovery Routing Account Certificates Help</nav>`; the names come
     * from the one catalog through the key table, and the static sign-in shell
     * (`appliance_management_web_ui_login_body`, which stays English and says
     * `lang="en"`) lists the same names.  ITEM-509 increment 10 replaced the
     * literal with this loop, and the historical pins above still match the
     * documented rendering.
     */
    appliance_management_web_ui_buffer_text(buffer, "<nav>");
    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        if (index != 0U) {
            appliance_management_web_ui_buffer_text(buffer, " ");
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, appliance_management_web_ui_section_keys[index]);
    }
    appliance_management_web_ui_buffer_text(buffer, "</nav>\n");
}

static void appliance_management_web_ui_write_nav(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    size_t index;

    appliance_management_web_ui_buffer_text(buffer, "<nav>");
    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT;
         ++index) {
        /*
         * ITEM-499: the href is the section's declared path, printed once and
         * printed as it is.  The previous writer prefixed it with `/panel` and
         * appended the whole path from the table, so the menu published
         * `/panel//panel/network` while the router matched `/panel/network`:
         * the panel opened and every menu entry but Dashboard answered 404.
         * The path the menu publishes and the path the router matches now come
         * from the same accessor, so they cannot disagree.
         */
        const char *path = appliance_management_web_ui_section_path(
            (enum appliance_management_web_ui_section)index);

        if (path == NULL) {
            continue;
        }
        appliance_management_web_ui_buffer_text(buffer, "<a href=\"");
        appliance_management_web_ui_buffer_text(buffer, path);
        appliance_management_web_ui_buffer_text(buffer, "\">");
        /*
         * ITEM-509 increment 10: the label is the catalog's, never a second
         * literal here; a missing translation is latched and the page refuses
         * by name instead of printing an empty link.
         */
        appliance_management_web_ui_buffer_language(
            buffer, language, appliance_management_web_ui_section_keys[index]);
        appliance_management_web_ui_buffer_text(buffer, "</a>");
        if (index + 1U <
            APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT) {
            appliance_management_web_ui_buffer_text(buffer, " ");
        }
    }
    /*
     * ITEM-382: the about/licenses page is reachable from every section page.
     * The section index above stays exact; this is one extra target.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        " <a href=\"" APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH "\">"
        APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_LABEL "</a>");
    /*
     * ITEM-513: the browser's own language switch.  One POST form with two
     * submit buttons: a click submits exactly the one field the panel target
     * parses (`language=en` or `language=ru`) with no script and no token, and
     * the answer sets the `hfoc_lang` cookie.  A button starts, so a browser
     * that cannot submit a form still sees the labels, and the labels come
     * from the catalog like every other word on the page.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        " <form method=\"post\" action=\""
        APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH
        "\" style=\"display:inline\">");
    for (index = 0U; index < APPLIANCE_LANGUAGE_COUNT; ++index) {
        if (index != 0U) {
            appliance_management_web_ui_buffer_text(buffer, " ");
        }
        appliance_management_web_ui_buffer_text(
            buffer, "<button name=\"language\" value=\"");
        appliance_management_web_ui_buffer_text(
            buffer, appliance_language_code((enum appliance_language)index));
        appliance_management_web_ui_buffer_text(buffer, "\">");
        appliance_management_web_ui_buffer_language(
            buffer,
            language,
            appliance_management_web_ui_language_keys[index]);
        appliance_management_web_ui_buffer_text(buffer, "</button>");
    }
    appliance_management_web_ui_buffer_text(buffer, "</form>");
    appliance_management_web_ui_buffer_text(buffer, "</nav>\n");
}

static void appliance_management_web_ui_write_footer(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "</main>\n<footer>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.footer");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</footer>\n"
        "</body>\n"
        "</html>\n");
}

const uint8_t *appliance_management_web_ui_shell_body(void)
{
    return (const uint8_t *)appliance_management_web_ui_login_body;
}

uint32_t appliance_management_web_ui_shell_length(void)
{
    return (uint32_t)(
        sizeof(appliance_management_web_ui_login_body) - 1U);
}

const char *appliance_management_web_ui_section_string(
    enum appliance_management_web_ui_section section)
{
    if (!appliance_management_web_ui_section_valid(section)) {
        return NULL;
    }
    /*
     * ITEM-509 increment 10: the section label has exactly one source, the
     * catalog's English value - the literals that stood in the removed table.
     */
    return appliance_language_string(
        APPLIANCE_LANGUAGE_DEFAULT,
        appliance_management_web_ui_section_keys[section]);
}

const char *appliance_management_web_ui_section_path(
    enum appliance_management_web_ui_section section)
{
    if (!appliance_management_web_ui_section_valid(section)) {
        return NULL;
    }
    return appliance_management_web_ui_section_paths[section];
}

const char *appliance_management_web_ui_section_owner(
    enum appliance_management_web_ui_section section)
{
    if (!appliance_management_web_ui_section_valid(section)) {
        return NULL;
    }
    return appliance_management_web_ui_section_owners[section];
}

enum appliance_management_web_ui_section_state
appliance_management_web_ui_section_state(
    enum appliance_management_web_ui_section section)
{
    if (!appliance_management_web_ui_section_valid(section)) {
        return APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE;
    }
    return appliance_management_web_ui_section_states[section];
}

bool appliance_management_web_ui_section_confirmable(
    enum appliance_management_web_ui_section section)
{
    if (!appliance_management_web_ui_section_valid(section)) {
        return false;
    }
    return appliance_management_web_ui_confirmable_table[section];
}

uint32_t appliance_management_web_ui_page_budget(
    enum appliance_management_web_ui_page page)
{
    if ((uint32_t)page >=
        (uint32_t)APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT) {
        return 0U;
    }
    return appliance_management_web_ui_page_budgets[page];
}

const char *appliance_management_web_ui_section_state_string(
    enum appliance_management_web_ui_section_state state)
{
    switch (state) {
    case APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED:
        return "AVAILABLE";
    case APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNAVAILABLE";
    }
}

const char *appliance_management_web_ui_status_string(
    enum appliance_management_web_ui_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_WEB_UI_OK:
        return "APPLIANCE_MANAGEMENT_WEB_UI_OK";
    case APPLIANCE_MANAGEMENT_WEB_UI_NULL:
        return "APPLIANCE_MANAGEMENT_WEB_UI_NULL";
    case APPLIANCE_MANAGEMENT_WEB_UI_BAD_STATE:
        return "APPLIANCE_MANAGEMENT_WEB_UI_BAD_STATE";
    case APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW:
        return "APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW";
    case APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION:
        return "APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION";
    case APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION:
        return "APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION";
    default:
        return NULL;
    }
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render_login(
    const struct appliance_management_web_ui_session *session,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    bool authenticated = session != NULL && session->authenticated;
    const enum appliance_language language =
        session != NULL ? session->language : APPLIANCE_LANGUAGE_DEFAULT;

    if (output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    appliance_management_web_ui_write_head(&buffer, language);
    appliance_management_web_ui_write_index_nav(&buffer, language);
    appliance_management_web_ui_buffer_text(&buffer, "<main>\n<h2>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.title");
    appliance_management_web_ui_buffer_text(
        &buffer, "</h2>\n<p class=\"state\">");
    if (session != NULL && session->lockout_remaining_ticks != 0U) {
        appliance_management_web_ui_buffer_text(&buffer, "THROTTLED");
    } else if (authenticated) {
        appliance_management_web_ui_buffer_text(&buffer, "SIGNED IN");
    } else {
        appliance_management_web_ui_buffer_text(&buffer, "SIGNED OUT");
    }
    appliance_management_web_ui_buffer_text(
        &buffer,
        "</p>\n"
        /*
         * ITEM-445 step 2: the one sign-in form. It posts the administrator
         * secret to the same origin and the page still loads no script and no
         * external asset; the content security policy allows exactly this
         * form (`form-action 'self'`) and nothing else.
         */
        "<form method=\"post\" action=\"/\">\n"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.login");
    appliance_management_web_ui_buffer_text(
        &buffer,
        "<input type=\"text\" name=\"username\" "
        "autocomplete=\"username\"></label></p>\n"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.password");
    appliance_management_web_ui_buffer_text(
        &buffer,
        "<input type=\"password\" name=\"password\" "
        "autocomplete=\"current-password\"></label></p>\n"
        "<p><button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.sign_in");
    appliance_management_web_ui_buffer_text(
        &buffer,
        "</button></p>\n"
        "</form>\n"
        "<p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.set_account_prefix");
    appliance_management_web_ui_buffer_text(
        &buffer, "<code>credentials</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.set_account_suffix");
    appliance_management_web_ui_buffer_text(&buffer, "</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.requirements_prefix");
    appliance_management_web_ui_buffer_text(
        &buffer, "<code>Authorization: Bearer</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.requirements_middle");
    appliance_management_web_ui_buffer_text(&buffer, "<code>HttpOnly</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.cookie_separator");
    appliance_management_web_ui_buffer_text(&buffer, "<code>Secure</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.cookie_separator");
    appliance_management_web_ui_buffer_text(
        &buffer, "<code>SameSite=Strict</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.cookie_suffix");
    appliance_management_web_ui_buffer_text(&buffer, "</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.session");
    if (authenticated) {
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.login.session_prefix");
        appliance_management_web_ui_buffer_decimal(
            &buffer, session->session_id);
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.login.principal_prefix");
        appliance_management_web_ui_buffer_decimal(
            &buffer, session->principal_id);
    } else {
        appliance_management_web_ui_buffer_text(&buffer, "none");
    }
    appliance_management_web_ui_buffer_text(&buffer, "</p>\n");
    if (authenticated) {
        /*
         * ITEM-445 step 2: a signed-in browser gets the one way out. The
         * sign-out form posts to its own target, which expires the cookie and
         * sends the browser back to this document.
         */
        appliance_management_web_ui_buffer_text(
            &buffer,
            "<form method=\"post\" action=\"/panel/logout\">\n"
            "<p><button type=\"submit\">");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.login.sign_out");
        appliance_management_web_ui_buffer_text(
            &buffer,
            "</button></p>\n"
            "</form>\n");
    }
    if (session != NULL && session->lockout_remaining_ticks != 0U) {
        appliance_management_web_ui_buffer_text(&buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.login.throttle");
        appliance_management_web_ui_buffer_decimal(
            &buffer, session->lockout_remaining_ticks);
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.login.throttle_suffix");
        appliance_management_web_ui_buffer_text(&buffer, "</p>\n");
    }
    if (session != NULL && session->notice != NULL) {
        appliance_management_web_ui_buffer_text(
            &buffer, "<p class=\"notice\">");
        appliance_management_web_ui_buffer_value(&buffer, session->notice);
        appliance_management_web_ui_buffer_text(&buffer, "</p>\n");
    }
    appliance_management_web_ui_buffer_text(&buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.login.api_discovery");
    appliance_management_web_ui_buffer_text(
        &buffer, "<code>/api/v1</code></p>\n");
    appliance_management_web_ui_write_footer(&buffer, language);
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    if (buffer.missing_translation) {
        return APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}

static void appliance_management_web_ui_write_view(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_view *view,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<dl>\n<dt>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.view.hostname");
    appliance_management_web_ui_buffer_text(buffer, "</dt><dd>");
    appliance_management_web_ui_buffer_value(
        buffer, view != NULL ? view->hostname : NULL);
    appliance_management_web_ui_buffer_text(buffer, "</dd>\n<dt>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.view.mac");
    appliance_management_web_ui_buffer_text(buffer, "</dt><dd>");
    appliance_management_web_ui_buffer_value(
        buffer, view != NULL ? view->mac_address : NULL);
    appliance_management_web_ui_buffer_text(buffer, "</dd>\n<dt>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.view.service");
    appliance_management_web_ui_buffer_text(buffer, "</dt><dd>");
    appliance_management_web_ui_buffer_value(
        buffer, view != NULL ? view->service_name : NULL);
    appliance_management_web_ui_buffer_text(buffer, "</dd>\n</dl>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.view.detail");
    appliance_management_web_ui_buffer_text(buffer, ": ");
    appliance_management_web_ui_buffer_value(
        buffer, view != NULL ? view->detail : NULL);
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.view.recent_log");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n<pre>");
    appliance_management_web_ui_buffer_value(
        buffer, view != NULL ? view->log_line : NULL);
    appliance_management_web_ui_buffer_text(buffer, "</pre>\n");
}

static void appliance_management_web_ui_write_state(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_management_web_ui_section section,
    enum appliance_language language)
{
    const enum appliance_management_web_ui_section_state state =
        appliance_management_web_ui_section_state(section);
    const char *owner =
        appliance_management_web_ui_section_owner(section);

    appliance_management_web_ui_buffer_text(
        buffer, "<p class=\"state\">");
    appliance_management_web_ui_buffer_text(
        buffer,
        appliance_management_web_ui_section_state_string(state));
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
        /*
         * ITEM-509 increment 8: Help names no management API owner, but it is
         * not an unavailable section either: the text is compiled into this
         * build and rendered from the same catalog the console `help` command
         * prints.  The page says where the content comes from instead of
         * naming an owner that does not exist.
         */
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.state.help_source_prefix");
        appliance_management_web_ui_buffer_text(buffer, "<code>help</code>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.state.help_source_suffix");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    if (state ==
        APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE) {
        /*
         * ITEM-499: an UNAVAILABLE field names why it is unavailable, so the
         * reader is not left at "UNAVAILABLE" with nowhere to go: no owner is
         * bound, so no read side is published for this section.
         * ITEM-503: Security has no read owner but it does have a mutation
         * surface, the BYO certificate import; the page says which one is
         * missing instead of claiming nothing is published at all.
         */
        if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY) {
            appliance_management_web_ui_buffer_text(buffer, "<p>");
            appliance_management_web_ui_buffer_language(
                buffer, language, "panel.state.security_no_read");
            appliance_management_web_ui_buffer_text(buffer, "</p>\n");
            return;
        }
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.state.unavailable");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.state.owner");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, owner);
    appliance_management_web_ui_buffer_text(buffer, "</code>");
    if (state == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED) {
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.state.degraded");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.state.read_side");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

/*
 * ITEM-509 increment 8: the Help section renderer.
 *
 * The text is the catalog the console's `help` command reads, so this page and
 * the console cannot tell different stories: one topic table, four strings per
 * topic, rendered as HTML here.  The page shapes are:
 *
 *   - `/panel/help` is the table of contents: every topic's title links to its
 *     topic page and its one-line summary is printed next to it;
 *   - `/panel/help/<id>` is one topic: title, summary, the body line by line
 *     and the related commands and sections;
 *   - the Dashboard carries a short block with the same links.
 *
 * Nothing is invented and nothing is silently cut: the catalog strings are
 * escaped whole (they are not run through the generic bounded value renderer,
 * whose 96-byte bound would cut a translated sentence in silence), and a
 * catalog that cannot render a topic publishes a named refusal with the
 * offending catalog key instead of a half topic.
 */

static bool appliance_management_web_ui_help_topic_renderable(
    size_t index,
    enum appliance_language language,
    const char **key_out)
{
    static const enum appliance_help_field fields[] = {
        APPLIANCE_HELP_FIELD_TITLE,
        APPLIANCE_HELP_FIELD_SUMMARY,
        APPLIANCE_HELP_FIELD_BODY,
        APPLIANCE_HELP_FIELD_RELATED
    };
    size_t field;

    for (field = 0U; field < sizeof(fields) / sizeof(fields[0]); ++field) {
        const char *text = appliance_help_topic_string(
            index, fields[field], language);

        if (text == NULL || text[0] == '\0') {
            if (key_out != NULL) {
                *key_out = appliance_help_topic_key(index, fields[field]);
            }
            return false;
        }
    }
    return true;
}

static void appliance_management_web_ui_write_help_refusal(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const char *reason,
    const char *key)
{
    appliance_management_web_ui_buffer_text(buffer, "<p class=\"notice\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.not_available");
    if (reason != NULL) {
        appliance_management_web_ui_buffer_text(buffer, ": ");
        appliance_management_web_ui_buffer_value(buffer, reason);
    }
    if (key != NULL) {
        appliance_management_web_ui_buffer_text(buffer, " key=");
        appliance_management_web_ui_buffer_value(buffer, key);
    }
    appliance_management_web_ui_buffer_text(buffer, ".</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.command_prefix");
    appliance_management_web_ui_buffer_text(buffer, "<code>help</code>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.command_suffix");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

static void appliance_management_web_ui_write_help_body(
    struct appliance_management_web_ui_buffer *buffer,
    const char *body)
{
    const char *cursor = body;

    appliance_management_web_ui_buffer_text(buffer, "<p>");
    while (*cursor != '\0') {
        const char *end = cursor;

        while (*end != '\0' && *end != '\n') {
            ++end;
        }
        appliance_management_web_ui_buffer_escaped_span(
            buffer, cursor, (size_t)(end - cursor));
        if (*end == '\n') {
            appliance_management_web_ui_buffer_text(buffer, "<br>\n");
            cursor = end + 1;
        } else {
            cursor = end;
        }
    }
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

static void appliance_management_web_ui_write_help_text(
    struct appliance_management_web_ui_buffer *buffer,
    const char *text)
{
    if (text == NULL) {
        appliance_management_web_ui_buffer_text(buffer, "unavailable");
        return;
    }
    appliance_management_web_ui_buffer_escaped_span(
        buffer, text, strlen(text));
}

static void appliance_management_web_ui_write_help_topic_page(
    struct appliance_management_web_ui_buffer *buffer,
    size_t index,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_write_help_text(
        buffer,
        appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_TITLE, language));
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
    appliance_management_web_ui_write_help_text(
        buffer,
        appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_SUMMARY, language));
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    appliance_management_web_ui_write_help_body(
        buffer,
        appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_BODY, language));
    /*
     * The related string carries its own label in the catalog (`RELATED: ...`
     * in English, `СВЯЗАНО: ...` in Russian), exactly as the console prints
     * it, so the page does not print a second one in front of it.
     */
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_write_help_text(
        buffer,
        appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_RELATED, language));
    appliance_management_web_ui_buffer_text(
        buffer,
        "</p>\n<p><a href=\"" APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH "\">");
    /*
     * ITEM-509 increment 25: the link label is operator prose and follows the
     * chosen language; only the markup around it stays a literal.
     */
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.all_topics");
    appliance_management_web_ui_buffer_text(buffer, "</a></p>\n");
}

static void appliance_management_web_ui_write_help_topic_link(
    struct appliance_management_web_ui_buffer *buffer,
    size_t index,
    enum appliance_language language)
{
    const char *id = appliance_help_topic_id(index);

    appliance_management_web_ui_buffer_text(
        buffer,
        "<a href=\"" APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX);
    appliance_management_web_ui_buffer_escaped_span(
        buffer, id, strlen(id));
    appliance_management_web_ui_buffer_text(buffer, "\">");
    appliance_management_web_ui_write_help_text(
        buffer,
        appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_TITLE, language));
    appliance_management_web_ui_buffer_text(buffer, "</a>");
}

static void appliance_management_web_ui_write_help(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_help *help)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
    const char *missing_key = NULL;
    size_t count;
    size_t index;

    if (help == NULL || !help->published || help->language == NULL ||
        appliance_language_parse(
            help->language,
            strlen(help->language),
            &language) != APPLIANCE_LANGUAGE_OK) {
        appliance_management_web_ui_write_help_refusal(
            buffer,
            language,
            help != NULL && help->reason != NULL
                ? help->reason
                : "LANGUAGE_UNAVAILABLE",
            NULL);
        return;
    }
    count = appliance_help_topic_count();
    for (index = 0U; index < count; ++index) {
        if (!appliance_management_web_ui_help_topic_renderable(
                index, language, &missing_key)) {
            appliance_management_web_ui_write_help_refusal(
                buffer, language, "CATALOG_INCOMPLETE", missing_key);
            return;
        }
    }
    if (count == 0U) {
        appliance_management_web_ui_write_help_refusal(
            buffer, language, "CATALOG_INCOMPLETE", NULL);
        return;
    }
    if (help->topic != NULL) {
        const size_t topic = appliance_help_topic_find(
            help->topic, strlen(help->topic));

        if (topic == (size_t)-1) {
            appliance_management_web_ui_write_help_refusal(
                buffer, language, "NO_SUCH_HELP_TOPIC", NULL);
            return;
        }
        appliance_management_web_ui_write_help_topic_page(
            buffer, topic, language);
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_decimal(buffer, (uint64_t)count);
    /*
     * ITEM-509 increment 25: the index sentence is operator prose and follows
     * the chosen language; the `<code>help</code>` between its two halves is
     * markup and stays a literal, so the sentence can be translated in the
     * order each language reads.
     */
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.index_intro");
    appliance_management_web_ui_buffer_text(buffer, "<code>help</code>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.index_intro_tail");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<ul>\n");
    for (index = 0U; index < count; ++index) {
        appliance_management_web_ui_buffer_text(buffer, "<li>");
        appliance_management_web_ui_write_help_topic_link(
            buffer, index, language);
        appliance_management_web_ui_buffer_text(buffer, " - ");
        appliance_management_web_ui_write_help_text(
            buffer,
            appliance_help_topic_string(
                index, APPLIANCE_HELP_FIELD_SUMMARY, language));
        appliance_management_web_ui_buffer_text(buffer, "</li>\n");
    }
    appliance_management_web_ui_buffer_text(buffer, "</ul>\n");
}

/*
 * ITEM-509 increment 8: the Dashboard's help block.  It is the index, not a
 * second copy of the text: every topic links to its own page.  The page is
 * measured at its budget, so the block stays one line of links.
 */
static void appliance_management_web_ui_write_help_block(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_help *help)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
    const char *missing_key = NULL;
    size_t count;
    size_t index;

    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.section.help");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n");
    if (help == NULL || !help->published || help->language == NULL ||
        appliance_language_parse(
            help->language,
            strlen(help->language),
            &language) != APPLIANCE_LANGUAGE_OK) {
        appliance_management_web_ui_write_help_refusal(
            buffer,
            language,
            help != NULL && help->reason != NULL
                ? help->reason
                : "LANGUAGE_UNAVAILABLE",
            NULL);
        return;
    }
    count = appliance_help_topic_count();
    for (index = 0U; index < count; ++index) {
        if (!appliance_management_web_ui_help_topic_renderable(
                index, language, &missing_key)) {
            appliance_management_web_ui_write_help_refusal(
                buffer, language, "CATALOG_INCOMPLETE", missing_key);
            return;
        }
    }
    if (count == 0U) {
        appliance_management_web_ui_write_help_refusal(
            buffer, language, "CATALOG_INCOMPLETE", NULL);
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.help.index_prefix");
    for (index = 0U; index < count; ++index) {
        if (index != 0U) {
            appliance_management_web_ui_buffer_text(buffer, ", ");
        }
        appliance_management_web_ui_write_help_topic_link(
            buffer, index, language);
    }
    appliance_management_web_ui_buffer_text(buffer, ".</p>\n");
}

static void appliance_management_web_ui_write_confirmation(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const struct appliance_management_web_ui_confirmation *confirmation)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.title");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n");
    if (confirmation == NULL || !confirmation->pending) {
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.confirm.none");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.mutation");
    appliance_management_web_ui_buffer_value(
        buffer, confirmation->mutation);
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.token");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->token);
    appliance_management_web_ui_buffer_text(buffer, " (");
    appliance_management_web_ui_buffer_language(
        buffer, language,
        confirmation->single_use ? "panel.confirm.single_use"
                                 : "panel.confirm.reusable");
    appliance_management_web_ui_buffer_text(buffer, ")</p>\n");
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.expires");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->remaining_ticks);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.window_suffix");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

/* ITEM-647: the one labelled confirmation field, defined below the page
 * readers so every form can share it. */
static void appliance_management_web_ui_write_token_field(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language);

/*
 * ITEM-503: the Security page carries the one mutation the section owns, the
 * BYO certificate import.  The forms are plain HTML with no script and post
 * to the same origin, exactly like the sign-in and sign-out forms; the
 * confirmation token the staging response renders is what the second form
 * repeats.
 */
static void appliance_management_web_ui_write_security_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.title");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</h3>\n"
        "<form method=\"post\" action=\"/panel/security\">\n"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.certificate_pem");
    appliance_management_web_ui_buffer_text(
        buffer,
        "<br><textarea name=\"certificate\" rows=\"3\" cols=\"40\""
        " placeholder=\"");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.placeholder_certificate");
    appliance_management_web_ui_buffer_text(
        buffer, "\"></textarea></label></p>\n<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.chain_pem");
    appliance_management_web_ui_buffer_text(
        buffer,
        "<br><textarea name=\"chain\" rows=\"2\" cols=\"40\""
        " placeholder=\"");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.placeholder_chain");
    appliance_management_web_ui_buffer_text(
        buffer, "\"></textarea></label></p>\n<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.key_pem");
    appliance_management_web_ui_buffer_text(
        buffer,
        "<br><textarea name=\"key\" rows=\"3\" cols=\"40\""
        " placeholder=\"");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.placeholder_key");
    appliance_management_web_ui_buffer_text(
        buffer,
        "\"></textarea></label></p>\n"
        "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<form method=\"post\" action=\"/panel/security\">\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button type=\"submit\" name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "<button type=\"submit\" name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<p><a href=\"" APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH "\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.control_link");
    appliance_management_web_ui_buffer_text(buffer, "</a>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.control_tail");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

/*
 * ITEM-503: the compact confirmation the import page renders while a
 * candidate waits.  The generic block gives the reader more prose than the
 * 2048-byte page can hold next to the form; the facts are the same: mutation,
 * single-use token, remaining window.
 */
static void appliance_management_web_ui_write_security_confirmation(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const struct appliance_management_web_ui_confirmation *confirmation)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.pending_title");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.mutation");
    appliance_management_web_ui_buffer_value(
        buffer, confirmation->mutation);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.token_inline");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->token);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.expires_inline");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->remaining_ticks);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.security.ticks_single_use");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

/*
 * ITEM-504: the Routing page.  It is the first section whose page configures
 * instead of reads: it lists the applied table its owner published, offers
 * the one-body stage form and the token confirmation form, and shows the
 * candidate the plane holds.  The forms stage and confirm; the page itself
 * never mutates and never confirms an operation by itself.
 */
static void appliance_management_web_ui_buffer_value_bounded(
    struct appliance_management_web_ui_buffer *buffer,
    const char *value,
    size_t max_characters)
{
    size_t index;

    if (value == NULL) {
        appliance_management_web_ui_buffer_text(buffer, "unavailable");
        return;
    }
    for (index = 0U;
         index < max_characters && value[index] != '\0';
         ++index) {
        char single[2];
        size_t escaped_length = 0U;
        char escaped[7];

        single[0] = value[index];
        single[1] = '\0';
        if (appliance_management_web_ui_escape(
                single,
                1U,
                escaped,
                sizeof(escaped),
                &escaped_length) != APPLIANCE_MANAGEMENT_WEB_UI_OK) {
            buffer->overflow = true;
            return;
        }
        appliance_management_web_ui_buffer_text(buffer, escaped);
    }
}

static void appliance_management_web_ui_write_routes(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_routes *routes,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.routes.title");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n");
    if (routes == NULL || !routes->published) {
        /*
         * ITEM-499: a section that cannot publish its value names why instead
         * of rendering an empty list a reader would take for "nothing is
         * routed".
         */
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.routes.none_published");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    if (routes->count == 0U) {
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.routes.none_applied");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_decimal(buffer, routes->count);
    appliance_management_web_ui_buffer_language(
        buffer, language,
        routes->count == 1U ? "panel.routes.one" : "panel.routes.many");
    if (routes->truncated) {
        appliance_management_web_ui_buffer_text(buffer, ", ");
        appliance_management_web_ui_buffer_decimal(buffer, routes->shown);
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.routes.truncated");
    }
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<pre>");
    appliance_management_web_ui_buffer_value_bounded(
        buffer,
        routes->text,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ROUTES_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, "</pre>\n");
}

/*
 * ITEM-505: the Network page.  It is the second configuring section and the
 * first one whose change is not applied immediately: the operator reads what
 * the interface runs now and what the durable record will apply at the next
 * boot, stages a candidate, and confirms it with the token the plane issues.
 * The page never mutates and never confirms by itself; both readings are the
 * owner's, and a section whose owner published nothing says so instead of
 * rendering a blank.
 */
static void appliance_management_web_ui_write_network(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_network *network,
    enum appliance_language language)
{
    if (network == NULL || !network->published) {
        appliance_management_web_ui_buffer_text(buffer, "<h3>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.network.title");
        appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.network.none_published");
        if (network != NULL && network->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(
                buffer, network->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.not_invented");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.active");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, network->active);
    appliance_management_web_ui_buffer_text(buffer, "</code><br>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.saved");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, network->saved);
    appliance_management_web_ui_buffer_text(buffer, "</code> source=");
    appliance_management_web_ui_buffer_value(buffer, network->source);
    appliance_management_web_ui_buffer_text(buffer, " applies=");
    appliance_management_web_ui_buffer_value(buffer, network->applies);
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.saved_applies");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

/*
 * ITEM-647: every token form carries the same visible label and the same
 * one-line explanation.  The field name and the numeric input mode stay the
 * contract the serving path reads; only the words around it are catalog
 * entries, so the selected language is the only one rendered.
 */
static void appliance_management_web_ui_write_token_field(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm_code");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"token\" inputmode=\"numeric\"></label><br>"
                "<small>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm_hint");
    appliance_management_web_ui_buffer_text(buffer, "</small></p>\n");
}

static void appliance_management_web_ui_write_network_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    /*
     * ITEM-515: every control carries the label that names it, so the operator
     * reads which field is which without relying on their order; the prefix
     * label says it is the bit count alone, because the form joins it to the
     * address itself.  The page has a 2048-byte cap and it must hold both
     * readings next to the forms; the form target, the field names and the
     * two-phase buttons stay the contract the serving path reads.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"/panel/network\">\n"
        "<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.label_mode");
    appliance_management_web_ui_buffer_text(
        buffer,
        " <select name=\"mode\"><option value=\"dhcp\">DHCP</option>"
        "<option value=\"static\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.mode_static");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</option></select></label>\n"
        "<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.label_address");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"address\"></label>\n<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.label_prefix");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"prefix\"></label>\n<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.label_gateway");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"gateway\"></label>\n<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.network.label_dns");
    appliance_management_web_ui_buffer_text(
        buffer,
        " <input name=\"dns\"></label>\n"
        "<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<form method=\"post\" action=\"/panel/network\">\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button><button "
        "name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n");
}

/*
 * ITEM-505 increment 3: the System page.  It shows the identity the running
 * system serves and the hostname the durable record will apply at the next
 * boot, offers the one-field hostname form and the token confirmation form,
 * and never mutates or confirms by itself.  A section whose owner published
 * nothing says so instead of rendering a blank.
 */
static void appliance_management_web_ui_write_system(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_system *system,
    enum appliance_language language)
{
    if (system == NULL || !system->published) {
        appliance_management_web_ui_buffer_text(buffer, "<h3>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.system.title");
        appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.system.none_published");
        if (system != NULL && system->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(
                buffer, system->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.not_invented");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.active");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, system->active);
    appliance_management_web_ui_buffer_text(buffer, "</code><br>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.saved");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, system->saved);
    appliance_management_web_ui_buffer_text(buffer, "</code> source=");
    appliance_management_web_ui_buffer_value(buffer, system->source);
    appliance_management_web_ui_buffer_text(buffer, " applies=");
    appliance_management_web_ui_buffer_value(buffer, system->applies);
    appliance_management_web_ui_buffer_text(buffer, "</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.saved_applies");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    /*
     * ITEM-509 increment 5: the interface language, from the same record.  The
     * panel renders in the language it was published with and says which one
     * that is, next to the language the record holds; a value the owner could
     * not read is published as UNAVAILABLE rather than guessed.  The switch
     * that changes it is the next increment, so this page shows the pair and
     * offers no form for it yet.
     */
    {
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.system.language_active");
        appliance_management_web_ui_buffer_text(buffer, "<code>");
        appliance_management_web_ui_buffer_value(
            buffer, system->language_active != NULL
                        ? system->language_active
                        : "UNAVAILABLE");
        appliance_management_web_ui_buffer_text(buffer, "</code><br>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.saved");
        appliance_management_web_ui_buffer_text(buffer, "<code>");
        appliance_management_web_ui_buffer_value(
            buffer, system->language_saved != NULL
                        ? system->language_saved
                        : "UNAVAILABLE");
        appliance_management_web_ui_buffer_text(buffer, "</code></p>\n");
    }
}

static void appliance_management_web_ui_write_system_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    /*
     * ITEM-513: one field per form, because the body a browser submits is the
     * body the stage parses.  ITEM-509 increment 6 had drawn both fields in
     * one form (`<input name="hostname"><input name="language">`), but the
     * stage accepts exactly one field per body and refuses a second, so the
     * form a browser actually submitted could never pass: the defect this item
     * closes.  Each stage form now carries exactly one named field, and the
     * confirmation form carries the token and its two buttons.  The form
     * target, the field names and the button names are the contract the
     * serving path reads.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"/panel/system\">\n"
        "<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.hostname");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"hostname\"></label>"
                "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<form method=\"post\" action=\"/panel/system\">\n"
        "<label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.language");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"language\"></label>"
                "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<form method=\"post\" action=\"/panel/system\">\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button type=\"submit\" name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button><button type=\"submit\" "
        "name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n");
}

/*
 * ITEM-506 increment 6: the Account page.  It shows the stored login, offers
 * the four-field change form and the token confirmation form, and never
 * renders the password or confirms by itself.  The change is immediate: the
 * commit writes the credential record, republishes the verifier and closes
 * every issued session, so the page says so and the operator expects the
 * sign-in page on the next request.  A section whose owner published nothing
 * says so instead of rendering a blank.
 */
static void appliance_management_web_ui_write_account(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_account *account,
    enum appliance_language language)
{
    if (account == NULL || !account->published) {
        appliance_management_web_ui_buffer_text(buffer, "<h3>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.section.account");
        appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.account.none_published");
        if (account != NULL && account->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(
                buffer, account->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.not_invented");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.account.login");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, account->login);
    appliance_management_web_ui_buffer_text(buffer, "</code></p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.account.change_prefix");
    appliance_management_web_ui_buffer_text(buffer, "<code>credentials</code>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.account.change_suffix");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

static void appliance_management_web_ui_write_account_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    /*
     * The page has a 2048-byte cap and it must hold the reading next to the
     * forms, so the fields carry names and the password inputs carry their
     * type and nothing else.  The form target, the field names and the
     * two-phase buttons are the contract the serving path reads.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"/panel/account\">\n"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.current_password");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"current\" type=\"password\"></label></p>\n"
                "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.new_login");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"login\"></label></p>\n<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.new_password");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"password\" type=\"password\"></label></p>\n"
                "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.repeat_password");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"repeat\" type=\"password\"></label></p>\n"
                "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<form method=\"post\" action=\"/panel/account\">\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button type=\"submit\" name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button><button type=\"submit\" "
        "name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n");
}

/*
 * ITEM-507 increment 5: the Certificates page.  It lists the active table the
 * running listener selects from next to the saved table the next boot will
 * read, names the default declaration of each and says `applies=reboot`; the
 * page never renders a key or a certificate body - the tables are bounded
 * `name (bytes)` lists.  The import, delete and default forms and the token
 * confirmation form post to `/panel/certificates`, which is the same mutation
 * the scriptable API target runs.  A section whose owner published nothing
 * says so instead of rendering a blank.
 */
/*
 * ITEM-508 increment 32: the ACME state block of the Certificates page.
 *
 * Read-only by construction: the block renders values and nothing else - no
 * `<form>`, no action, no token - because ITEM-508 point 10 makes enabling,
 * disabling and issuance mutations of the two-phase plane.  An unpublished
 * block renders the reason instead of disappearing, so "the panel does not
 * know" is never read as "ACME is off".
 */
static void appliance_management_web_ui_write_acme(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_acme *acme,
    enum appliance_language language)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.title");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n");
    if (acme == NULL || !acme->published) {
        appliance_management_web_ui_buffer_text(buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.acme.none_published");
        if (acme != NULL && acme->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(buffer, acme->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.not_invented");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.state");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->state, (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.account");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->account,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.renewal");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->renewal,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.expires");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->expires,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.attempts");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->attempts,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    /* ITEM-508 increment 37: the order path's own state and step. */
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.order");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, acme->order,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.names_note");
    appliance_management_web_ui_buffer_text(
        buffer, "<a href=\"" APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH "\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.control_link");
    appliance_management_web_ui_buffer_text(buffer, "</a>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme.control_tail");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

static void appliance_management_web_ui_write_certificates(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_management_web_ui_certificates *certificates,
    enum appliance_language language)
{
    if (certificates == NULL || !certificates->published) {
        appliance_management_web_ui_buffer_text(buffer, "<h3>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.section.certificates");
        appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.certificates.none_published");
        if (certificates != NULL && certificates->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(
                buffer, certificates->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.read.not_invented");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    /*
     * The page has a 2048-byte cap and it must hold two tables next to the
     * mutation forms, so the reading is deliberately asymmetric: the active
     * table is the boot-time table the listener already selected from and is
     * summarised (how many names, the default declaration and the source it
     * was read from), while the saved table - the one the operator edits - is
     * listed name by name with each certificate's byte count.  A saved table
     * too large for the list is cut at a complete entry and flagged, never
     * silently truncated in the middle of one.
     */
    appliance_management_web_ui_buffer_text(buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.active_names");
    appliance_management_web_ui_buffer_decimal(
        buffer, certificates->count);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.default");
    appliance_management_web_ui_buffer_value(
        buffer, certificates->active_default);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.source");
    appliance_management_web_ui_buffer_value(
        buffer, certificates->active_source);
    appliance_management_web_ui_buffer_text(buffer, ".</p>\n<p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.read.saved");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(buffer, certificates->saved);
    appliance_management_web_ui_buffer_text(buffer, "</code>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.default");
    appliance_management_web_ui_buffer_text(buffer, "<code>");
    appliance_management_web_ui_buffer_value(
        buffer, certificates->saved_default);
    appliance_management_web_ui_buffer_text(buffer, "</code>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.applies");
    appliance_management_web_ui_buffer_value(buffer, certificates->applies);
    if (certificates->truncated) {
        appliance_management_web_ui_buffer_text(buffer, " [");
        appliance_management_web_ui_buffer_decimal(
            buffer, certificates->shown);
        appliance_management_web_ui_buffer_text(buffer, "/");
        appliance_management_web_ui_buffer_decimal(
            buffer, certificates->count);
        appliance_management_web_ui_buffer_text(buffer, "]");
    }
    appliance_management_web_ui_buffer_text(buffer, ".</p>\n");
}

static void appliance_management_web_ui_write_certificates_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    /*
     * The page has a 2048-byte cap and it must hold the two tables next to the
     * forms, so the mutation surface is two compact forms: the import form and
     * one form that carries the delete action and the token confirmation the
     * plane reads by name.  The form target and the field names are the
     * contract the serving path reads; `default` is a checkbox, so a browser
     * sends it only when the operator asked for it.  The declaration action
     * stays on the scriptable API target, which the page names.
     */
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"/panel/certificates\">"
        "<input type=\"hidden\" name=\"action\" value=\"import\">"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.certificate_name");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"name\"></label></p>\n"
                "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.certificate_pem");
    appliance_management_web_ui_buffer_text(
        buffer, "<br><textarea name=\"certificate\"></textarea></label></p>\n"
                "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.key_pem");
    appliance_management_web_ui_buffer_text(
        buffer, "<br><textarea name=\"key\"></textarea></label></p>\n"
                "<p><label><input type=\"checkbox\" name=\"default\""
                " value=\"1\">");
    appliance_management_web_ui_buffer_text(buffer, " ");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.default_certificate");
    appliance_management_web_ui_buffer_text(
        buffer, "</label></p>\n<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.certificates.import");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n"
        "<form method=\"post\" action=\"/panel/certificates\">"
        "<input type=\"hidden\" name=\"action\" value=\"delete\">"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.certificate_name");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"name\"></label></p>\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>"
        "<button name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n");
}

static void appliance_management_web_ui_write_routing_form(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language)
{
    /*
     * The page has a 2048-byte cap and it must hold the applied table next to
     * the forms, so the fields carry names and nothing else: no placeholder,
     * no size hint, no input mode.  The form target, the field names and the
     * two-phase buttons are the contract the serving path reads.
     */
    /*
     * ITEM-651: the owner could not tell the add block from the remove block.
     * Give each form its own visible heading and action, and name the remove
     * field for what it deletes.  The token form keeps the shared two-phase
     * heading so the third block is visibly the confirmation, not a third
     * mutation.
     */
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.route_add");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</h3>\n"
        "<form method=\"post\" action=\"/panel/routing\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"add\">"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.route_name");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"name\"></label></p>\n<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.backend_address");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"backend\" placeholder=\"192.0.2.10\">"
                "</label></p>\n<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.backend_port");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"port\" placeholder=\"443\"></label></p>\n"
                "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.tls_to_server");
    appliance_management_web_ui_buffer_text(
        buffer,
        " <select name=\"tls\"><option value=\"off\">off</option>"
        "<option value=\"on\">on</option></select></label></p>\n"
        "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage_add");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.route_remove");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</h3>\n"
        "<form method=\"post\" action=\"/panel/routing\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"remove\">"
        "<p><label>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.route_name_remove");
    appliance_management_web_ui_buffer_text(
        buffer, " <input name=\"name\"></label></p>\n"
                "<button type=\"submit\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.stage_remove");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n"
        "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.title");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</h3>\n"
        "<form method=\"post\" action=\"/panel/routing\">\n");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button type=\"submit\" name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button><button type=\"submit\" "
        "name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button>\n"
        "</form>\n");
}

static void appliance_management_web_ui_write_mutation_confirmation(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const struct appliance_management_web_ui_confirmation *confirmation)
{
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.pending");
    appliance_management_web_ui_buffer_text(buffer, "</h3>\n<p>");
    appliance_management_web_ui_buffer_value(
        buffer, confirmation->mutation);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.token_inline");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->token);
    /*
     * ITEM-504: the window is milliseconds, the unit the plane compares.  The
     * page says so instead of printing a bare number a reader would take for
     * seconds, and it stays one line: the table needs the room.
     */
    appliance_management_web_ui_buffer_text(buffer, ", ");
    appliance_management_web_ui_buffer_decimal(
        buffer, confirmation->remaining_ticks);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.confirm.ms_left");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render_section(
    enum appliance_management_web_ui_section section,
    const struct appliance_management_web_ui_view *view,
    const struct appliance_management_web_ui_confirmation *confirmation,
    const struct appliance_management_web_ui_routes *routes,
    const struct appliance_management_web_ui_network *network,
    const struct appliance_management_web_ui_system *system,
    const struct appliance_management_web_ui_account *account,
    const struct appliance_management_web_ui_certificates *certificates,
    const struct appliance_management_web_ui_help *help,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    enum appliance_language language =
        view != NULL ? view->language : APPLIANCE_LANGUAGE_DEFAULT;

    if (output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    if (!appliance_management_web_ui_section_valid(section)) {
        return APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION;
    }
    /*
     * ITEM-509 increment 10: the Help page renders its body from
     * `help->language`, the `en`/`ru` code the dispatcher read from the same
     * system record; the document's `lang` attribute comes from that same
     * string, so the page cannot claim one language and print another.  On
     * every other section the language is the view's field, which the
     * dispatcher fills from the same record.
     */
    if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
        enum appliance_language help_language = APPLIANCE_LANGUAGE_DEFAULT;

        if (help != NULL && help->published && help->language != NULL &&
            appliance_language_parse(
                help->language,
                strlen(help->language),
                &help_language) == APPLIANCE_LANGUAGE_OK) {
            language = help_language;
        } else {
            language = APPLIANCE_LANGUAGE_DEFAULT;
        }
    }
    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    appliance_management_web_ui_write_head(&buffer, language);
    appliance_management_web_ui_write_nav(&buffer, language);
    appliance_management_web_ui_buffer_text(&buffer, "<main>\n<h2>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, appliance_management_web_ui_section_keys[section]);
    appliance_management_web_ui_buffer_text(&buffer, "</h2>\n");
    appliance_management_web_ui_write_state(&buffer, section, language);
    if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY) {
        /*
         * ITEM-503: the import page carries the form, not the read-side value
         * dump, and it renders the confirmation block only while a candidate
         * is actually pending - the page has one job and a 2048-byte cap.
         */
        appliance_management_web_ui_write_security_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_security_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK) {
        /*
         * ITEM-505: the network page shows the active and the saved
         * configuration its owner published, offers the mode form and the
         * token confirmation form, and shows the candidate the plane holds.
         */
        appliance_management_web_ui_write_network(&buffer, network, language);
        appliance_management_web_ui_write_network_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_mutation_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING) {
        /*
         * ITEM-504: the first mutating section.  The page lists the applied
         * table its owner published, offers the stage form and the token
         * confirmation form, and shows the candidate the plane holds.  It
         * carries no read-side value dump: the table is the section's state
         * and the page has a 2048-byte cap.
         */
        appliance_management_web_ui_write_routes(&buffer, routes, language);
        appliance_management_web_ui_write_routing_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_mutation_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT) {
        /*
         * ITEM-506 increment 6: the Account page shows the stored login its
         * owner published, offers the four-field change form and the token
         * confirmation form, and shows the candidate the plane holds.  It
         * carries no password: neither the current one nor the new one is
         * ever rendered.
         */
        appliance_management_web_ui_write_account(&buffer, account, language);
        appliance_management_web_ui_write_account_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_mutation_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES) {
        /*
         * ITEM-507 increment 5: the Certificates page lists the active and the
         * saved name tables the owner published, offers the import, delete and
         * default forms and the token confirmation form, and shows the
         * candidate the plane holds.  It carries no key and no certificate
         * body.
         */
        appliance_management_web_ui_write_certificates(&buffer, certificates, language);
        appliance_management_web_ui_write_certificates_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_mutation_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM) {
        /*
         * ITEM-505 increment 3: the System page shows the identity the running
         * system serves and the saved hostname its owner published, offers the
         * one-field form and the token confirmation form, and shows the
         * candidate the plane holds.
         */
        appliance_management_web_ui_write_system(&buffer, system, language);
        appliance_management_web_ui_write_system_form(&buffer, language);
        if (confirmation != NULL && confirmation->pending) {
            appliance_management_web_ui_write_mutation_confirmation(
                &buffer, language, confirmation);
        }
    } else if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP) {
        /*
         * ITEM-509 increment 8: the Help page renders the catalog the console
         * prints - the table of contents on `/panel/help`, one topic with all
         * four of its strings on `/panel/help/<id>` - so the two surfaces
         * cannot tell different stories.
         */
        appliance_management_web_ui_write_help(&buffer, help);
    } else {
        appliance_management_web_ui_write_view(&buffer, view, language);
        if (section == APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD) {
            /*
             * ITEM-508 increment 32: the read-only ACME state on the page the
             * operator opens first.  The Certificates page is measured at its
             * 2048-byte cap with two name tables and four forms, so the block
             * lives here, beside the generic view, and names the Certificates
             * page for the names themselves.  It is a reading only: no form,
             * no action, no token - enabling, disabling and issuance stay
             * two-phase operator mutations.
             */
            appliance_management_web_ui_write_acme(
                &buffer, certificates != NULL ? &certificates->acme : NULL,
                language);
            /*
             * ITEM-509 increment 8: the help index on the first page.  It is
             * the same catalog the Help section and the console print; each
             * entry links to its topic page instead of copying the text.
             */
            appliance_management_web_ui_write_help_block(&buffer, help);
        }
        if (appliance_management_web_ui_section_confirmable(section)) {
            appliance_management_web_ui_write_confirmation(
                &buffer, language, confirmation);
        }
    }
    appliance_management_web_ui_write_footer(&buffer, language);
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    if (buffer.missing_translation) {
        return APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render(
    char *output,
    size_t capacity,
    size_t *output_length)
{
    return appliance_management_web_ui_render_login(
        NULL, output, capacity, output_length);
}

/*
 * ITEM-382 about/licenses document.
 *
 * The declared inventory is rendered from `appliance_license_inventory`, the
 * compiled-in mirror of `release/hfoc-license-inventory.txt`; every declared
 * notice is forced onto the ISO by the payload contract, and the payload
 * acceptance stage verifies it there. The page publishes the state the owner
 * reported and never invents a license list: an unpublished inventory is
 * UNAVAILABLE with its reason, and a declared inventory whose payload is
 * incomplete is DEGRADED with the shipped and declared counts.
 */
static void appliance_management_web_ui_write_license_notices(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_license_inventory_dependency *dependency,
    const char *payload_root)
{
    uint32_t slot;

    for (slot = 0U;
         slot < APPLIANCE_LICENSE_INVENTORY_NOTICE_COUNT;
         ++slot) {
        const char *notice =
            appliance_license_inventory_notice(dependency, slot);

        if (notice == NULL) {
            continue;
        }
        if (slot != 0U) {
            appliance_management_web_ui_buffer_text(buffer, " ");
        }
        appliance_management_web_ui_buffer_text(buffer, "<code>");
        appliance_management_web_ui_buffer_value(buffer, payload_root);
        appliance_management_web_ui_buffer_text(buffer, "/");
        appliance_management_web_ui_buffer_value(buffer, notice);
        appliance_management_web_ui_buffer_text(buffer, "</code>");
    }
}

static void appliance_management_web_ui_write_license_row(
    struct appliance_management_web_ui_buffer *buffer,
    const struct appliance_license_inventory_dependency *dependency,
    const char *payload_root)
{
    appliance_management_web_ui_buffer_text(buffer, "<tr><td>");
    appliance_management_web_ui_buffer_value(buffer, dependency->component);
    appliance_management_web_ui_buffer_text(buffer, "</td><td>");
    appliance_management_web_ui_buffer_value(buffer, dependency->version);
    appliance_management_web_ui_buffer_text(buffer, "</td><td>");
    appliance_management_web_ui_buffer_value(buffer, dependency->license);
    appliance_management_web_ui_buffer_text(buffer, "</td><td>");
    appliance_management_web_ui_write_license_notices(
        buffer, dependency, payload_root);
    appliance_management_web_ui_buffer_text(buffer, "</td></tr>");
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render_licenses(
    const struct appliance_management_web_ui_licenses *licenses,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    const struct appliance_license_inventory_dependency *mbedtls =
        appliance_license_inventory_find("mbedtls");
    const bool declared = licenses != NULL && licenses->declared;
    const bool complete = declared && licenses->payload_complete;
    const char *reason = NULL;
    const char *payload_root = NULL;
    uint32_t index;
    const enum appliance_language language =
        licenses != NULL ? licenses->language : APPLIANCE_LANGUAGE_DEFAULT;

    if (output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    if (!declared) {
        reason = licenses != NULL && licenses->reason != NULL
                     ? licenses->reason
                     : "LICENSE_INVENTORY_UNPUBLISHED";
    } else if (!complete) {
        reason = licenses->reason != NULL ? licenses->reason
                                          : "LICENSE_PAYLOAD_INCOMPLETE";
    }
    payload_root = licenses != NULL && licenses->payload_root != NULL
                       ? licenses->payload_root
                       : appliance_license_inventory_payload_root();

    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    appliance_management_web_ui_write_head(&buffer, language);
    appliance_management_web_ui_write_nav(&buffer, language);
    appliance_management_web_ui_buffer_text(&buffer, "<main><h2>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.licenses.title");
    appliance_management_web_ui_buffer_text(
        &buffer, "</h2><p class=\"state\">");
    appliance_management_web_ui_buffer_text(
        &buffer,
        complete ? "AVAILABLE"
                 : (declared ? "DEGRADED" : "UNAVAILABLE"));
    appliance_management_web_ui_buffer_text(&buffer, "</p><p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.licenses.appliance");
    appliance_management_web_ui_buffer_text(&buffer, "<code>");
    appliance_management_web_ui_buffer_value(
        &buffer, appliance_license_inventory_license());
    appliance_management_web_ui_buffer_text(&buffer, "</code></p>");
    if (declared) {
        appliance_management_web_ui_buffer_text(&buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.payload");
        appliance_management_web_ui_buffer_text(&buffer, "<code>");
        appliance_management_web_ui_buffer_value(&buffer, payload_root);
        appliance_management_web_ui_buffer_text(&buffer, "/");
        appliance_management_web_ui_buffer_value(
            &buffer,
            licenses->manifest != NULL
                ? licenses->manifest
                : appliance_license_inventory_manifest());
        appliance_management_web_ui_buffer_text(&buffer, "</code>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.shipped");
        appliance_management_web_ui_buffer_decimal(
            &buffer, licenses->shipped_count);
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.of");
        appliance_management_web_ui_buffer_decimal(
            &buffer, licenses->declared_count);
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.declared");
        appliance_management_web_ui_buffer_text(&buffer, "</p>");
    } else {
        appliance_management_web_ui_buffer_text(&buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.none_published");
        appliance_management_web_ui_buffer_text(&buffer, "</p>\n");
    }
    if (reason != NULL) {
        appliance_management_web_ui_buffer_text(&buffer, "<p>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.reason");
        appliance_management_web_ui_buffer_value(&buffer, reason);
        appliance_management_web_ui_buffer_text(&buffer, "</p>\n");
    }
    if (declared) {
        appliance_management_web_ui_buffer_text(
            &buffer, "<table><tr><th>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.column_component");
        appliance_management_web_ui_buffer_text(&buffer, "</th><th>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.column_version");
        appliance_management_web_ui_buffer_text(&buffer, "</th><th>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.column_license");
        appliance_management_web_ui_buffer_text(&buffer, "</th><th>");
        appliance_management_web_ui_buffer_language(
            &buffer, language, "panel.licenses.column_notice");
        appliance_management_web_ui_buffer_text(&buffer, "</th></tr>");
        for (index = 0U;
             index < appliance_license_inventory_dependency_count();
             ++index) {
            const struct appliance_license_inventory_dependency *dependency =
                appliance_license_inventory_dependency(index);

            if (dependency == NULL) {
                buffer.overflow = true;
                break;
            }
            /*
             * Notice paths are published relative to the payload root stated
             * above, so the page stays inside the recorded page budget.
             */
            appliance_management_web_ui_write_license_row(
                &buffer, dependency, NULL);
        }
        appliance_management_web_ui_buffer_text(&buffer, "</table>");
    }
    appliance_management_web_ui_buffer_text(&buffer, "<p>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.licenses.election");
    appliance_management_web_ui_buffer_text(&buffer, "<code>");
    appliance_management_web_ui_buffer_value(
        &buffer, appliance_license_inventory_mbedtls_election());
    appliance_management_web_ui_buffer_text(&buffer, "</code>");
    appliance_management_web_ui_buffer_language(
        &buffer, language, "panel.licenses.election_from");
    appliance_management_web_ui_buffer_text(&buffer, "<code>");
    appliance_management_web_ui_buffer_value(
        &buffer, mbedtls != NULL ? mbedtls->license_offered : NULL);
    /*
     * ITEM-506 increment 6: adding the Account section grows every page's nav
     * by one link, and this page was within a few bytes of the 2048-byte cap;
     * the trailing sentence naming the internal LICENSE_PAYLOAD stage is what
     * moved.  The stage itself is unchanged and still named by the build
     * acceptance matrix and its documents; the page keeps the election, the
     * manifest, the payload root and the notice list.
     */
    appliance_management_web_ui_buffer_text(&buffer, "</code>.</p>");
    appliance_management_web_ui_write_footer(&buffer, language);
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    if (buffer.missing_translation) {
        return APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}

/*
 * ITEM-508 increment 34: the ACME control page.
 *
 * The durable setting the operator committed, the three two-phase actions and
 * the pending confirmation.  The page is a page of its own, rendered like the
 * licenses page, because the Dashboard - where the read-only ACME block lives -
 * is measured at its 2048-byte cap and the forms do not fit there.  Every
 * string is bounded and escaped; the page carries no certificate, key, nonce
 * or token byte, and the token the confirmation form returns is the plane's
 * own single-use confirmation token, not an ACME one.
 */
/*
 * ITEM-516: the management-gate page.  It shows the durable setting, where it
 * was read from, that it applies to the next boot, the full warning the
 * operator must read before the setting is ever enabled, and the one mutation
 * the panel owns: closing the gate.  The stage form carries `action=close`
 * only; an attempt to submit anything else is refused by name by the serving
 * path, and there is no control on this page that opens the gate.
 */
static void appliance_management_web_ui_write_gate(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const struct appliance_management_web_ui_gate *gate)
{
    appliance_management_web_ui_buffer_text(buffer, "<main><h2>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.title");
    appliance_management_web_ui_buffer_text(buffer, "</h2>");
    if (gate == NULL || !gate->published) {
        appliance_management_web_ui_buffer_text(
            buffer, "<p class=\"state\">UNAVAILABLE</p><p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.gate.none_published");
        if (gate != NULL && gate->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(buffer, gate->reason);
        }
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p class=\"state\">");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, gate->setting,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, "</p><p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.source");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, gate->source,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, ", ");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, gate->applies,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    if (gate->refusal != NULL) {
        appliance_management_web_ui_buffer_text(buffer, "<p class=\"refused\">");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.gate.refused");
        appliance_management_web_ui_buffer_value_bounded(
            buffer, gate->refusal,
            (size_t)APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX);
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    }
    appliance_management_web_ui_buffer_text(buffer, "<h3>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_title");
    appliance_management_web_ui_buffer_text(buffer, "</h3><p>");
    /*
     * The warning is the item's contract text: physical presence, the two
     * remaining protections, and audit finding R01.
     */
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_no_presence");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_password_only");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_lockout");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_wildcard");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.warning_internet");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    /*
     * Closing is always available and is the only mutation this page offers.
     * Opening is the local console's, behind a proven local secret.
     */
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.local_only");
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH
        "\"><input type=\"hidden\" name=\"action\" value=\"close\">"
        "<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.close");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n"
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH
        "\">");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer, "</button><button name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(buffer, "</button></form>\n");
    /* The way back to the section that owns this setting. */
    appliance_management_web_ui_buffer_text(
        buffer, "<p><a href=\"" "/panel/security" "\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.gate.security_link");
    appliance_management_web_ui_buffer_text(buffer, "</a></p>\n");
}

static void appliance_management_web_ui_write_acme_control(
    struct appliance_management_web_ui_buffer *buffer,
    enum appliance_language language,
    const struct appliance_management_web_ui_acme_control *control)
{
    appliance_management_web_ui_buffer_text(buffer, "<main><h2>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.title");
    appliance_management_web_ui_buffer_text(buffer, "</h2>");
    if (control == NULL || !control->published) {
        appliance_management_web_ui_buffer_text(
            buffer, "<p class=\"state\">UNAVAILABLE</p><p>");
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.acme_control.none_published");
        if (control != NULL && control->reason != NULL) {
            appliance_management_web_ui_buffer_text(buffer, ": ");
            appliance_management_web_ui_buffer_value(buffer, control->reason);
        }
        appliance_management_web_ui_buffer_language(
            buffer, language, "panel.acme_control.none_suffix");
        appliance_management_web_ui_buffer_text(buffer, "</p>\n");
        return;
    }
    appliance_management_web_ui_buffer_text(buffer, "<p class=\"state\">");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, control->enabled,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, "</p><p>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.requested");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, control->requested,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.at");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, control->requested_at,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.source");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, control->source,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX);
    appliance_management_web_ui_buffer_text(buffer, ", applies=");
    appliance_management_web_ui_buffer_value_bounded(
        buffer, control->applies,
        (size_t)APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX);
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.two_phase_note");
    appliance_management_web_ui_buffer_text(buffer, "</p>\n");
    appliance_management_web_ui_buffer_text(
        buffer,
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH
        "\"><input type=\"hidden\" name=\"action\" value=\"enable\">"
        "<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.enable");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n"
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH
        "\"><input type=\"hidden\" name=\"action\" value=\"disable\">"
        "<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.disable");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n"
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH
        "\"><input type=\"hidden\" name=\"action\" value=\"request\">"
        "<button>");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.acme_control.request");
    appliance_management_web_ui_buffer_text(
        buffer,
        "</button></form>\n"
        "<form method=\"post\" action=\"" APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH
        "\">");
    appliance_management_web_ui_write_token_field(buffer, language);
    appliance_management_web_ui_buffer_text(
        buffer, "<button name=\"confirm\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.confirm");
    appliance_management_web_ui_buffer_text(
        buffer, "</button><button name=\"cancel\" value=\"1\">");
    appliance_management_web_ui_buffer_language(
        buffer, language, "panel.form.cancel");
    appliance_management_web_ui_buffer_text(buffer, "</button></form>\n");
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render_acme_control(
    const struct appliance_management_web_ui_acme_control *control,
    const struct appliance_management_web_ui_confirmation *confirmation,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    const enum appliance_language language =
        control != NULL ? control->language : APPLIANCE_LANGUAGE_DEFAULT;

    if (output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    appliance_management_web_ui_write_head(&buffer, language);
    appliance_management_web_ui_write_nav(&buffer, language);
    appliance_management_web_ui_write_acme_control(&buffer, language, control);
    if (confirmation != NULL && confirmation->pending) {
        appliance_management_web_ui_write_mutation_confirmation(
            &buffer, language, confirmation);
    }
    appliance_management_web_ui_write_footer(&buffer, language);
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    if (buffer.missing_translation) {
        return APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}

enum appliance_management_web_ui_status
appliance_management_web_ui_render_gate(
    const struct appliance_management_web_ui_gate *gate,
    const struct appliance_management_web_ui_confirmation *confirmation,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_management_web_ui_buffer buffer;
    const enum appliance_language language =
        gate != NULL ? gate->language : APPLIANCE_LANGUAGE_DEFAULT;

    if (output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_WEB_UI_NULL;
    }
    appliance_management_web_ui_buffer_init(&buffer, output, capacity);
    appliance_management_web_ui_write_head(&buffer, language);
    appliance_management_web_ui_write_nav(&buffer, language);
    appliance_management_web_ui_write_gate(&buffer, language, gate);
    if (confirmation != NULL && confirmation->pending) {
        appliance_management_web_ui_write_mutation_confirmation(
            &buffer, language, confirmation);
    }
    appliance_management_web_ui_write_footer(&buffer, language);
    *output_length = buffer.length;
    if (buffer.overflow) {
        return APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW;
    }
    if (buffer.missing_translation) {
        return APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;
    }
    return APPLIANCE_MANAGEMENT_WEB_UI_OK;
}
