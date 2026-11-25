#ifndef HFOC_APPLIANCE_ACME_HTTP_H
#define HFOC_APPLIANCE_ACME_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 10: the HTTP exchange the ACME client speaks, without a
 * socket and without TLS.
 *
 * The order module produces one request at a time and consumes one reply at a
 * time; between them sits an HTTP exchange this module owns in both
 * directions.  It is deliberately transport-free: the caller hands in the
 * exact bytes a server answered and gets back the status, the headers the item
 * acts on and the body, so the whole exchange is provable on the host with
 * fixed data, and the socket-and-TLS increment above it has nothing left to
 * decide.
 *
 * Direction one, the request:
 *
 *   * the URL is parsed once into host, port and target.  Only an absolute
 *     `https://` URL is accepted - the item's only outbound connection is the
 *     server the directory named - and the authority carries no userinfo and
 *     no IPv6 literal; an IPv4 literal is marked so the transport can skip the
 *     resolver.  The port defaults to 443 and an explicit one is kept, because
 *     the offline ACME stub of the QEMU proof listens on 127.0.0.1:14000;
 *   * the request carries exactly what the item needs and nothing else:
 *     `METHOD target HTTP/1.1`, `Host`, and for a POST `Content-Type` and
 *     `Content-Length`, then `Connection: close`, then the body.  GET and HEAD
 *     carry no body and no content type; a method that is not GET, HEAD or
 *     POST is refused, and an empty POST body is refused because an ACME POST
 *     is a JWS;
 *   * the caller owns the buffer, and a request that does not fit in it is
 *     refused whole - the buffer is never left holding half a request.
 *
 * Direction two, the reply:
 *
 *   * the status line must be `HTTP/1.0` or `HTTP/1.1` with a three-digit
 *     code; anything else is `BAD_STATUS_LINE`, and a body the caller did not
 *     receive whole is `TRUNCATED`, never a shorter answer;
 *   * headers are read with their exact names (`Location`, `Replay-Nonce`,
 *     `Content-Type`, `Content-Length`) and any other header is skipped, but
 *     an obs-fold, a control byte in a value, a duplicate of a header the item
 *     acts on, or more headers than the bound is a refusal;
 *   * `Transfer-Encoding` is refused by name: a chunked body is not parsed
 *     here, and pretending it was would hand the caller framing bytes as
 *     payload.  The transport asks for `Connection: close` and the item's
 *     replies are read whole;
 *   * `Content-Length`, when present, must equal the body actually received;
 *     a body longer than the caller's buffer is `BUFFER_TOO_SMALL` and nothing
 *     is partially copied;
 *   * the parse publishes one bounded record `[ACME-HTTP] response status=…
 *     bytes=…`; the body is never echoed into it, and neither is a nonce.
 */

#define APPLIANCE_ACME_HTTP_VERSION 1U
#define APPLIANCE_ACME_HTTP_URL_MAX 255U
#define APPLIANCE_ACME_HTTP_HOST_MAX 253U
#define APPLIANCE_ACME_HTTP_METHOD_MAX 8U
#define APPLIANCE_ACME_HTTP_REQUEST_MAX 2816U
#define APPLIANCE_ACME_HTTP_RESPONSE_MAX 8192U
#define APPLIANCE_ACME_HTTP_HEADER_BLOCK_MAX 4096U
#define APPLIANCE_ACME_HTTP_HEADER_COUNT_MAX 32U
#define APPLIANCE_ACME_HTTP_BODY_MAX 6144U
#define APPLIANCE_ACME_HTTP_NONCE_MAX 128U
#define APPLIANCE_ACME_HTTP_LOCATION_MAX APPLIANCE_ACME_HTTP_URL_MAX
#define APPLIANCE_ACME_HTTP_CONTENT_TYPE_MAX 64U
#define APPLIANCE_ACME_HTTP_RECORD_MAX 160U
#define APPLIANCE_ACME_HTTP_PORT_DEFAULT 443U

enum appliance_acme_http_status {
    APPLIANCE_ACME_HTTP_OK = 0,
    APPLIANCE_ACME_HTTP_NULL = 1,
    /* Not an absolute https URL, or one with a fragment or userinfo. */
    APPLIANCE_ACME_HTTP_BAD_URL = 2,
    APPLIANCE_ACME_HTTP_NOT_HTTPS = 3,
    APPLIANCE_ACME_HTTP_BAD_HOST = 4,
    APPLIANCE_ACME_HTTP_BAD_PORT = 5,
    APPLIANCE_ACME_HTTP_BAD_METHOD = 6,
    APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE = 7,
    APPLIANCE_ACME_HTTP_BAD_BODY = 8,
    APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL = 9,
    APPLIANCE_ACME_HTTP_BAD_STATUS_LINE = 10,
    APPLIANCE_ACME_HTTP_BAD_HEADER = 11,
    APPLIANCE_ACME_HTTP_HEADER_TOO_LONG = 12,
    APPLIANCE_ACME_HTTP_TOO_MANY_HEADERS = 13,
    /* The body is shorter than the exchange promised. */
    APPLIANCE_ACME_HTTP_TRUNCATED = 14,
    APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH = 15,
    /* A framing this module deliberately does not parse. */
    APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED = 16,
    /* A header the item acts on was answered twice. */
    APPLIANCE_ACME_HTTP_DUPLICATE_HEADER = 17,
};

/*
 * Where one request goes.  `literal_ipv4` is set only for a dotted-quad
 * authority, so the transport can skip the resolver instead of asking it for a
 * name that is already an address.
 */
struct appliance_acme_http_target {
    char host[APPLIANCE_ACME_HTTP_HOST_MAX + 1U];
    uint32_t host_length;
    uint16_t port;
    bool literal_ipv4;
    /* The URL bound subsumes the target: it cannot be longer than its URL. */
    char target[APPLIANCE_ACME_HTTP_URL_MAX + 1U];
    uint32_t target_length;
    char record[APPLIANCE_ACME_HTTP_RECORD_MAX + 1U];
};

/*
 * One reply, read whole.  The headers the item acts on are optional and say so
 * (`has_nonce`/`has_location`); the body is the caller's buffer.  The item
 * always sends the recorded request itself, so the response carries no
 * `Content-Type` requirement: a certificate body is PEM and a JSON body is
 * checked by the module that parses it.
 */
struct appliance_acme_http_response {
    uint32_t status_code;
    bool has_nonce;
    char nonce[APPLIANCE_ACME_HTTP_NONCE_MAX + 1U];
    uint32_t nonce_length;
    bool has_location;
    char location[APPLIANCE_ACME_HTTP_LOCATION_MAX + 1U];
    uint32_t location_length;
    bool has_content_type;
    char content_type[APPLIANCE_ACME_HTTP_CONTENT_TYPE_MAX + 1U];
    uint32_t content_type_length;
    bool has_content_length;
    uint32_t content_length;
    uint32_t body_length;
    uint8_t body[APPLIANCE_ACME_HTTP_BODY_MAX];
    char record[APPLIANCE_ACME_HTTP_RECORD_MAX + 1U];
};

/*
 * Parse one absolute URL into where the request goes and which target it asks
 * for.  The host is kept as written (a DNS name is case-insensitive, and the
 * transport hands it to SNI); the target is the origin-form path and query,
 * which must begin with `/`, and a URL with an empty path becomes `/`.
 */
enum appliance_acme_http_status appliance_acme_http_parse_url(
    const char *url,
    size_t url_length,
    struct appliance_acme_http_target *target_out);

/*
 * Build one request into the caller's buffer.  A POST carries the content type
 * and the body; GET and HEAD carry neither and refuse a non-empty body.  The
 * length written is the whole request, and a buffer that cannot hold it is
 * refused with the buffer left empty.
 */
enum appliance_acme_http_status appliance_acme_http_build_request(
    const char *method,
    size_t method_length,
    const struct appliance_acme_http_target *target,
    const char *content_type,
    size_t content_type_length,
    const uint8_t *body,
    size_t body_length,
    char *out,
    size_t capacity,
    size_t *length_out);

/*
 * Read one reply the transport received whole.  `bytes` is the exact exchange:
 * status line, headers, the empty line and the body.  Every refusal leaves the
 * response zeroed rather than half filled.
 */
enum appliance_acme_http_status appliance_acme_http_read_response(
    const uint8_t *bytes,
    size_t length,
    struct appliance_acme_http_response *response_out);

/* True for 200..299: the item's successes are 200 and 201. */
bool appliance_acme_http_status_is_success(uint32_t status_code);

const char *appliance_acme_http_status_string(
    enum appliance_acme_http_status status);

#endif
