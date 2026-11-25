#include "appliance_acme_message.h"

#include "appliance_acme_json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 3: the reply readers.  Each one walks the object with the
 * bounded JSON reader and fills a fixed structure; a field that is absent, of
 * the wrong kind, or not the thing it claims to be refuses the whole reply, so
 * a caller never acts on half an answer.
 */

static bool appliance_acme_copy_text(
    char *destination,
    size_t capacity,
    const char *source,
    size_t length)
{
    if (destination == NULL || source == NULL || capacity == 0U ||
        length >= capacity) {
        return false;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

static bool appliance_acme_url_absolute(const char *text, size_t length)
{
    static const char scheme[] = "https://";

    if (text == NULL || length <= sizeof(scheme) - 1U ||
        length > APPLIANCE_ACME_URL_MAX ||
        memcmp(text, scheme, sizeof(scheme) - 1U) != 0) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value <= 0x20U || value >= 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool appliance_acme_token_valid(const char *text, size_t length)
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

/*
 * One exact DNS name: the same shape the trust module checks, because the name
 * an order carries is the name the certificate is requested for.
 */
static bool appliance_acme_name_valid(const char *text, size_t length)
{
    size_t label = 0U;
    bool hyphen = false;

    if (text == NULL || length == 0U || length > APPLIANCE_ACME_NAME_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value == (unsigned char)'.') {
            if (label == 0U || hyphen) {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              value == (unsigned char)'-')) {
            return false;
        }
        if (value == (unsigned char)'-' && label == 0U) {
            return false;
        }
        hyphen = value == (unsigned char)'-';
        ++label;
        if (label > 63U) {
            return false;
        }
    }
    return label != 0U && !hyphen;
}

static bool appliance_acme_status_parse(
    const struct appliance_acme_json_value *value,
    enum appliance_acme_status *status_out)
{
    static const struct {
        const char *text;
        enum appliance_acme_status status;
    } names[] = {
        { "pending", APPLIANCE_ACME_STATUS_PENDING },
        { "ready", APPLIANCE_ACME_STATUS_READY },
        { "processing", APPLIANCE_ACME_STATUS_PROCESSING },
        { "valid", APPLIANCE_ACME_STATUS_VALID },
        { "invalid", APPLIANCE_ACME_STATUS_INVALID },
    };

    if (value == NULL || status_out == NULL ||
        value->kind != APPLIANCE_ACME_JSON_STRING) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]);
         ++index) {
        const size_t length = strlen(names[index].text);

        if (value->length == length &&
            memcmp(value->text, names[index].text, length) == 0) {
            *status_out = names[index].status;
            return true;
        }
    }
    return false;
}

static bool appliance_acme_url_read(
    const struct appliance_acme_json_value *value,
    struct appliance_acme_url *url_out)
{
    if (value == NULL || url_out == NULL ||
        value->kind != APPLIANCE_ACME_JSON_STRING ||
        !appliance_acme_url_absolute(value->text, value->length)) {
        return false;
    }
    memset(url_out, 0, sizeof(*url_out));
    if (!appliance_acme_copy_text(
            url_out->text, sizeof(url_out->text), value->text,
            value->length)) {
        return false;
    }
    url_out->length = (uint32_t)value->length;
    return true;
}

static bool appliance_acme_member_status(
    const char *json,
    size_t length,
    enum appliance_acme_status *status_out)
{
    struct appliance_acme_json_value value;

    return appliance_acme_json_member(json, length, "status", &value) &&
           appliance_acme_status_parse(&value, status_out);
}

static enum appliance_acme_message_status appliance_acme_directory_read(
    const char *json,
    size_t length,
    struct appliance_acme_directory *directory)
{
    static const char *const names[] = { "newNonce", "newAccount", "newOrder" };
    struct appliance_acme_url *urls[] = {
        &directory->new_nonce, &directory->new_account, &directory->new_order
    };

    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]);
         ++index) {
        struct appliance_acme_json_value value;

        if (!appliance_acme_json_member(json, length, names[index], &value)) {
            return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
        }
        if (!appliance_acme_url_read(&value, urls[index])) {
            return APPLIANCE_ACME_MESSAGE_BAD_URL;
        }
    }
    return APPLIANCE_ACME_MESSAGE_OK;
}

enum appliance_acme_message_status appliance_acme_parse_directory(
    const char *json,
    size_t length,
    struct appliance_acme_directory *directory_out)
{
    if (json == NULL || directory_out == NULL) {
        return APPLIANCE_ACME_MESSAGE_NULL;
    }
    if (!appliance_acme_json_is_object(json, length)) {
        return APPLIANCE_ACME_MESSAGE_NOT_OBJECT;
    }
    memset(directory_out, 0, sizeof(*directory_out));
    return appliance_acme_directory_read(json, length, directory_out);
}

enum appliance_acme_message_status appliance_acme_parse_account(
    const char *json,
    size_t length,
    enum appliance_acme_status *status_out)
{
    if (json == NULL || status_out == NULL) {
        return APPLIANCE_ACME_MESSAGE_NULL;
    }
    if (!appliance_acme_json_is_object(json, length)) {
        return APPLIANCE_ACME_MESSAGE_NOT_OBJECT;
    }
    if (!appliance_acme_member_status(json, length, status_out)) {
        return APPLIANCE_ACME_MESSAGE_BAD_STATUS;
    }
    return APPLIANCE_ACME_MESSAGE_OK;
}

static enum appliance_acme_message_status appliance_acme_identifiers_read(
    const char *json,
    size_t length,
    struct appliance_acme_order *order)
{
    uint32_t count = 0U;

    if (!appliance_acme_json_array_count(
            json, length, "identifiers", &count)) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    if (count > APPLIANCE_ACME_IDENTIFIER_MAX) {
        return APPLIANCE_ACME_MESSAGE_TOO_MANY;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        struct appliance_acme_json_value element;
        struct appliance_acme_json_value type;
        struct appliance_acme_json_value value;
        struct appliance_acme_identifier *identifier =
            &order->identifiers[index];

        if (!appliance_acme_json_array_element(
                json, length, "identifiers", index, &element) ||
            element.kind != APPLIANCE_ACME_JSON_OBJECT ||
            !appliance_acme_json_object_member(&element, "type", &type) ||
            !appliance_acme_json_object_member(&element, "value", &value) ||
            type.kind != APPLIANCE_ACME_JSON_STRING ||
            type.length != 3U || memcmp(type.text, "dns", 3U) != 0 ||
            value.kind != APPLIANCE_ACME_JSON_STRING ||
            !appliance_acme_name_valid(value.text, value.length) ||
            !appliance_acme_copy_text(
                identifier->name, sizeof(identifier->name), value.text,
                value.length)) {
            return APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER;
        }
        identifier->name_length = (uint32_t)value.length;
        order->identifier_count = index + 1U;
    }
    return APPLIANCE_ACME_MESSAGE_OK;
}

static enum appliance_acme_message_status appliance_acme_authorizations_read(
    const char *json,
    size_t length,
    struct appliance_acme_order *order)
{
    uint32_t count = 0U;

    if (!appliance_acme_json_array_count(
            json, length, "authorizations", &count)) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    if (count > APPLIANCE_ACME_AUTHORIZATION_MAX) {
        return APPLIANCE_ACME_MESSAGE_TOO_MANY;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        struct appliance_acme_json_value element;

        if (!appliance_acme_json_array_element(
                json, length, "authorizations", index, &element) ||
            !appliance_acme_url_read(
                &element, &order->authorizations[index])) {
            return APPLIANCE_ACME_MESSAGE_BAD_URL;
        }
        order->authorization_count = index + 1U;
    }
    return APPLIANCE_ACME_MESSAGE_OK;
}

enum appliance_acme_message_status appliance_acme_parse_order(
    const char *json,
    size_t length,
    struct appliance_acme_order *order_out)
{
    enum appliance_acme_message_status status;
    struct appliance_acme_json_value finalize;

    if (json == NULL || order_out == NULL) {
        return APPLIANCE_ACME_MESSAGE_NULL;
    }
    if (!appliance_acme_json_is_object(json, length)) {
        return APPLIANCE_ACME_MESSAGE_NOT_OBJECT;
    }
    memset(order_out, 0, sizeof(*order_out));
    if (!appliance_acme_member_status(json, length, &order_out->status)) {
        return APPLIANCE_ACME_MESSAGE_BAD_STATUS;
    }
    if (!appliance_acme_json_member(json, length, "finalize", &finalize)) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    if (!appliance_acme_url_read(&finalize, &order_out->finalize)) {
        return APPLIANCE_ACME_MESSAGE_BAD_URL;
    }
    {
        struct appliance_acme_json_value certificate;

        if (appliance_acme_json_member(
                json, length, "certificate", &certificate)) {
            if (!appliance_acme_url_read(
                    &certificate, &order_out->certificate)) {
                return APPLIANCE_ACME_MESSAGE_BAD_URL;
            }
            order_out->has_certificate = true;
        }
    }
    status = appliance_acme_identifiers_read(json, length, order_out);
    if (status != APPLIANCE_ACME_MESSAGE_OK) {
        return status;
    }
    return appliance_acme_authorizations_read(json, length, order_out);
}

static enum appliance_acme_message_status
appliance_acme_challenge_read_value(
    const char *json,
    size_t length,
    struct appliance_acme_challenge *challenge);

enum appliance_acme_message_status appliance_acme_parse_challenge(
    const char *json,
    size_t length,
    struct appliance_acme_challenge *challenge_out)
{
    if (json == NULL || challenge_out == NULL) {
        return APPLIANCE_ACME_MESSAGE_NULL;
    }
    if (!appliance_acme_json_is_object(json, length)) {
        return APPLIANCE_ACME_MESSAGE_NOT_OBJECT;
    }
    memset(challenge_out, 0, sizeof(*challenge_out));
    return appliance_acme_challenge_read_value(json, length, challenge_out);
}

static enum appliance_acme_message_status appliance_acme_challenge_read_value(
    const char *json,
    size_t length,
    struct appliance_acme_challenge *challenge)
{
    struct appliance_acme_json_value url;
    struct appliance_acme_json_value status;
    struct appliance_acme_json_value token;

    if (challenge == NULL ||
        !appliance_acme_json_member(json, length, "url", &url) ||
        !appliance_acme_json_member(json, length, "status", &status) ||
        !appliance_acme_json_member(json, length, "token", &token)) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    if (!appliance_acme_url_read(&url, &challenge->url)) {
        return APPLIANCE_ACME_MESSAGE_BAD_URL;
    }
    if (!appliance_acme_status_parse(&status, &challenge->status)) {
        return APPLIANCE_ACME_MESSAGE_BAD_STATUS;
    }
    if (token.kind != APPLIANCE_ACME_JSON_STRING ||
        !appliance_acme_token_valid(token.text, token.length) ||
        !appliance_acme_copy_text(
            challenge->token, sizeof(challenge->token), token.text,
            token.length)) {
        return APPLIANCE_ACME_MESSAGE_BAD_TOKEN;
    }
    challenge->token_length = (uint32_t)token.length;
    return APPLIANCE_ACME_MESSAGE_OK;
}

static enum appliance_acme_message_status appliance_acme_challenge_read(
    const struct appliance_acme_json_value *element,
    struct appliance_acme_challenge *challenge)
{
    if (element == NULL || element->kind != APPLIANCE_ACME_JSON_OBJECT) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    return appliance_acme_challenge_read_value(
        element->text, element->length, challenge);
}

enum appliance_acme_message_status appliance_acme_parse_authorization(
    const char *json,
    size_t length,
    struct appliance_acme_authorization *authorization_out)
{
    struct appliance_acme_json_value identifier_object;
    struct appliance_acme_json_value identifier_value;
    struct appliance_acme_json_value challenges;
    uint32_t count = 0U;

    if (json == NULL || authorization_out == NULL) {
        return APPLIANCE_ACME_MESSAGE_NULL;
    }
    if (!appliance_acme_json_is_object(json, length)) {
        return APPLIANCE_ACME_MESSAGE_NOT_OBJECT;
    }
    memset(authorization_out, 0, sizeof(*authorization_out));
    if (!appliance_acme_member_status(json, length, &authorization_out->status)) {
        return APPLIANCE_ACME_MESSAGE_BAD_STATUS;
    }
    if (!appliance_acme_json_member(json, length, "identifier", &identifier_object) ||
        identifier_object.kind != APPLIANCE_ACME_JSON_OBJECT ||
        !appliance_acme_json_object_member(
            &identifier_object, "value", &identifier_value) ||
        identifier_value.kind != APPLIANCE_ACME_JSON_STRING ||
        !appliance_acme_name_valid(
            identifier_value.text, identifier_value.length) ||
        !appliance_acme_copy_text(
            authorization_out->identifier.name,
            sizeof(authorization_out->identifier.name),
            identifier_value.text, identifier_value.length)) {
        return APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER;
    }
    authorization_out->identifier.name_length =
        (uint32_t)identifier_value.length;
    if (!appliance_acme_json_member(json, length, "challenges", &challenges) ||
        challenges.kind != APPLIANCE_ACME_JSON_ARRAY ||
        !appliance_acme_json_array_count(
            json, length, "challenges", &count)) {
        return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
    }
    if (count > APPLIANCE_ACME_CHALLENGE_MAX) {
        return APPLIANCE_ACME_MESSAGE_TOO_MANY;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        struct appliance_acme_json_value element;
        struct appliance_acme_json_value type;
        struct appliance_acme_challenge *challenge =
            &authorization_out->challenges[index];
        enum appliance_acme_message_status status;

        if (!appliance_acme_json_array_element(
                json, length, "challenges", index, &element) ||
            element.kind != APPLIANCE_ACME_JSON_OBJECT) {
            return APPLIANCE_ACME_MESSAGE_MISSING_MEMBER;
        }
        status = appliance_acme_challenge_read(&element, challenge);
        if (status != APPLIANCE_ACME_MESSAGE_OK) {
            return status;
        }
        authorization_out->challenge_count = index + 1U;
        if (appliance_acme_json_object_member(&element, "type", &type) &&
            type.kind == APPLIANCE_ACME_JSON_STRING &&
            type.length == 7U && memcmp(type.text, "http-01", 7U) == 0) {
            authorization_out->has_http01 = true;
            authorization_out->http01 = *challenge;
        }
    }
    if (!authorization_out->has_http01) {
        return APPLIANCE_ACME_MESSAGE_NO_HTTP01;
    }
    return APPLIANCE_ACME_MESSAGE_OK;
}

const char *appliance_acme_message_status_string(
    enum appliance_acme_message_status status)
{
    switch (status) {
    case APPLIANCE_ACME_MESSAGE_OK:
        return "OK";
    case APPLIANCE_ACME_MESSAGE_NULL:
        return "NULL";
    case APPLIANCE_ACME_MESSAGE_NOT_OBJECT:
        return "NOT_OBJECT";
    case APPLIANCE_ACME_MESSAGE_MISSING_MEMBER:
        return "MISSING_MEMBER";
    case APPLIANCE_ACME_MESSAGE_BAD_URL:
        return "BAD_URL";
    case APPLIANCE_ACME_MESSAGE_BAD_TOKEN:
        return "BAD_TOKEN";
    case APPLIANCE_ACME_MESSAGE_BAD_STATUS:
        return "BAD_STATUS";
    case APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER:
        return "BAD_IDENTIFIER";
    case APPLIANCE_ACME_MESSAGE_TOO_MANY:
        return "TOO_MANY";
    case APPLIANCE_ACME_MESSAGE_NO_HTTP01:
        return "NO_HTTP01";
    default:
        break;
    }
    return "UNKNOWN";
}
