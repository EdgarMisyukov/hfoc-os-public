#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#define HTTP_PARSER_VERSION 1U
#define HTTP_PARSER_SLOT_COUNT 4U
#define HTTP_PARSER_GENERATION_MIN 1U
#define HTTP_PARSER_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HTTP_PARSER_REQUEST_LINE_MAX 1024U
#define HTTP_PARSER_HEADERS_MAX 4096U
#define HTTP_REQUEST_LINE_VERSION 1U
#define HTTP_HEADER_VERSION 1U
#define HTTP_FRAMING_VERSION 1U
#define HTTP_METHOD_VERSION_POLICY_VERSION 1U
#define HTTP_ROUTE_TABLE_VERSION 1U
#define HTTP_ROUTE_COUNT 3U
#define HTTP_ROUTE_ROOT_LENGTH 1U
#define HTTP_ROUTE_HEALTH_LENGTH 7U
/* ITEM-503: the appliance certificate, downloadable before any session. */
#define HTTP_ROUTE_CERTIFICATE_LENGTH 12U

enum http_parser_state {
    HTTP_PARSER_STATE_EMPTY = 0,
    HTTP_PARSER_STATE_REQUEST_LINE = 1,
    HTTP_PARSER_STATE_HEADERS = 2,
    HTTP_PARSER_STATE_COMPLETE = 3,
    HTTP_PARSER_STATE_REJECTED = 4,
};

enum http_parser_status {
    HTTP_PARSER_OK = 0,
    HTTP_PARSER_NULL = 1,
    HTTP_PARSER_BAD_STATE = 2,
    HTTP_PARSER_BAD_TOKEN = 3,
    HTTP_PARSER_STALE_TOKEN = 4,
    HTTP_PARSER_NOT_ATTACHED = 5,
    HTTP_PARSER_ALREADY_ATTACHED = 6,
    HTTP_PARSER_INVALID_PREDECESSOR = 7,
    HTTP_PARSER_INVALID_PROGRESS = 8,
    HTTP_PARSER_REQUEST_LINE_LIMIT = 9,
    HTTP_PARSER_HEADERS_LIMIT = 10,
    HTTP_PARSER_TERMINAL = 11,
    HTTP_PARSER_REQUEST_LINE_INCOMPLETE = 12,
    HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER = 13,
    HTTP_PARSER_REQUEST_LINE_INVALID_SPACING = 14,
    HTTP_PARSER_REQUEST_LINE_INVALID_TARGET = 15,
    HTTP_PARSER_REQUEST_LINE_INVALID_VERSION = 16,
    HTTP_PARSER_HEADER_INCOMPLETE = 17,
    HTTP_PARSER_HEADER_INVALID_NAME = 18,
    HTTP_PARSER_HEADER_INVALID_VALUE = 19,
    HTTP_PARSER_HEADER_FOLDING = 20,
    HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION = 21,
    HTTP_PARSER_HEADER_OUT_OF_RANGE = 22,
};

enum http_framing_kind {
    HTTP_FRAMING_NONE = 0,
    HTTP_FRAMING_CONTENT_LENGTH = 1,
    HTTP_FRAMING_TRANSFER_ENCODING = 2,
    HTTP_FRAMING_EXPECT = 3,
    HTTP_FRAMING_UPGRADE = 4,
    HTTP_FRAMING_DUPLICATE = 5,
    HTTP_FRAMING_CONFLICT = 6,
    HTTP_FRAMING_INVALID = 7,
};

struct http_parser_token {
    uint32_t index;
    uint32_t generation;
};

struct http_parser_progress {
    uint32_t request_line_offset;
    uint32_t request_line_count;
    uint32_t header_offset;
    uint32_t header_count;
    uint32_t total_count;
};

struct http_request_line_fields {
    uint32_t method_offset;
    uint32_t method_length;
    uint32_t target_offset;
    uint32_t target_length;
    uint32_t version_offset;
    uint32_t version_length;
    uint32_t line_length;
};

struct http_request_line {
    const uint8_t *method;
    uint32_t method_length;
    const uint8_t *target;
    uint32_t target_length;
    const uint8_t *version;
    uint32_t version_length;
    uint32_t line_length;
};

struct http_header_field {
    const uint8_t *name;
    uint32_t name_length;
    const uint8_t *value;
    uint32_t value_length;
};

/*
 * ITEM-445 step 2 prototype: the appliance accepts a bounded request body
 * instead of denying every Content-Length outright - first the url-encoded
 * sign-in form, and (ITEM-503) the certificate and key an operator uploads for
 * the BYO import.  A body is present only for POST and only up to
 * HTTP_FRAMING_BODY_MAX; every other Content-Length stays denied exactly as
 * before.  The caller must consume exactly `content_length` bytes before it
 * completes the request.
 *
 * ITEM-503 raised the bound from 512 to 8192 because a certificate chain plus
 * a private key is kilobytes, not bytes, and a plain HTML form cannot chunk
 * without script.  The bound is per request and the per-connection body buffer
 * is sized by it.
 */
#define HTTP_FRAMING_BODY_MAX 8192U

struct http_framing_result {
    bool body_denied;
    bool upgrade_denied;
    bool body_present;
    uint32_t content_length;
    enum http_framing_kind reason;
    uint32_t field_index;
};

enum http_method_kind {
    HTTP_METHOD_NONE = 0,
    HTTP_METHOD_GET = 1,
    HTTP_METHOD_HEAD = 2,
    HTTP_METHOD_UNSUPPORTED = 3,
    /*
     * ITEM-445 step 2: POST exists for the browser forms only.  The value is
     * appended so every existing comparison keeps its meaning.
     * ITEM-503 adds the certificate import form to that list.
     */
    HTTP_METHOD_POST = 4,
};

/*
 * The POST targets that accept a form body, and their exact bytes.  `/` is the
 * sign-in form, `/panel/logout` the sign-out form, `/panel/security` the BYO
 * certificate form, `/api/v1/system/certificate` the scriptable entry to the
 * same two-phase certificate mutation, `/panel/routing` the route-table form
 * and `/api/v1/system/routes` the scriptable entry to the same two-phase
 * routing mutation.  ITEM-505 adds `/panel/network` (the network form) and
 * `/api/v1/network` (the scriptable entry to the same two-phase
 * network-configuration mutation), and ITEM-505 increment 3 adds
 * `/panel/system` (the hostname form) and `/api/v1/system` (the scriptable
 * entry to the same two-phase hostname mutation; its GET body keeps its
 * read-only meaning).  Every other target keeps answering 405 for POST.
 */
#define HTTP_POLICY_FORM_TARGET "/"
#define HTTP_POLICY_FORM_TARGET_LENGTH 1U
#define HTTP_POLICY_SIGN_OUT_TARGET "/panel/logout"
#define HTTP_POLICY_SIGN_OUT_TARGET_LENGTH 13U
#define HTTP_POLICY_SECURITY_TARGET "/panel/security"
#define HTTP_POLICY_SECURITY_TARGET_LENGTH 15U
#define HTTP_POLICY_CERTIFICATE_TARGET "/api/v1/system/certificate"
#define HTTP_POLICY_CERTIFICATE_TARGET_LENGTH 26U
#define HTTP_POLICY_ROUTING_TARGET "/panel/routing"
#define HTTP_POLICY_ROUTING_TARGET_LENGTH 14U
#define HTTP_POLICY_ROUTES_TARGET "/api/v1/system/routes"
#define HTTP_POLICY_ROUTES_TARGET_LENGTH 21U
#define HTTP_POLICY_NETWORK_TARGET "/api/v1/network"
#define HTTP_POLICY_NETWORK_TARGET_LENGTH 15U
#define HTTP_POLICY_PANEL_NETWORK_TARGET "/panel/network"
#define HTTP_POLICY_PANEL_NETWORK_TARGET_LENGTH 14U
#define HTTP_POLICY_SYSTEM_TARGET "/api/v1/system"
#define HTTP_POLICY_SYSTEM_TARGET_LENGTH 14U
#define HTTP_POLICY_PANEL_SYSTEM_TARGET "/panel/system"
#define HTTP_POLICY_PANEL_SYSTEM_TARGET_LENGTH 13U
/* ITEM-506 increment 6: the panel form and the scriptable account change. */
#define HTTP_POLICY_ACCOUNT_TARGET "/api/v1/system/account"
#define HTTP_POLICY_ACCOUNT_TARGET_LENGTH 22U
#define HTTP_POLICY_PANEL_ACCOUNT_TARGET "/panel/account"
#define HTTP_POLICY_PANEL_ACCOUNT_TARGET_LENGTH 14U
/*
 * ITEM-507 increments 4-5: the name -> certificate/key table.  The scriptable
 * target and the panel page target are the same two-phase mutation; both are
 * form targets, and neither has a read meaning.
 */
#define HTTP_POLICY_CERTIFICATE_NAME_TARGET \
    "/api/v1/system/certificate/name"
#define HTTP_POLICY_CERTIFICATE_NAME_TARGET_LENGTH 31U
#define HTTP_POLICY_PANEL_CERTIFICATES_TARGET "/panel/certificates"
#define HTTP_POLICY_PANEL_CERTIFICATES_TARGET_LENGTH 19U
/*
 * ITEM-508 increment 34: the durable ACME control.  The panel control page and
 * the scriptable API target are the same two-phase mutation; both are form
 * targets, and only the panel target has a GET meaning (the rendered page).
 */
#define HTTP_POLICY_ACME_TARGET "/api/v1/system/acme"
#define HTTP_POLICY_ACME_TARGET_LENGTH 20U
#define HTTP_POLICY_PANEL_ACME_TARGET "/panel/acme"
#define HTTP_POLICY_PANEL_ACME_TARGET_LENGTH 12U
/*
 * ITEM-513: the nav's one-click language switch.  It is a POST target like the
 * forms above - one bounded body, `language=en|ru` - and it has no read
 * meaning: a GET is the panel's own 405, never a rendered page.
 */
#define HTTP_POLICY_PANEL_LANGUAGE_TARGET "/panel/language"
#define HTTP_POLICY_PANEL_LANGUAGE_TARGET_LENGTH 15U
/*
 * ITEM-516: the management gate's panel page and scriptable mutation target.
 * Both are form targets: the panel posts the one safe action (`close`), the
 * API twin accepts the same body for a script, and neither is a read target.
 */
#define HTTP_POLICY_GATE_TARGET "/api/v1/system/gate"
#define HTTP_POLICY_GATE_TARGET_LENGTH 21U
#define HTTP_POLICY_PANEL_GATE_TARGET "/panel/gate"
#define HTTP_POLICY_PANEL_GATE_TARGET_LENGTH 12U

enum http_policy_reason {
    HTTP_POLICY_NONE = 0,
    HTTP_POLICY_METHOD = 1,
    HTTP_POLICY_VERSION = 2,
    HTTP_POLICY_MISSING_HOST = 3,
    HTTP_POLICY_EMPTY_HOST = 4,
    HTTP_POLICY_DUPLICATE_HOST = 5,
    HTTP_POLICY_INVALID_HOST = 6,
};

enum http_route_id {
    HTTP_ROUTE_NONE = 0,
    HTTP_ROUTE_ROOT = 1,
    HTTP_ROUTE_HEALTH = 2,
    HTTP_ROUTE_CERTIFICATE = 3,
};

enum http_route_status {
    HTTP_ROUTE_OK = 0,
    HTTP_ROUTE_NULL = 1,
};

struct http_policy_result {
    bool allowed;
    bool method_allowed;
    bool version_allowed;
    bool host_valid;
    enum http_method_kind method;
    enum http_policy_reason reason;
    uint32_t field_index;
};

struct http_route_descriptor {
    enum http_route_id id;
    const uint8_t *target;
    uint32_t target_length;
};

struct http_parser_entry {
    bool active;
    uint32_t generation;
    enum http_parser_state state;
    struct http_parser_progress progress;
    uint32_t request_line_stored;
    uint8_t request_line_stage;
    bool request_line_complete;
    enum http_parser_status request_line_error;
    struct http_request_line_fields request_line_fields;
    uint8_t request_line_data[HTTP_PARSER_REQUEST_LINE_MAX];
    uint32_t header_stored;
    uint8_t header_stage;
    bool header_complete;
    uint32_t header_fields;
    enum http_parser_status header_error;
    uint8_t header_data[HTTP_PARSER_HEADERS_MAX];
};

struct http_parser {
    bool published;
    uint32_t version;
    uint32_t active_count;
    struct http_parser_entry entries[HTTP_PARSER_SLOT_COUNT];
};

enum http_parser_status http_parser_init(
    struct http_parser *parser);
enum http_parser_status http_parser_start(
    struct http_parser *parser,
    const struct http_parser_token *token);
enum http_parser_status http_parser_advance(
    struct http_parser *parser,
    const struct http_parser_token *token,
    enum http_parser_state next_state,
    const struct http_parser_progress *progress);
enum http_parser_status http_parser_reject(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const struct http_parser_progress *progress);
enum http_parser_status http_parser_release(
    struct http_parser *parser,
    const struct http_parser_token *token);

bool http_parser_published(
    const struct http_parser *parser);
uint32_t http_parser_active_count(
    const struct http_parser *parser);
uint32_t http_parser_request_line_max(
    const struct http_parser *parser);
uint32_t http_parser_headers_max(
    const struct http_parser *parser);

enum http_parser_state http_parser_state_at(
    const struct http_parser *parser,
    uint32_t index);
uint32_t http_parser_generation_at(
    const struct http_parser *parser,
    uint32_t index);
enum http_parser_status http_parser_progress_at(
    const struct http_parser *parser,
    uint32_t index,
    struct http_parser_progress *progress);

enum http_parser_state http_parser_state(
    const struct http_parser *parser,
    const struct http_parser_token *token);
uint32_t http_parser_generation(
    const struct http_parser *parser,
    const struct http_parser_token *token);
enum http_parser_status http_parser_progress(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_parser_progress *progress);

enum http_parser_status http_request_line_feed(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length,
    bool *complete);
enum http_parser_status http_request_line_get(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_request_line *line);
enum http_parser_status http_request_line_error(
    const struct http_parser *parser,
    const struct http_parser_token *token);
enum http_parser_status http_header_feed(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length,
    bool *complete);
enum http_parser_status http_header_count(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    uint32_t *count);
enum http_parser_status http_header_field(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    uint32_t index,
    struct http_header_field *field);
enum http_parser_status http_header_error(
    const struct http_parser *parser,
    const struct http_parser_token *token);
enum http_parser_status http_framing_check(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_framing_result *result);
const char *http_framing_kind_string(
    enum http_framing_kind kind);
enum http_parser_status http_policy_check(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_policy_result *result);
const char *http_method_kind_string(
    enum http_method_kind kind);
const char *http_policy_reason_string(
    enum http_policy_reason reason);
enum http_route_status http_route_table(
    const struct http_route_descriptor **routes,
    uint32_t *count);
enum http_route_status http_route_match(
    const uint8_t *target,
    uint32_t target_length,
    enum http_route_id *route);
enum http_route_status http_route_match_line(
    const struct http_request_line *line,
    enum http_route_id *route);
const char *http_route_id_string(
    enum http_route_id route);

const char *http_parser_state_string(
    enum http_parser_state state);
const char *http_parser_status_string(
    enum http_parser_status status);

#endif
