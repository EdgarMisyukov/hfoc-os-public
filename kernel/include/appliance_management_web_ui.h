#ifndef HFOC_APPLIANCE_MANAGEMENT_WEB_UI_H
#define HFOC_APPLIANCE_MANAGEMENT_WEB_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"

/*
 * ITEM-384 rendered management panel.
 *
 * The ITEM-345 module published one static shell document. ITEM-384 turns it
 * into server-side rendered HTML pages: one sign-in page served on the
 * anonymous root target and one page per management section, all built into
 * caller-owned bounded buffers, with no allocation, no scripting runtime, no
 * external asset, and no CDN.
 *
 * Every dynamic value a page shows (hostname, MAC address, service name, log
 * line, notice, owner path) passes through `appliance_management_web_ui_escape`
 * before it is appended, so a hostile value cannot inject markup into the
 * panel. An injection here would be a management-plane compromise: the panel
 * is served from the management listener with an administrator session.
 *
 * Section state is published honestly. A section with a read-side owner names
 * that owner. A section whose owner is a mutation-only surface is DEGRADED. A
 * section with no owner at all is UNAVAILABLE. A section is never hidden.
 *
 * The module owns no storage, network, TLS, session, or credential. It never
 * mutates appliance state and it never decides authorization: the panel
 * dispatch layer does that with the existing management authorization matrix.
 */

#define APPLIANCE_MANAGEMENT_WEB_UI_VERSION 6U
#define APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT 13U
/*
 * ITEM-509 increment 8: the help surfaces (the Help section's table of
 * contents, one page per topic and the Dashboard's help block) need more than
 * the previous 2048-byte ceiling.  The new ceiling is the measured worst case
 * plus headroom, and the price is deliberate: `struct
 * appliance_management_panel` carries four page buffers of this size, so the
 * raise costs 4 x 1280 bytes of static RAM, and it stays far below the
 * transport limit the documents are written through (`TCP_SND_BUF` is eight
 * MSS in `kernel/include/lwipopts.h`, so a ~3.2 KiB document is still one TLS
 * record in one `tcp_write()`).  The recorded values in
 * `docs/hfoc-management-web-ui.md` move with it.
 *
 * ITEM-509 increment 10: the ceiling is raised from 3328 to 3584 because the
 * Russian navigation the panel now renders is longer than the English one (the
 * longest Russian help topic measured 3179 bytes *with the English
 * navigation*), and the ceiling has to hold the worst page in either language.
 * The price is the same four static page buffers as increment 8: 4 x 256 bytes
 * more, still one TLS record.  The per-page budgets stay the measured English
 * worst cases until increment 11 translates the page bodies and re-measures
 * `max(en, ru)` page by page (see `docs/hfoc-language-surfaces.md`).
 *
 * ITEM-513: the ceiling is raised from 3584 to 3840 for the nav's one-click
 * language switch.  The switch adds the same 171-byte form to every panel page
 * (the `EN`/`RU` buttons and their `style="display:inline"`), and the worst
 * page - the Russian Help topic - measured 3481 bytes before it, so 3584 no
 * longer holds the page the panel renders.  The price is the same four static
 * page buffers: 4 x 256 bytes more, still one TLS record below the eight-MSS
 * send buffer.  `HTTP_RESPONSE_BODY_MAX` moves with it.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX 3840U
#define APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX 96U
#define APPLIANCE_MANAGEMENT_WEB_UI_ESCAPE_MAX \
    (APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX * 6U)

/*
 * Recorded per-page asset budget: the exact byte count of HTML plus inline
 * CSS that the panel serves for that page. The recorded value is the measured
 * value, so any growth beyond it fails the host test until the budget and the
 * recorded table in `docs/hfoc-management-web-ui.md` are updated together.
 *
 * ITEM-509 increment 12: the forms, the two-phase confirmations and the
 * licenses page now render their prose from the catalog, so every budget that
 * carries a form is the measured `max(en, ru)` and the login budget covers the
 * signed-in document as well - the sign-out form, the session pair, a notice
 * and the throttle line - instead of the anonymous shell alone.  The measured
 * values after the increment: login 2461, network 2430, services 2375,
 * system 2394, security 2568, updates 2474, recovery 2484, routing 2501,
 * account 2531, certificates 2477, licenses 2331, acme 2418; dashboard 2590,
 * storage 2071, diagnostics 2075 and help 3460 did not move, and the worst
 * page is still well inside `..._BODY_MAX` (3584).
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN 2461U
/*
 * ITEM-505 increment 2: the generic sections (Dashboard, Services, Storage,
 * System, Diagnostics, Updates, Recovery) are measured with the worst case the
 * live production panel view can publish - the bounds the compose module
 * enforces - and not only with the short QEMU fixture values, so these are the
 * budgets a real appliance renders under.
 * ITEM-508 increment 32 grows the Dashboard by the read-only ACME state block
 * (five values, each bounded by `..._ACME_VALUE_MAX`): measured worst case was
 * 1911 bytes.  ITEM-508 increment 34 adds the link to the ACME control page
 * (1942) and increment 37 the order path's own `STATE/STEP` (1973), so the
 * measured worst case is 1973 bytes, recorded here and in
 * `docs/hfoc-management-web-ui.md`.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD 2759U
/*
 * ITEM-505: the Network page grew by the two readings, the mode form and the
 * pending-confirmation block; measured worst case is 2013 bytes (ITEM-506
 * increment 6 re-measured it with the Account link and the real longest
 * mutation name in the pending block).  ITEM-515 gave every control its label
 * and re-measured the worst case at 2754 bytes.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_NETWORK 2922U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SERVICES 2544U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_STORAGE 2240U
/*
 * ITEM-505 increment 3: the System page grew by the two hostname readings and
 * the one-field stage form.  ITEM-506 increment 6 adds the Account link to
 * every page's navigation, so the measured worst case is 1810 bytes.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U
/*
 * ITEM-503: the Security page grew by the BYO certificate import form and its
 * pending-confirmation block; measured worst case is 1976 bytes (ITEM-506
 * increment 6 re-measured it with the Account link and the real longest
 * mutation name in the pending block).
 */
/*
 * ITEM-516 re-measured this page: it carries one service link to the
 * management-gate page now, which grew the document by 167 bytes.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SECURITY 3209U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DIAGNOSTICS 2244U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_UPDATES 2643U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_RECOVERY 2653U
/*
 * ITEM-504: the Routing page carries the applied table plus the stage and
 * confirmation forms; measured worst case is recorded here and in
 * `docs/hfoc-management-web-ui.md`.  ITEM-506 increment 6 adds the Account
 * link to every page's navigation.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ROUTING 3532U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES 2736U
/*
 * ITEM-506 increment 6: the Account page carries the stored login, the
 * four-field stage form and the confirmation form; measured worst case is
 * recorded here and in `docs/hfoc-management-web-ui.md`.  Adding the section
 * also grows every other section page and the licenses page by one nav link,
 * so those rows move with it.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACCOUNT 3073U
/*
 * ITEM-507 increment 5: the Certificates page lists both name tables its owner
 * published, carries the import, delete and default forms and the confirmation
 * form; measured worst case is recorded here and in
 * `docs/hfoc-management-web-ui.md`.  Adding the section grows every other
 * section page and the licenses page by one nav link, so those rows move with
 * it.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_CERTIFICATES 3123U
/*
 * ITEM-508 increment 34: the ACME control page carries the durable setting,
 * the three stage forms and the confirmation form; measured worst case is
 * 1985 bytes, recorded here and in `docs/hfoc-management-web-ui.md`.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACME 2797U
/*
 * ITEM-509 increment 8: the Help section's own page.  The recorded budget is
 * the measured worst case over both shapes the section serves - the table of
 * contents and the longest topic page (`storage`, whose Russian body is the
 * longest text in the catalog) - because both are one page in the recorded
 * table.  ITEM-509 increment 10 re-measured it at 3350 bytes: the worst page
 * is the Russian topic page, and its navigation is Russian too now.
 * ITEM-509 increment 25 re-measured it at 3481 bytes: the index sentence and
 * the way back are translated now, so the Russian topic page grew by the
 * longer label.  The value is the measurement; see
 * `docs/hfoc-management-web-ui.md`.
 *
 * ITEM-513 re-measured every page that carries the navigation after the
 * language switch joined it: each such page grew by the same 171-byte form,
 * and the Help budget below is the new measurement.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP 3650U
/*
 * ITEM-516: the management-gate page carries the durable setting, the full
 * warning, the close-only stage form, the token confirmation form and the
 * pending block.  The value is the measured worst case.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_GATE 3648U
#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_MAX 3840U

/* Panel targets. The panel module dispatches exactly this static table. */
#define APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH "/panel"
#define APPLIANCE_MANAGEMENT_WEB_UI_PANEL_PATH_LENGTH 6U
/*
 * ITEM-382 about/licenses surface. It is published like every panel page and
 * is not one of the nine management sections: the section model stays exact.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH "/panel/about/licenses"
#define APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH_LENGTH 21U
#define APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_LABEL "Licenses"
/*
 * ITEM-508 increment 34: the ACME control page.  It is a page of its own, not
 * one of the twelve management sections: the read-only ACME block lives on the
 * Dashboard, whose page is measured at its 2048-byte cap, and the two-phase
 * control forms do not fit there.  The page is published like every panel page
 * and the section model stays exact.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH "/panel/acme"
#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH_LENGTH 11U

/*
 * ITEM-516: the management gate's page.  Like the ACME control page it is a
 * page of its own: the warning the operator must read before the setting is
 * ever enabled does not fit on a section page, and the close-only two-phase
 * form needs its own target.  The nav does not carry it; the Security page
 * links to it, and every page is authorized through the same `PANEL` route.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH "/panel/gate"
#define APPLIANCE_MANAGEMENT_WEB_UI_GATE_PATH_LENGTH 11U

/*
 * ITEM-513: the panel's language switch target.  The nav on every panel page
 * carries one POST form with two submit buttons, `EN` and `RU`; a click sends
 * the one field `language=en|ru` to this target, which answers the one
 * `hfoc_lang` cookie and sends the browser to the dashboard.  The target is a
 * panel target like the sections (it is authorized through the same `PANEL`
 * route) but it is not a section: it renders no document of its own.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH "/panel/language"
#define APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH_LENGTH 15U

/*
 * ITEM-509 increment 8: the Help section and its topic pages.
 *
 * The section's declared path is `/panel/help`; a topic page is that path, a
 * slash and a catalog id (`/panel/help/install`).  The topic ids are the
 * catalog's, so the Help surface has no second topic table to drift from the
 * console's `help <topic>`.  A topic page renders all four catalog fields of
 * one topic (title, summary, body and related), and the section's index page
 * renders the table of contents; the Dashboard carries a short help block
 * that links the topics.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH "/panel/help"
#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH_LENGTH 11U
#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX "/panel/help/"
#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH 12U
/*
 * The longest topic id in the catalog is `certificates` (12 bytes); the bound
 * covers any future id without accepting an unbounded path segment.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_MAX 31U

enum appliance_management_web_ui_section {
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD = 0,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_NETWORK = 1,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SERVICES = 2,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STORAGE = 3,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM = 4,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY = 5,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DIAGNOSTICS = 6,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_UPDATES = 7,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_RECOVERY = 8,
    /*
     * ITEM-504: the first mutating panel section.  It is appended after the
     * nine read-side sections so their indices, and the `page = section + 1`
     * mapping, do not move.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING = 9,
    /*
     * ITEM-506 increment 6: the account section.  It is appended after the
     * routing section so every earlier index, and the `page = section + 1`
     * mapping, do not move.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT = 10,
    /*
     * ITEM-507 increment 5: the certificates section.  It is appended after
     * the account section so every earlier index, and the `page = section + 1`
     * mapping, do not move.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES = 11,
    /*
     * ITEM-509 increment 8: the help section, appended last so every earlier
     * index and the `page = section + 1` mapping stay exact.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP = 12,
};

/*
 * A page is the anonymous sign-in document, one section document, or the
 * ITEM-382 about/licenses document. The section pages are offset by one so
 * `page = section + 1` stays exact; the licenses page follows the sections.
 */
enum appliance_management_web_ui_page {
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LOGIN = 0,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_DASHBOARD = 1,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_NETWORK = 2,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_SERVICES = 3,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_STORAGE = 4,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_SYSTEM = 5,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_SECURITY = 6,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_DIAGNOSTICS = 7,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_UPDATES = 8,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_RECOVERY = 9,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ROUTING = 10,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACCOUNT = 11,
    /*
     * ITEM-507 increment 5: the certificates section page; the licenses page
     * follows the sections, so it moves with the new section.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_CERTIFICATES = 12,
    /*
     * ITEM-509 increment 8: the help section page; every page after the
     * sections moves with the thirteenth section.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_HELP = 13,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES = 14,
    /* ITEM-508 increment 34: the ACME control page follows the licenses page. */
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME = 15,
    /* ITEM-516: the management-gate page follows the ACME control page. */
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_GATE = 16,
    APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT = 17,
};

enum appliance_management_web_ui_section_state {
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED = 0,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED = 1,
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE = 2,
};

enum appliance_management_web_ui_status {
    APPLIANCE_MANAGEMENT_WEB_UI_OK = 0,
    APPLIANCE_MANAGEMENT_WEB_UI_NULL = 1,
    APPLIANCE_MANAGEMENT_WEB_UI_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_WEB_UI_OVERFLOW = 3,
    APPLIANCE_MANAGEMENT_WEB_UI_BAD_SECTION = 4,
    /*
     * ITEM-509 increment 10: the catalog has no value for a key the page has
     * to print in the language the render was asked for.  The page is refused
     * with this named status instead of being published half translated or
     * silently English: the panel maps it to a 503 that names the cause.
     */
    APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION = 5,
};

/*
 * Dynamic values shown by a page. Every field is optional; a null field is
 * rendered as the literal `unavailable` instead of being hidden.
 */
struct appliance_management_web_ui_view {
    const char *hostname;
    const char *mac_address;
    const char *service_name;
    const char *log_line;
    const char *detail;
    /*
     * ITEM-509 increment 10: the language this render serves the page in, the
     * one the dispatcher parsed from the same `v1:system.language` record the
     * System page publishes.  Default 0 is English, so a caller that does not
     * fill the field renders exactly the document it rendered before this
     * increment; an out-of-range value is refused with
     * `APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION` rather than falling
     * back to English in silence.
     */
    enum appliance_language language;
};

/*
 * Session presentation. The module renders the state it is given; it never
 * authenticates and never issues a session.
 */
struct appliance_management_web_ui_session {
    bool authenticated;
    uint32_t session_id;
    uint32_t principal_id;
    const char *notice;
    uint64_t lockout_remaining_ticks;
    /* ITEM-509 increment 10: the language the sign-in document is served in. */
    enum appliance_language language;
};

/*
 * ITEM-382 about/licenses presentation.
 *
 * The page publishes the declared dependency inventory that the compiled-in
 * `appliance_license_inventory` mirror and the payload contract carry; it does
 * not read a file and it does not verify a checksum itself. The owner states
 * what it found:
 *
 *   - nothing published (`declared == false`) renders UNAVAILABLE with the
 *     owner reason, or `LICENSE_INVENTORY_UNPUBLISHED`;
 *   - a declared inventory whose payload is incomplete renders DEGRADED with
 *     the owner reason, or `LICENSE_PAYLOAD_INCOMPLETE`, and the shipped and
 *     declared counts;
 *   - a declared, complete payload renders AVAILABLE.
 *
 * The page never renders an empty or invented license list: an unavailable
 * inventory is published as UNAVAILABLE with its reason.
 */
struct appliance_management_web_ui_licenses {
    bool declared;
    bool payload_complete;
    const char *manifest;
    const char *payload_root;
    const char *reason;
    uint32_t declared_count;
    uint32_t shipped_count;
    /* ITEM-509 increment 10: the language this page is served in. */
    enum appliance_language language;
};

/*
 * Two-phase confirmation presentation. A mutation is single-use and
 * time-bounded: the page shows the pending token, the mutation it authorizes,
 * and the remaining window. The panel never confirms by itself.
 */
struct appliance_management_web_ui_confirmation {
    bool pending;
    uint64_t token;
    const char *mutation;
    uint64_t remaining_ticks;
    bool single_use;
};

/*
 * ITEM-504: the applied route table the Routing page lists.  The route
 * configuration owns the table; this is the bounded copy of what its owner
 * published, so the page lists what is applied instead of inventing an empty
 * table.  A table too large for the page is counted and flagged (`truncated`),
 * never silently cut, and a table that was never published keeps `published`
 * false so the page says so.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_ROUTES_TEXT_MAX 224U
struct appliance_management_web_ui_routes {
    bool published;
    uint32_t count;
    uint32_t shown;
    bool truncated;
    char text[APPLIANCE_MANAGEMENT_WEB_UI_ROUTES_TEXT_MAX];
};

/*
 * ITEM-505: the network section's two readings.
 *
 * A network change committed from the panel is durable but is not applied to
 * the live interface: the machine keeps serving the address it booted with
 * until the next boot, exactly as the console menu has said since ITEM-496
 * (`applies=reboot current=...`).  The page therefore shows both facts - the
 * active configuration the wire runs now and the saved configuration the next
 * boot will apply - and names where the saved value came from.  The owner
 * publishes the two bounded renderings; the page never invents a value, and a
 * section whose owner published nothing says so instead of showing a blank.
 * The strings are borrowed for the render call and stay owned by the owner.
 */
struct appliance_management_web_ui_network {
    bool published;
    const char *active;
    const char *saved;
    const char *source;
    const char *applies;
    const char *reason;
};

/*
 * ITEM-505 increment 3: the system section's two readings.
 *
 * A hostname committed from the panel is durable but is not applied to the
 * running identity: the listener keeps the name (and the certificate name) the
 * machine booted with until the next boot, exactly as a network change waits
 * for the reboot.  The page shows the active identity the running system
 * serves, the saved hostname the next boot will apply and the source of the
 * saved record; the owner publishes the bounded renderings and the page never
 * invents a value.  A section whose owner published nothing says so instead of
 * showing a blank, and the strings are borrowed for the render call.
 */
struct appliance_management_web_ui_system {
    bool published;
    const char *active;
    const char *saved;
    const char *source;
    const char *applies;
    const char *reason;
    /*
     * ITEM-509 increment 5: the interface language pair, read from the same
     * system record the hostname comes from.  `language_active` is the
     * language this render serves in, `language_saved` is what the record
     * holds - the panel never invents either, and a record that did not load
     * publishes UNAVAILABLE instead of a value.  The switch that changes it is
     * the next increment; this one makes the panel tell the truth about the
     * language the machine is set to.
     */
    const char *language_active;
    const char *language_saved;
};

/*
 * ITEM-506 increment 6: the account section's one reading.
 *
 * The change is immediate, unlike the hostname and the network configuration:
 * the account record is written at the commit, the verifier is republished
 * and every issued session - including the one that confirmed the change - is
 * closed.  The page therefore shows the stored login only; the password is
 * never rendered and never stored by the page.  A section whose owner
 * published nothing says so instead of showing a blank, and the strings are
 * borrowed for the render call.
 */
struct appliance_management_web_ui_account {
    bool published;
    const char *login;
    const char *reason;
};

/*
 * ITEM-507 increment 5: the certificates section's two tables and their
 * declarations.
 *
 * The name -> certificate/key table the SNI callback selects from is durable
 * but not rebound live: a mutation saves the next boot's table while the
 * running listener keeps the table it was configured with.  The page therefore
 * lists the active table (what this boot serves, with the source it was read
 * from) next to the saved table (what the next boot will read), names the
 * default declaration of each, and says `applies=reboot`.  Each table is a
 * bounded rendering of `name (bytes)` pairs: a table too large for the page is
 * counted and flagged (`truncated`), never silently cut, and the strings are
 * borrowed for the render call.  No key material is ever part of the view.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_CERTIFICATES_TEXT_MAX 64U

/*
 * ITEM-508 increment 32: the ACME state block of the Certificates page.
 *
 * ITEM-508 point 10 asks the panel for the ACME state - enabled or not, the
 * names, the expiry and the last attempt with its result - as a **read-only**
 * view: enabling, disabling and starting an issuance are the two-phase
 * mutation plane's business, not this page's.  The block is composed by the
 * production serving path from the ACME service's own getters and rendered
 * beside the name tables; it carries the service state, the account's
 * bound/refused verdict, the renewal decision with the expiry and the attempt
 * count, and it never carries a key, a nonce, a token or a certificate byte.
 *
 * Every string is caller-owned and bounded, and the renderer escapes it.  A
 * block that is not published renders one line naming `reason` instead of
 * disappearing, so "the panel does not know" is never mistaken for "ACME is
 * off".
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_TEXT_MAX 64U
/*
 * The bound one ACME value is rendered at.  The block renders five values on
 * the Dashboard, whose page is measured against a 2048-byte cap, so each value
 * is bounded here rather than at the renderer's generic 96: the producers'
 * longest real values (the service state, the account verdict, the renewal
 * decision and two decimal numbers) are far shorter than this.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX 24U
struct appliance_management_web_ui_acme {
    bool published;
    const char *state;
    const char *account;
    const char *renewal;
    const char *expires;
    const char *attempts;
    /*
     * ITEM-508 increment 37: the order path's own `STATE/STEP`, so the
     * operator sees whether the issuance they requested is running, finished
     * or refused by name - not only that it was requested.  Bounded by
     * `..._ACME_VALUE_MAX` at the renderer like every other value.
     */
    const char *order;
    const char *reason;
};

struct appliance_management_web_ui_certificates {
    bool published;
    uint32_t count;
    uint32_t shown;
    bool truncated;
    const char *active;
    const char *active_default;
    const char *active_source;
    const char *saved;
    const char *saved_default;
    const char *saved_source;
    const char *applies;
    const char *reason;
    /*
     * ITEM-508 increment 32: nested rather than a new render parameter, so
     * every existing caller of `appliance_management_web_ui_render_section`
     * keeps its signature and a caller that does not fill the block renders
     * the named "not published" line.
     */
    struct appliance_management_web_ui_acme acme;
};

/*
 * ITEM-508 increment 34: the ACME control page's own view.
 *
 * `enabled`, `requested` and the pending request's instant are the durable
 * setting the operator committed; `source` names where the record was read
 * from; `applies` says honestly whether the running service already follows
 * the saved setting or whether it takes effect on the next boot; `reason`
 * names why nothing is published.  Every string is caller-owned and bounded,
 * and the renderer escapes it.  The page carries no certificate, key or token
 * byte.
 *
 * The bound is the producers' longest real value, not the renderer's generic
 * 96: the serving composition publishes `ENABLED`/`DISABLED`, `REQUESTED`/
 * `NONE`, a decimal instant, a source word (`DEFAULT_UNAVAILABLE` is the
 * longest) and `service`, and the page is measured against a 2048-byte cap.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_CONTROL_TEXT_MAX 24U
struct appliance_management_web_ui_acme_control {
    bool published;
    const char *enabled;
    const char *requested;
    const char *requested_at;
    const char *source;
    const char *applies;
    const char *reason;
    /* ITEM-509 increment 10: the language this page is served in. */
    enum appliance_language language;
};

/*
 * ITEM-509 increment 8: the Help section's inputs.
 *
 * The help text is the catalog the console's `help` command already prints,
 * so the panel renderer owns no second copy of a topic.  The only dynamic
 * inputs are the language the machine is set to (`language`, an `en`/`ru`
 * code read from the same system record as the System page's pair) and, for a
 * topic page, the topic id (`topic`, a catalog id, or NULL for the section's
 * table of contents).  `published == false`, an unparseable language or an
 * incomplete catalog renders a named refusal instead of an empty page, so "the
 * panel does not know" is never mistaken for "there is no help".  The strings
 * are borrowed for the render call.
 */
struct appliance_management_web_ui_help {
    bool published;
    const char *language;
    const char *topic;
    const char *reason;
};

/*
 * ITEM-516: the management-gate page's own view.
 *
 * `setting` is the policy the system record holds (`OPEN_AFTER_BOOT` or
 * `CLOSED_AFTER_BOOT`), `source` names where the value was read from,
 * `applies` says honestly that the policy takes effect on the next boot, and
 * `refusal` carries the stable name an attempt to open from the panel was
 * refused with (never a silent success).  Every string is caller-owned and
 * bounded, and the renderer escapes it.  The page carries no secret.
 */
#define APPLIANCE_MANAGEMENT_WEB_UI_GATE_TEXT_MAX 24U
struct appliance_management_web_ui_gate {
    bool published;
    const char *setting;
    const char *source;
    const char *applies;
    const char *refusal;
    const char *reason;
    /* ITEM-509 increment 10: the language this page is served in. */
    enum appliance_language language;
};

/* The anonymous sign-in document served on the root target. */
const uint8_t *appliance_management_web_ui_shell_body(void);
uint32_t appliance_management_web_ui_shell_length(void);
const char *appliance_management_web_ui_section_string(
    enum appliance_management_web_ui_section section);
const char *appliance_management_web_ui_section_state_string(
    enum appliance_management_web_ui_section_state state);
const char *appliance_management_web_ui_status_string(
    enum appliance_management_web_ui_status status);

/* Panel target served for a section. */
const char *appliance_management_web_ui_section_path(
    enum appliance_management_web_ui_section section);
/* Owning management API path, or NULL when no owner exists. */
const char *appliance_management_web_ui_section_owner(
    enum appliance_management_web_ui_section section);
enum appliance_management_web_ui_section_state
appliance_management_web_ui_section_state(
    enum appliance_management_web_ui_section section);
/* True when the section owns a two-phase mutation surface. */
bool appliance_management_web_ui_section_confirmable(
    enum appliance_management_web_ui_section section);
/* Recorded asset budget for a page, or 0 for an unknown page. */
uint32_t appliance_management_web_ui_page_budget(
    enum appliance_management_web_ui_page page);

/* Escape a dynamic value into caller-owned storage. */
enum appliance_management_web_ui_status
appliance_management_web_ui_escape(
    const char *input,
    size_t input_length,
    char *output,
    size_t capacity,
    size_t *output_length);

/* Render the sign-in document for a session state. */
enum appliance_management_web_ui_status
appliance_management_web_ui_render_login(
    const struct appliance_management_web_ui_session *session,
    char *output,
    size_t capacity,
    size_t *output_length);

/* Render one section document. */
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
    /*
     * ITEM-509 increment 8: the Help section's language and selected topic,
     * and the Dashboard's help block.  A NULL help pointer means "nothing was
     * published": the Help page names that instead of rendering an empty
     * document.
     */
    const struct appliance_management_web_ui_help *help,
    char *output,
    size_t capacity,
    size_t *output_length);

/* Render the about/licenses document for a declared-inventory state. */
enum appliance_management_web_ui_status
appliance_management_web_ui_render_licenses(
    const struct appliance_management_web_ui_licenses *licenses,
    char *output,
    size_t capacity,
    size_t *output_length);

/*
 * Render the ACME control page: the durable setting, the two-phase forms and
 * the pending confirmation.  Rendered standalone like the licenses page
 * because it is a page of its own, not one of the twelve sections.
 */
enum appliance_management_web_ui_status
appliance_management_web_ui_render_acme_control(
    const struct appliance_management_web_ui_acme_control *control,
    const struct appliance_management_web_ui_confirmation *confirmation,
    char *output,
    size_t capacity,
    size_t *output_length);

/*
 * Render the management-gate page: the durable setting, the full warning, the
 * close-only two-phase forms and the pending confirmation.  Rendered
 * standalone like the licenses and ACME pages because it is a page of its own,
 * not one of the twelve sections.
 */
enum appliance_management_web_ui_status
appliance_management_web_ui_render_gate(
    const struct appliance_management_web_ui_gate *gate,
    const struct appliance_management_web_ui_confirmation *confirmation,
    char *output,
    size_t capacity,
    size_t *output_length);

/* Render the anonymous sign-in document, exactly as the shell body. */
enum appliance_management_web_ui_status
appliance_management_web_ui_render(
    char *output,
    size_t capacity,
    size_t *output_length);

#endif
