#include "appliance_acme_challenge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 7: the HTTP-01 decision.  Nothing here opens a connection
 * or writes a response: the caller hands in the request line it received and
 * gets back the exact body to send, or the named reason to refuse.
 */

#define APPLIANCE_ACME_CHALLENGE_VERSION_CURRENT 1U

static bool appliance_acme_challenge_token_valid(
    const char *text,
    size_t length)
{
    if (text == NULL || length == 0U || length > APPLIANCE_ACME_TOKEN_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (!((value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              value == (unsigned char)'-' || value == (unsigned char)'_')) {
            return false;
        }
    }
    return true;
}

/* RFC 7230 token: the bytes a method may be spelled with. */
static bool appliance_acme_challenge_method_char(unsigned char value)
{
    if ((value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
        (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
        (value >= (unsigned char)'0' && value <= (unsigned char)'9')) {
        return true;
    }
    switch (value) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        break;
    }
    return false;
}

/* A visible byte that may appear in a request target. */
static bool appliance_acme_challenge_target_char(unsigned char value)
{
    return value > 0x20U && value < 0x7FU;
}

static bool appliance_acme_challenge_version(const char *text, size_t length)
{
    static const char http10[] = "HTTP/1.0";
    static const char http11[] = "HTTP/1.1";

    return (length == sizeof(http10) - 1U &&
            memcmp(text, http10, sizeof(http10) - 1U) == 0) ||
           (length == sizeof(http11) - 1U &&
            memcmp(text, http11, sizeof(http11) - 1U) == 0);
}

static enum appliance_acme_challenge_status
appliance_acme_challenge_parse_request_line(
    const char *line,
    size_t length,
    struct appliance_acme_challenge_request *request_out)
{
    size_t method_length = 0U;
    size_t target_begin = 0U;
    size_t target_length = 0U;
    size_t version_begin = 0U;
    size_t version_length = 0U;
    size_t index = 0U;

    if (line == NULL || request_out == NULL) {
        return APPLIANCE_ACME_CHALLENGE_NULL;
    }
    if (length == 0U) {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }
    if (length >= 2U && line[length - 2U] == '\r' &&
        line[length - 1U] == '\n') {
        length -= 2U;
    } else if (line[length - 1U] == '\n') {
        length -= 1U;
    }
    if (length == 0U) {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)line[index];

        if (value < 0x20U || value == 0x7FU) {
            return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
        }
    }

    while (method_length < length && line[method_length] != ' ') {
        if (!appliance_acme_challenge_method_char(
                (unsigned char)line[method_length])) {
            return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
        }
        ++method_length;
    }
    if (method_length == 0U ||
        method_length > APPLIANCE_ACME_CHALLENGE_METHOD_MAX ||
        method_length >= length) {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }

    target_begin = method_length + 1U;
    if (target_begin >= length || line[target_begin] == ' ') {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }
    while (target_begin + target_length < length &&
           line[target_begin + target_length] != ' ') {
        if (!appliance_acme_challenge_target_char(
                (unsigned char)line[target_begin + target_length])) {
            return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
        }
        ++target_length;
    }
    if (target_length == 0U || line[target_begin] != '/') {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }
    if (target_begin + target_length >= length) {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }

    version_begin = target_begin + target_length + 1U;
    if (version_begin >= length || line[version_begin] == ' ') {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }
    while (version_begin + version_length < length) {
        if (line[version_begin + version_length] == ' ') {
            return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
        }
        ++version_length;
    }
    if (!appliance_acme_challenge_version(
            &line[version_begin], version_length)) {
        return APPLIANCE_ACME_CHALLENGE_BAD_REQUEST;
    }

    if (target_length > APPLIANCE_ACME_CHALLENGE_PATH_MAX) {
        /* Nothing is copied: a truncated target is never a decision. */
        return APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL;
    }
    memcpy(request_out->method, line, method_length);
    request_out->method[method_length] = '\0';
    request_out->method_length = (uint32_t)method_length;
    memcpy(request_out->path, &line[target_begin], target_length);
    request_out->path[target_length] = '\0';
    request_out->path_length = (uint32_t)target_length;
    return APPLIANCE_ACME_CHALLENGE_OK;
}

enum appliance_acme_challenge_status
appliance_acme_challenge_read_request_line(
    const char *line,
    size_t length,
    struct appliance_acme_challenge_request *request_out)
{
    if (request_out != NULL) {
        memset(request_out, 0, sizeof(*request_out));
    }
    return appliance_acme_challenge_parse_request_line(
        line, length, request_out);
}

enum appliance_acme_challenge_status appliance_acme_challenge_arm_token(
    struct appliance_acme_challenge_arm *arm,
    const char *token,
    size_t token_length,
    const char *key_authorization,
    size_t key_authorization_length,
    uint32_t now_epoch_seconds)
{
    if (arm == NULL || token == NULL || key_authorization == NULL) {
        return APPLIANCE_ACME_CHALLENGE_NULL;
    }
    if (!appliance_acme_challenge_token_valid(token, token_length)) {
        return APPLIANCE_ACME_CHALLENGE_BAD_TOKEN;
    }
    if (key_authorization_length == 0U ||
        key_authorization_length > APPLIANCE_ACME_CHALLENGE_ANSWER_MAX ||
        now_epoch_seconds == 0U ||
        /*
         * A clock this late would wrap the deadline into the past and arm a
         * token that never expires; refuse instead of arming it.
         */
        now_epoch_seconds >
            UINT32_MAX - APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS) {
        return APPLIANCE_ACME_CHALLENGE_BAD_STATE;
    }
    memset(arm, 0, sizeof(*arm));
    arm->version = APPLIANCE_ACME_CHALLENGE_VERSION_CURRENT;
    memcpy(arm->token, token, token_length);
    arm->token[token_length] = '\0';
    arm->token_length = (uint32_t)token_length;
    memcpy(arm->answer, key_authorization, key_authorization_length);
    arm->answer[key_authorization_length] = '\0';
    arm->answer_length = (uint32_t)key_authorization_length;
    arm->armed_at_epoch_seconds = now_epoch_seconds;
    arm->deadline_epoch_seconds =
        now_epoch_seconds + APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS;
    arm->served_count = 0U;
    arm->armed = true;
    return APPLIANCE_ACME_CHALLENGE_OK;
}

void appliance_acme_challenge_disarm(struct appliance_acme_challenge_arm *arm)
{
    if (arm != NULL) {
        memset(arm, 0, sizeof(*arm));
    }
}

static bool appliance_acme_challenge_method(
    const char *method,
    size_t method_length,
    bool *head_out)
{
    if (method == NULL) {
        return false;
    }
    if (method_length == 3U && memcmp(method, "GET", 3U) == 0) {
        *head_out = false;
        return true;
    }
    if (method_length == 4U && memcmp(method, "HEAD", 4U) == 0) {
        *head_out = true;
        return true;
    }
    return false;
}

enum appliance_acme_challenge_status appliance_acme_challenge_decide(
    struct appliance_acme_challenge_arm *arm,
    const char *method,
    size_t method_length,
    const char *path,
    size_t path_length,
    uint32_t now_epoch_seconds,
    const char **body_out,
    size_t *body_length_out)
{
    static const char prefix[] = APPLIANCE_ACME_CHALLENGE_PREFIX;
    bool head = false;
    size_t prefix_length = sizeof(prefix) - 1U;

    if (arm == NULL || body_out == NULL || body_length_out == NULL) {
        return APPLIANCE_ACME_CHALLENGE_NULL;
    }
    *body_out = NULL;
    *body_length_out = 0U;
    if (!arm->armed || arm->version != APPLIANCE_ACME_CHALLENGE_VERSION_CURRENT) {
        return APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE;
    }
    if (!appliance_acme_challenge_method(method, method_length, &head)) {
        return APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED;
    }
    /*
     * The exact path, byte for byte: the prefix and the armed token, with no
     * query string, no trailing slash and no other segment.  A request target
     * that carries a query is not this path.
     */
    if (path == NULL || path_length != prefix_length + arm->token_length ||
        memcmp(path, prefix, prefix_length) != 0 ||
        memcmp(&path[prefix_length], arm->token, arm->token_length) != 0) {
        return APPLIANCE_ACME_CHALLENGE_NOT_FOUND;
    }
    if (now_epoch_seconds == 0U ||
        now_epoch_seconds > arm->deadline_epoch_seconds) {
        /* The listener never serves a stale token; the caller closes it. */
        return APPLIANCE_ACME_CHALLENGE_EXPIRED;
    }
    arm->served_count += 1U;
    if (head) {
        *body_out = NULL;
        *body_length_out = 0U;
        return APPLIANCE_ACME_CHALLENGE_OK;
    }
    *body_out = arm->answer;
    *body_length_out = arm->answer_length;
    return APPLIANCE_ACME_CHALLENGE_OK;
}

enum appliance_acme_challenge_status appliance_acme_challenge_decide_line(
    struct appliance_acme_challenge_arm *arm,
    const char *line,
    size_t line_length,
    uint32_t now_epoch_seconds,
    const char **body_out,
    size_t *body_length_out)
{
    struct appliance_acme_challenge_request request;
    enum appliance_acme_challenge_status status =
        appliance_acme_challenge_read_request_line(line, line_length,
                                                   &request);

    if (status != APPLIANCE_ACME_CHALLENGE_OK) {
        if (body_out != NULL) {
            *body_out = NULL;
        }
        if (body_length_out != NULL) {
            *body_length_out = 0U;
        }
        return status;
    }
    return appliance_acme_challenge_decide(
        arm, request.method, request.method_length, request.path,
        request.path_length, now_epoch_seconds, body_out, body_length_out);
}

const char *appliance_acme_challenge_status_string(
    enum appliance_acme_challenge_status status)
{
    switch (status) {
    case APPLIANCE_ACME_CHALLENGE_OK:
        return "OK";
    case APPLIANCE_ACME_CHALLENGE_NULL:
        return "NULL";
    case APPLIANCE_ACME_CHALLENGE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_CHALLENGE_BAD_TOKEN:
        return "BAD_TOKEN";
    case APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE:
        return "NO_CHALLENGE";
    case APPLIANCE_ACME_CHALLENGE_NOT_FOUND:
        return "NOT_FOUND";
    case APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED:
        return "METHOD_NOT_ALLOWED";
    case APPLIANCE_ACME_CHALLENGE_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case APPLIANCE_ACME_CHALLENGE_BAD_REQUEST:
        return "BAD_REQUEST";
    default:
        break;
    }
    return "UNKNOWN";
}
