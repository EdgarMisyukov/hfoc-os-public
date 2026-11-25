#ifndef HFOC_APPLIANCE_ACME_CHALLENGE_H
#define HFOC_APPLIANCE_ACME_CHALLENGE_H

#include "appliance_acme_message.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 7: the HTTP-01 answer, as a decision.
 *
 * The item's point 8 is explicit: the challenge listener exists **only** for
 * the task - it answers exactly `/.well-known/acme-challenge/<token>` for the
 * token that is currently armed, answers nothing else, does not redirect, and
 * closes once the check is over.  This module owns that decision and the
 * bounded lifetime; the transport that accepts the connection and writes the
 * answer is the next increment.
 *
 * The listener reads exactly one request line, so the whole input path is
 * provable here: `read_request_line` parses `METHOD SP TARGET SP VERSION`
 * (origin-form target, `HTTP/1.0` or `HTTP/1.1`, an optional trailing CRLF)
 * into fixed buffers, and `decide_line` runs that parse into `decide`.
 *
 * Fail-closed:
 *
 *   * a path that is not exactly the well-known prefix followed by the armed
 *     token (no query string, no extra segment, no case folding, no percent
 *     decoding) is `NOT_FOUND`;
 *   * a method that is not GET or HEAD is `METHOD_NOT_ALLOWED` - an ACME
 *     server never POSTs a challenge;
 *   * a request line that is not exactly the three tokens, or that carries a
 *     control byte, a relative target, an asterisk-form target or a version
 *     this listener does not speak, is `BAD_REQUEST`; a target longer than the
 *     caller's buffer is `BUFFER_TOO_SMALL`, and nothing is copied;
 *   * nothing armed, an armed answer whose deadline has passed, or an answer
 *     that was already consumed is `NO_CHALLENGE` / `EXPIRED`: the listener
 *     never serves a stale token, and it closes after the check;
 *   * arming refuses a clock whose 900 s lifetime would wrap the 32-bit
 *     deadline, rather than arming a token that can never expire;
 *   * HEAD answers the headers with no body, and the body of a GET is exactly
 *     the key authorization - never a directory listing, never a redirect.
 */

#define APPLIANCE_ACME_CHALLENGE_VERSION 1U
#define APPLIANCE_ACME_CHALLENGE_PREFIX "/.well-known/acme-challenge/"
#define APPLIANCE_ACME_CHALLENGE_ANSWER_MAX 512U
#define APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS 900U
#define APPLIANCE_ACME_CHALLENGE_METHOD_MAX 15U
#define APPLIANCE_ACME_CHALLENGE_PATH_MAX 320U

enum appliance_acme_challenge_status {
    APPLIANCE_ACME_CHALLENGE_OK = 0,
    APPLIANCE_ACME_CHALLENGE_NULL = 1,
    APPLIANCE_ACME_CHALLENGE_BAD_STATE = 2,
    APPLIANCE_ACME_CHALLENGE_BAD_TOKEN = 3,
    APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE = 4,
    APPLIANCE_ACME_CHALLENGE_NOT_FOUND = 5,
    APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED = 6,
    APPLIANCE_ACME_CHALLENGE_EXPIRED = 7,
    APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL = 8,
    APPLIANCE_ACME_CHALLENGE_BAD_REQUEST = 9,
};

struct appliance_acme_challenge_request {
    char method[APPLIANCE_ACME_CHALLENGE_METHOD_MAX + 1U];
    uint32_t method_length;
    char path[APPLIANCE_ACME_CHALLENGE_PATH_MAX + 1U];
    uint32_t path_length;
};

struct appliance_acme_challenge_arm {
    uint32_t version;
    bool armed;
    char token[APPLIANCE_ACME_TOKEN_MAX + 1U];
    uint32_t token_length;
    char answer[APPLIANCE_ACME_CHALLENGE_ANSWER_MAX + 1U];
    uint32_t answer_length;
    uint32_t armed_at_epoch_seconds;
    uint32_t deadline_epoch_seconds;
    uint32_t served_count;
};

/*
 * Parse one request line.  `line` is what the listener read up to and
 * including the CRLF, or the same text without the CRLF; every byte of it is
 * validated.  On success the method and the target (query string included, if
 * any) are copied into `request_out` and the line's third token is checked to
 * be a version this listener speaks.
 */
enum appliance_acme_challenge_status
appliance_acme_challenge_read_request_line(
    const char *line,
    size_t length,
    struct appliance_acme_challenge_request *request_out);

/* Arm one token with the key authorization that answers it. */
enum appliance_acme_challenge_status appliance_acme_challenge_arm_token(
    struct appliance_acme_challenge_arm *arm,
    const char *token,
    size_t token_length,
    const char *key_authorization,
    size_t key_authorization_length,
    uint32_t now_epoch_seconds);

/*
 * Decide one request.  `method` is the request's method as it arrived; `path`
 * is the request target with its query string, if any, still attached - a
 * target with a query is not the exact path and is refused.
 */
enum appliance_acme_challenge_status appliance_acme_challenge_decide(
    struct appliance_acme_challenge_arm *arm,
    const char *method,
    size_t method_length,
    const char *path,
    size_t path_length,
    uint32_t now_epoch_seconds,
    const char **body_out,
    size_t *body_length_out);

/*
 * The listener's whole decision, from the line it read to the body it writes:
 * parse, then decide.  A malformed line is `BAD_REQUEST`, and it never reaches
 * `decide`.
 */
enum appliance_acme_challenge_status appliance_acme_challenge_decide_line(
    struct appliance_acme_challenge_arm *arm,
    const char *line,
    size_t line_length,
    uint32_t now_epoch_seconds,
    const char **body_out,
    size_t *body_length_out);

/* Close the challenge: after this every request is NO_CHALLENGE. */
void appliance_acme_challenge_disarm(struct appliance_acme_challenge_arm *arm);

const char *appliance_acme_challenge_status_string(
    enum appliance_acme_challenge_status status);

#endif
