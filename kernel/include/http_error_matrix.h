#ifndef HTTP_ERROR_MATRIX_H
#define HTTP_ERROR_MATRIX_H

#include "http_parser.h"
#include "http_response.h"

#include <stddef.h>

#define HTTP_ERROR_MATRIX_VERSION 1U

enum http_error_matrix_status {
    HTTP_ERROR_MATRIX_OK = 0,
    HTTP_ERROR_MATRIX_NULL = 1,
    HTTP_ERROR_MATRIX_NOT_ERROR = 2,
    HTTP_ERROR_MATRIX_UNMAPPED = 3,
};

enum http_error_matrix_status http_error_matrix_map_parser(
    enum http_parser_status parser_status,
    enum http_status_code *http_status);
enum http_error_matrix_status http_error_matrix_map_policy(
    enum http_policy_reason policy_reason,
    enum http_status_code *http_status);
enum http_error_matrix_status http_error_matrix_map_framing(
    const struct http_framing_result *framing,
    enum http_status_code *http_status);
enum http_error_matrix_status http_error_matrix_map_route(
    enum http_route_id route,
    enum http_status_code *http_status);
const char *http_error_matrix_status_string(
    enum http_error_matrix_status status);

/*
 * ITEM-498: the body of a refusal.
 *
 * Every refusal used to carry the same word - `rejected` - under
 * `Content-Type: application/json`, whatever it was and whatever asked for it.
 * The owner's browser showed a JSON parse error instead of the panel's sign-in
 * page, and a program could not tell a 404 from a 503 without parsing the
 * status line alone.  Two shapes now, chosen by the route family:
 *
 *   * `[INPUT-...]`/API targets get valid JSON naming the status and the
 *     reason - `{"status":401,"error":"unauthorized"}` - so any JSON client
 *     reads the refusal instead of failing on it;
 *   * the browser targets (`/`, `/health`, `/panel` and its sections) get a
 *     small HTML document a person can read, with the status and a link to the
 *     panel sign-in page.
 *
 * The status is decided where it always was; this is the body and its content
 * type, and nothing here changes an authorization decision.
 */
enum http_error_body_family {
    HTTP_ERROR_BODY_JSON = 0,
    HTTP_ERROR_BODY_HTML = 1,
};

/* The longer of the two shapes, with room for the widest status text. */
#define HTTP_ERROR_BODY_MAX 512U

/* The family a target belongs to; a NULL target is an API refusal. */
enum http_error_body_family http_error_matrix_body_family(
    const uint8_t *target,
    size_t target_length);
/* The stable reason word of a status, for the JSON body and the HTML text. */
const char *http_error_matrix_reason(enum http_status_code status);
/* The two bodies, or 0 when the buffer is too small (never a truncation). */
size_t http_error_matrix_json_body(
    enum http_status_code status,
    uint8_t *buffer,
    size_t size);
size_t http_error_matrix_html_body(
    enum http_status_code status,
    uint8_t *buffer,
    size_t size);

#endif
