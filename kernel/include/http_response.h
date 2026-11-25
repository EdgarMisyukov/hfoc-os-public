#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stdint.h>

#define HTTP_RESPONSE_VERSION 1U
#define HTTP_RESPONSE_HEAD_VERSION 1U
/*
 * ITEM-384: the rendered management panel serves documents up to
 * APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX, so the wire buffer and the body cap
 * grow with it. The head grew by the Content-Security-Policy line.
 * ITEM-445 step 2 adds the two headers a browser sign-in answers with,
 * `Set-Cookie` and `Location`, so the buffer grows by both bounds.
 * ITEM-509 increment 8 raises the wire body cap with the panel's Help pages:
 * the longest measured page is 3179 bytes, so a 2048-byte cap rejected it with
 * `HTTP_RESPONSE_BODY_TOO_LARGE` even though the panel rendered it.
 * ITEM-509 increment 10 raises it again with the panel's page buffers, to 3584:
 * the Russian navigation the panel now renders is longer than the English one,
 * so the longest Help topic grows past the old cap.  The assembled buffer
 * itself is unchanged - 3584 bytes of body plus the ~400-byte head stay inside
 * 4256, which is still one TLS record below the eight-MSS `TCP_SND_BUF`.
 *
 * ITEM-513 raises both bounds by 256 again: the nav's one-click language switch
 * adds the same form to every panel page, and the worst page (the Russian Help
 * topic) measured 3481 bytes before it, so the body cap moves to 3840 and the
 * assembled buffer to 4512 - still one TLS record below the eight-MSS send
 * buffer.  The four panel page buffers and the per-connection response buffer
 * grow by 256 bytes each; no JSON API bound moves.
 *
 * `HTTP_RESPONSE_API_BODY_MAX` is deliberately a separate, smaller bound: the
 * JSON API documents are bounded at 2048 bytes, and aliasing them to the wire
 * cap would grow every API body buffer (five of them, several on the serving
 * stack) by 1792 bytes for no API that needs it.
 */
#define HTTP_RESPONSE_BUFFER_MAX 4512U
#define HTTP_RESPONSE_BODY_MAX 3840U
#define HTTP_RESPONSE_API_BODY_MAX 2048U
#define HTTP_RESPONSE_SET_COOKIE_MAX 160U
#define HTTP_RESPONSE_LOCATION_MAX 64U

/*
 * ITEM-384: the panel publishes denials instead of hiding them, so the
 * encoder needs the unauthenticated, denied, throttled and unavailable
 * statuses.
 * ITEM-445 step 2: the sign-in form posts and is answered with 303 See Other
 * so a browser reload does not repost the credential, and the policy allows
 * that one form to post to its own origin while still loading no script.
 */
#define HTTP_RESPONSE_CONTENT_SECURITY_POLICY \
    "Content-Security-Policy: default-src 'none'; " \
    "style-src 'unsafe-inline'; base-uri 'none'; " \
    "form-action 'self'; frame-ancestors 'none'"

enum http_status_code {
    HTTP_STATUS_200_OK = 200,
    HTTP_STATUS_303_SEE_OTHER = 303,
    HTTP_STATUS_400_BAD_REQUEST = 400,
    HTTP_STATUS_401_UNAUTHORIZED = 401,
    HTTP_STATUS_403_FORBIDDEN = 403,
    HTTP_STATUS_404_NOT_FOUND = 404,
    HTTP_STATUS_405_METHOD_NOT_ALLOWED = 405,
    HTTP_STATUS_413_PAYLOAD_TOO_LARGE = 413,
    HTTP_STATUS_414_URI_TOO_LONG = 414,
    HTTP_STATUS_429_TOO_MANY_REQUESTS = 429,
    HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_STATUS_503_SERVICE_UNAVAILABLE = 503,
};

enum http_content_type {
    HTTP_CONTENT_TYPE_HTML_UTF8 = 1,
    HTTP_CONTENT_TYPE_JSON = 2,
    /* ITEM-503: the active certificate, served as a PEM document. */
    HTTP_CONTENT_TYPE_PEM = 3,
};

enum http_response_status {
    HTTP_RESPONSE_OK = 0,
    HTTP_RESPONSE_NULL = 1,
    HTTP_RESPONSE_BAD_STATUS = 2,
    HTTP_RESPONSE_BAD_CONTENT_TYPE = 3,
    HTTP_RESPONSE_BODY_TOO_LARGE = 4,
    HTTP_RESPONSE_BUFFER_TOO_SMALL = 5,
};

struct http_response {
    enum http_status_code status;
    enum http_content_type content_type;
    const uint8_t *body;
    uint32_t body_length;
    /*
     * ITEM-445 step 2: the two headers the browser sign-in adds. A response
     * that carries neither leaves both pointers NULL and both lengths zero,
     * which is the pre-item head byte for byte.
     */
    const uint8_t *set_cookie;
    uint32_t set_cookie_length;
    const uint8_t *location;
    uint32_t location_length;
};

uint32_t http_response_length(
    const struct http_response *response);
uint32_t http_response_head_length(
    const struct http_response *response);
enum http_response_status http_response_encode(
    const struct http_response *response,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written);
enum http_response_status http_response_encode_head(
    const struct http_response *response,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written);
const char *http_status_code_string(
    enum http_status_code status);
const char *http_content_type_string(
    enum http_content_type content_type);
const char *http_response_status_string(
    enum http_response_status status);

#endif
