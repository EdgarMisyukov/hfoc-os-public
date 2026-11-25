/*
 * ITEM-508 increment 3: host proof of the bounded JSON reader and the ACME
 * reply readers.
 *
 * The proof parses fixed replies - a directory with `meta`, an account, an
 * order with two identifiers and two authorizations, an authorization whose
 * `http-01` challenge sits behind a `dns-01` one - and refuses the malformed
 * shapes an ACME client meets in the wild: a missing member, a plain-http URL,
 * an unknown status, a non-DNS identifier, too many identifiers, an
 * authorization without `http-01`, a token that is not base64url, an
 * unterminated string, an escape the reader does not decode and trailing
 * bytes.  No network and no ACME server is involved.
 */

#include "appliance_acme_json.h"
#include "appliance_acme_message.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

static const char json_reader_document[] =
    "{\n  \"keyChange\": \"https://acme.example/key-change\",\n"
    "  \"newAccount\": \"https://acme.example/new-account\",\n"
    "  \"newNonce\": \"https://acme.example/new-nonce\",\n"
    "  \"newOrder\": \"https://acme.example/new-order\",\n"
    "  \"revokeCert\": \"https://acme.example/revoke-cert\",\n"
    "  \"meta\": {\"termsOfService\": \"https://acme.example/terms\"}\n}\n";
static const char directory[] =
    "{\"newNonce\":\"https://acme.example/new-nonce\","
    "\"newAccount\":\"https://acme.example/new-account\","
    "\"newOrder\":\"https://acme.example/new-order\","
    "\"meta\":{\"termsOfService\":\"https://acme.example/terms\"}}";
static const char directory_plain[] =
    "{\"newNonce\":\"http://acme.example/new-nonce\","
    "\"newAccount\":\"https://acme.example/new-account\","
    "\"newOrder\":\"https://acme.example/new-order\"}";
static const char directory_missing[] =
    "{\"newNonce\":\"https://acme.example/new-nonce\","
    "\"newAccount\":\"https://acme.example/new-account\"}";
static const char account[] = "{\"status\":\"valid\",\"contact\":[]}";
static const char account_unknown[] = "{\"status\":\"revoked\"}";
static const char order[] =
    "{\"status\":\"pending\",\"expires\":\"2027-01-15T00:00:00Z\","
    "\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"},"
    "{\"type\":\"dns\",\"value\":\"www.example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\","
    "\"https://acme.example/authz/2\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";
static const char order_bad_type[] =
    "{\"status\":\"pending\","
    "\"identifiers\":[{\"type\":\"ip\",\"value\":\"example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";
static const char order_too_many[] =
    "{\"status\":\"pending\",\"identifiers\":["
    "{\"type\":\"dns\",\"value\":\"a.test\"},"
    "{\"type\":\"dns\",\"value\":\"b.test\"},"
    "{\"type\":\"dns\",\"value\":\"c.test\"},"
    "{\"type\":\"dns\",\"value\":\"d.test\"},"
    "{\"type\":\"dns\",\"value\":\"e.test\"},"
    "{\"type\":\"dns\",\"value\":\"f.test\"},"
    "{\"type\":\"dns\",\"value\":\"g.test\"},"
    "{\"type\":\"dns\",\"value\":\"h.test\"},"
    "{\"type\":\"dns\",\"value\":\"i.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";
static const char authorization[] =
    "{\"status\":\"pending\","
    "\"identifier\":{\"type\":\"dns\",\"value\":\"example.test\"},"
    "\"challenges\":["
    "{\"type\":\"dns-01\",\"url\":\"https://acme.example/chall/2\","
    "\"status\":\"pending\",\"token\":\"other\"},"
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"pending\",\"token\":\"tok3n-ABC_123\"}]}";
static const char authorization_no_http[] =
    "{\"status\":\"pending\","
    "\"identifier\":{\"type\":\"dns\",\"value\":\"example.test\"},"
    "\"challenges\":["
    "{\"type\":\"dns-01\",\"url\":\"https://acme.example/chall/2\","
    "\"status\":\"pending\",\"token\":\"other\"}]}";
static const char authorization_bad_token[] =
    "{\"status\":\"pending\","
    "\"identifier\":{\"type\":\"dns\",\"value\":\"example.test\"},"
    "\"challenges\":["
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"pending\",\"token\":\"bad+token\"}]}";

static void test_json_reader(void)
{
    struct appliance_acme_json_value value;
    struct appliance_acme_json_value nested;
    uint32_t count = 0U;

    CHECK(appliance_acme_json_is_object(
              json_reader_document, sizeof(json_reader_document) - 1U),
          "the reader accepts one object");
    CHECK(appliance_acme_json_member(
              json_reader_document, sizeof(json_reader_document) - 1U,
              "newNonce", &value) &&
              value.kind == APPLIANCE_ACME_JSON_STRING &&
              value.length == 30U &&
              memcmp(value.text, "https://acme.example/new-nonce", 30U) == 0,
          "a string member is handed back as its content");
    CHECK(appliance_acme_json_member(
              json_reader_document, sizeof(json_reader_document) - 1U,
              "meta", &value) &&
              value.kind == APPLIANCE_ACME_JSON_OBJECT,
          "an object member is handed back as an object");
    CHECK(appliance_acme_json_object_member(&value, "termsOfService", &nested),
          "a nested member is readable one step down");
    CHECK(!appliance_acme_json_member(
              json_reader_document, sizeof(json_reader_document) - 1U,
              "revoked", &value),
          "a member the document does not carry refuses");
    CHECK(appliance_acme_json_array_count(
              order, sizeof(order) - 1U, "identifiers", &count) && count == 2U,
          "an array is counted");
    CHECK(appliance_acme_json_array_count(
              authorization, sizeof(authorization) - 1U, "challenges",
              &count) && count == 2U,
          "a nested array is counted");
    CHECK(!appliance_acme_json_is_object("[]", 2U),
          "an array document is not an object");
    CHECK(!appliance_acme_json_is_object("{\"a\":\"b\"} trailing", 19U),
          "trailing bytes refuse");
    CHECK(!appliance_acme_json_member(
              "{\"a\":\"unterminated}", 18U, "a", &value),
          "an unterminated string refuses");
    CHECK(!appliance_acme_json_member("{\"a\":\"\\u0041\"}", 14U, "a", &value),
          "an escape the reader does not decode refuses");
}

static void test_directory(void)
{
    struct appliance_acme_directory parsed;

    CHECK(appliance_acme_parse_directory(
              directory, sizeof(directory) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_OK,
          "the directory is parsed");
    CHECK(parsed.new_nonce.length == 30U &&
              memcmp(parsed.new_nonce.text,
                     "https://acme.example/new-nonce", 30U) == 0 &&
              parsed.new_account.length == 32U &&
              parsed.new_order.length == 30U,
          "every directory URL is read");
    CHECK(appliance_acme_parse_directory(
              directory_plain, sizeof(directory_plain) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_BAD_URL,
          "a plain-http directory URL is refused");
    CHECK(appliance_acme_parse_directory(
              directory_missing, sizeof(directory_missing) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_MISSING_MEMBER,
          "a directory without newOrder is refused");
    CHECK(appliance_acme_parse_directory("[]", 2U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_NOT_OBJECT,
          "a directory that is not an object is refused");
}

static void test_account(void)
{
    enum appliance_acme_status status = APPLIANCE_ACME_STATUS_PENDING;

    CHECK(appliance_acme_parse_account(
              account, sizeof(account) - 1U, &status) ==
              APPLIANCE_ACME_MESSAGE_OK &&
              status == APPLIANCE_ACME_STATUS_VALID,
          "an account's valid status is read");
    CHECK(appliance_acme_parse_account(
              account_unknown, sizeof(account_unknown) - 1U, &status) ==
              APPLIANCE_ACME_MESSAGE_BAD_STATUS,
          "a status RFC 8555 does not define is refused");
}

static void test_order(void)
{
    struct appliance_acme_order parsed;

    CHECK(appliance_acme_parse_order(order, sizeof(order) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_OK,
          "the order is parsed");
    CHECK(parsed.status == APPLIANCE_ACME_STATUS_PENDING &&
              parsed.identifier_count == 2U &&
              parsed.authorization_count == 2U,
          "the order's status and counts are read");
    CHECK(parsed.identifiers[1].name_length == 16U &&
              memcmp(parsed.identifiers[1].name, "www.example.test", 16U) == 0,
          "the second identifier is read");
    CHECK(parsed.authorizations[0].length == 28U &&
              parsed.finalize.length == 31U,
          "the authorization and finalize URLs are read");
    CHECK(appliance_acme_parse_order(
              order_bad_type, sizeof(order_bad_type) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER,
          "an identifier that is not a DNS name is refused");
    CHECK(appliance_acme_parse_order(
              order_too_many, sizeof(order_too_many) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_TOO_MANY,
          "more identifiers than the bound is refused");
}

static void test_authorization(void)
{
    struct appliance_acme_authorization parsed;

    CHECK(appliance_acme_parse_authorization(
              authorization, sizeof(authorization) - 1U, &parsed) ==
              APPLIANCE_ACME_MESSAGE_OK,
          "the authorization is parsed");
    CHECK(parsed.challenge_count == 2U && parsed.has_http01,
          "http-01 is found behind another challenge");
    CHECK(parsed.http01.token_length == 13U &&
              memcmp(parsed.http01.token, "tok3n-ABC_123", 13U) == 0,
          "the http-01 token is read");
    CHECK(parsed.http01.url.length == 28U &&
              parsed.http01.status == APPLIANCE_ACME_STATUS_PENDING &&
              parsed.identifier.name_length == 12U,
          "the http-01 URL, status and identifier are read");
    CHECK(appliance_acme_parse_authorization(
              authorization_no_http, sizeof(authorization_no_http) - 1U,
              &parsed) == APPLIANCE_ACME_MESSAGE_NO_HTTP01,
          "an authorization without http-01 is refused");
    CHECK(appliance_acme_parse_authorization(
              authorization_bad_token, sizeof(authorization_bad_token) - 1U,
              &parsed) == APPLIANCE_ACME_MESSAGE_BAD_TOKEN,
          "a token that is not base64url is refused");
}

int main(void)
{
    test_json_reader();
    test_directory();
    test_account();
    test_order();
    test_authorization();

    if (failures != 0) {
        fprintf(stderr, "appliance acme message host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme message host test: OK\n");
    return 0;
}
