#ifndef HFOC_APPLIANCE_ACME_MESSAGE_H
#define HFOC_APPLIANCE_ACME_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 3: the ACME replies the client role parses.
 *
 * RFC 8555 answers one question at a time with a JSON object, and this module
 * reads exactly the answers the item needs - the directory, an account, an
 * order, an authorization with its challenges, and the finalized order that
 * names the certificate URL - into fixed structures the caller owns.  There is
 * no dynamic memory, no document tree and no copying beyond the bounded text
 * fields; every field is validated as the thing it claims to be before it is
 * stored:
 *
 *   * a URL must be absolute `https://` and printable, because the item's
 *     only outbound connection is the server this reply names;
 *   * a challenge token must be the base64url alphabet, because it is about to
 *     be written into a `.well-known` path and printed on the wire;
 *   * a status must be one of the statuses RFC 8555 defines for that object; a
 *     status this module does not know is a refusal, not an `UNKNOWN` that a
 *     caller could mistake for progress;
 *   * the identifier list and the authorization list are bounded and counted,
 *     so a reply cannot make the caller walk off its own arrays.
 *
 * The item's HTTP-01 path is the only challenge type recognised: an
 * authorization whose challenges carry no `http-01` entry is a refusal, and
 * the challenge's own status is read so the caller knows whether to answer.
 */

#define APPLIANCE_ACME_URL_MAX 255U
#define APPLIANCE_ACME_TOKEN_MAX 255U
#define APPLIANCE_ACME_NAME_MAX 253U
#define APPLIANCE_ACME_IDENTIFIER_MAX 8U
#define APPLIANCE_ACME_AUTHORIZATION_MAX 8U
#define APPLIANCE_ACME_CHALLENGE_MAX 8U

enum appliance_acme_message_status {
    APPLIANCE_ACME_MESSAGE_OK = 0,
    APPLIANCE_ACME_MESSAGE_NULL = 1,
    APPLIANCE_ACME_MESSAGE_NOT_OBJECT = 2,
    APPLIANCE_ACME_MESSAGE_MISSING_MEMBER = 3,
    APPLIANCE_ACME_MESSAGE_BAD_URL = 4,
    APPLIANCE_ACME_MESSAGE_BAD_TOKEN = 5,
    APPLIANCE_ACME_MESSAGE_BAD_STATUS = 6,
    APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER = 7,
    APPLIANCE_ACME_MESSAGE_TOO_MANY = 8,
    APPLIANCE_ACME_MESSAGE_NO_HTTP01 = 9,
};

enum appliance_acme_status {
    APPLIANCE_ACME_STATUS_PENDING = 0,
    APPLIANCE_ACME_STATUS_READY = 1,
    APPLIANCE_ACME_STATUS_PROCESSING = 2,
    APPLIANCE_ACME_STATUS_VALID = 3,
    APPLIANCE_ACME_STATUS_INVALID = 4,
};

struct appliance_acme_url {
    char text[APPLIANCE_ACME_URL_MAX + 1U];
    uint32_t length;
};

struct appliance_acme_directory {
    struct appliance_acme_url new_nonce;
    struct appliance_acme_url new_account;
    struct appliance_acme_url new_order;
};

struct appliance_acme_identifier {
    char name[APPLIANCE_ACME_NAME_MAX + 1U];
    uint32_t name_length;
};

struct appliance_acme_order {
    enum appliance_acme_status status;
    struct appliance_acme_url finalize;
    /*
     * ITEM-508 increment 6: a finalized order names where its certificate can
     * be fetched.  The member is absent until the order is valid, so it is
     * optional and its absence is not an error.
     */
    bool has_certificate;
    struct appliance_acme_url certificate;
    uint32_t identifier_count;
    struct appliance_acme_identifier identifiers[APPLIANCE_ACME_IDENTIFIER_MAX];
    uint32_t authorization_count;
    struct appliance_acme_url
        authorizations[APPLIANCE_ACME_AUTHORIZATION_MAX];
};

struct appliance_acme_challenge {
    struct appliance_acme_url url;
    enum appliance_acme_status status;
    char token[APPLIANCE_ACME_TOKEN_MAX + 1U];
    uint32_t token_length;
};

struct appliance_acme_authorization {
    enum appliance_acme_status status;
    struct appliance_acme_identifier identifier;
    uint32_t challenge_count;
    struct appliance_acme_challenge challenges[APPLIANCE_ACME_CHALLENGE_MAX];
    /* The `http-01` challenge, when the authorization carries one. */
    bool has_http01;
    struct appliance_acme_challenge http01;
};

/* `{"status":"valid", ...}` - an account is read for its status alone. */
enum appliance_acme_message_status appliance_acme_parse_account(
    const char *json,
    size_t length,
    enum appliance_acme_status *status_out);

enum appliance_acme_message_status appliance_acme_parse_directory(
    const char *json,
    size_t length,
    struct appliance_acme_directory *directory_out);

enum appliance_acme_message_status appliance_acme_parse_order(
    const char *json,
    size_t length,
    struct appliance_acme_order *order_out);

enum appliance_acme_message_status appliance_acme_parse_authorization(
    const char *json,
    size_t length,
    struct appliance_acme_authorization *authorization_out);

/*
 * One challenge object on its own, as the POST to the challenge URL answers.
 * The http-01 rule is the authorization reader's: a challenge this module
 * reads is one whose url, status and token all validate.
 */
enum appliance_acme_message_status appliance_acme_parse_challenge(
    const char *json,
    size_t length,
    struct appliance_acme_challenge *challenge_out);

const char *appliance_acme_message_status_string(
    enum appliance_acme_message_status status);

#endif
