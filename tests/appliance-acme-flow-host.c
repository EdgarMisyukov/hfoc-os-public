/*
 * ITEM-508 increment 5: host proof of the ACME order's first steps.
 *
 * The proof drives the whole spine with fixed data and no network: an
 * unvalidated clock refuses before anything is read, then a directory, an
 * account, an order and an authorization move the flow through its states,
 * and every request it produces is a real JWS whose signature is verified
 * over `protected.payload` with the shared ITEM-503 fixture key.  The HTTP-01
 * key authorization is compared against the token and the independently
 * computed thumbprint, and the records the flow publishes never carry a
 * nonce or a token.
 */

#include "appliance_acme_flow.h"
#include "appliance_production_acme_http01.h"

#include "appliance-certificate-fixtures.h"
#include "hfoc_mbedtls_alloc.h"

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

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

/*
 * ITEM-508 increment 30: the listener half of this proof links the lwIP TCP
 * objects, which need the two clocks the kernel supplies; both are fixed
 * ticks here, exactly as the increment-28 listener proof stubs them.
 */
u32_t sys_now(void)
{
    return 1000U;
}

uint32_t hfoc_lwip_time_now(void)
{
    return 1000U;
}

static void make_live_netif(struct hfoc_lwip_netif *netif)
{
    memset(netif, 0, sizeof(*netif));
    netif->published = true;
    netif->live = true;
    netif->version = HFOC_LWIP_NETIF_VERSION;
    netif->generation = 1U;
    netif->netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif->netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif->netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    netif->netif.hwaddr[0] = 0x52U;
    netif->netif.hwaddr[1] = 0x54U;
    netif->netif.hwaddr[2] = 0x00U;
    netif->netif.hwaddr[3] = 0x12U;
    netif->netif.hwaddr[4] = 0x34U;
    netif->netif.hwaddr[5] = 0x56U;
    netif->netif.name[0] = 'e';
    netif->netif.name[1] = 't';
    netif->netif.num = 0U;
    IP4_ADDR(&netif->netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif->netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif->netif.gw, 10U, 0U, 2U, 2U);
}

static void make_config(struct network_ipv4_config *config)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config->address.octets[0] = 10U;
    config->address.octets[1] = 0U;
    config->address.octets[2] = 2U;
    config->address.octets[3] = 15U;
    config->prefix_length = 24U;
    config->gateway.octets[0] = 10U;
    config->gateway.octets[1] = 0U;
    config->gateway.octets[2] = 2U;
    config->gateway.octets[3] = 2U;
    config->dns.octets[0] = 10U;
    config->dns.octets[1] = 0U;
    config->dns.octets[2] = 2U;
    config->dns.octets[3] = 3U;
}

static const char directory_json[] =
    "{\"newNonce\":\"https://acme.example/new-nonce\","
    "\"newAccount\":\"https://acme.example/new-account\","
    "\"newOrder\":\"https://acme.example/new-order\"}";
static const char account_json[] = "{\"status\":\"valid\",\"contact\":[]}";
static const char order_json[] =
    "{\"status\":\"pending\","
    "\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";
static const char authorization_json[] =
    "{\"status\":\"pending\","
    "\"identifier\":{\"type\":\"dns\",\"value\":\"example.test\"},"
    "\"challenges\":["
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"pending\",\"token\":\"tok3n-ABC_123\"}]}";
static const char order_bad_json[] =
    "{\"status\":\"revoked\","
    "\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";

static const char challenge_pending_json[] =
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"processing\",\"token\":\"tok3n-ABC_123\"}";
static const char challenge_valid_json[] =
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"valid\",\"token\":\"tok3n-ABC_123\"}";
static const char challenge_invalid_json[] =
    "{\"type\":\"http-01\",\"url\":\"https://acme.example/chall/1\","
    "\"status\":\"invalid\",\"token\":\"tok3n-ABC_123\"}";
static const char finalized_order_json[] =
    "{\"status\":\"valid\","
    "\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\","
    "\"certificate\":\"https://acme.example/cert/1\"}";
static const char fixture_chain_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBjjCCATWgAwIBAgIUTsMSXC7gRDPm/QMAzkHeLoJ4NeQwCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMaGZvYy10ZXN0LWNhMB4XDTI2MTAwMTA0MTUwOVoXDTM2MDky\n"
    "ODA0MTUwOVowGTEXMBUGA1UEAwwOaGZvYy1hcHBsaWFuY2UwWTATBgcqhkjOPQIB\n"
    "BggqhkjOPQMBBwNCAARlPtFAVWlzbQexGBv0eDsdUbzF2ZtyiW0n91p6tHfZDR8x\n"
    "uITcv8A6obiUm9oW3FToWlKhM/ojs/uW9j831aFzo10wWzAZBgNVHREEEjAQgg5o\n"
    "Zm9jLWFwcGxpYW5jZTAdBgNVHQ4EFgQUDsMOk6u1fIbDPkUJJPN0G5z/UI8wHwYD\n"
    "VR0jBBgwFoAUAdBI8qlg91ywN1B1Km7WZRo663EwCgYIKoZIzj0EAwIDRwAwRAIg\n"
    "QfuOs8xzRbeasYZ3Mp8xnJTBPUFYwiZXJCdobcvCcgoCIBa9FEe5sXNgjyfgHyHj\n"
    "+Iu2F4ZcQgYJkeefOp0YNl4r\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBkzCCATmgAwIBAgIUH4xzqxqbxqbm4Tn57H5GFrdsp4owCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMaGZvYy10ZXN0LWNhMB4XDTI2MTAwMTA0MTUwOVoXDTM2MDky\n"
    "ODA0MTUwOVowFzEVMBMGA1UEAwwMaGZvYy10ZXN0LWNhMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAETpz13bcJaE+0y2IZViIxmcjH7YD5Ezb2F3BSeteM3XUX5ibs\n"
    "ZbG/bFiqevh32NStC/hkLv7bnyx+jAKQ92h246NjMGEwHQYDVR0OBBYEFAHQSPKp\n"
    "YPdcsDdQdSpu1mUaOutxMB8GA1UdIwQYMBaAFAHQSPKpYPdcsDdQdSpu1mUaOutx\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoGCCqGSM49BAMCA0gA\n"
    "MEUCIQCZ1YUiZ3w095Nwiy24G+mmdc/qYAddwg1/6LFV3kObLwIgF599oXSrTF0Y\n"
    "2J2r4sZiN/x7m23T1dmSWEc1D6IyClU=\n"
    "-----END CERTIFICATE-----\n"
    ;
/* length = 1206 */

static const char finalized_order_without_certificate_json[] =
    "{\"status\":\"valid\","
    "\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"}],"
    "\"authorizations\":[\"https://acme.example/authz/1\"],"
    "\"finalize\":\"https://acme.example/finalize/1\"}";

/* The Location of the newOrder reply; the flow polls it at finalize time. */
static const char order_url[] = "https://acme.example/order/1";

/* Rebuild the DER mbedTLS verifies from the raw R || S the JWS carries. */
static size_t raw_signature_to_der(
    const unsigned char raw[64U],
    unsigned char der[80U])
{
    unsigned char body[72U];
    size_t body_length = 0U;

    for (size_t half = 0U; half < 2U; ++half) {
        const unsigned char *value = &raw[half * 32U];
        size_t length = 32U;
        size_t offset = 0U;

        while (length > 1U && value[offset] == 0x00U) {
            offset += 1U;
            length -= 1U;
        }
        body[body_length++] = 0x02U;
        if ((value[offset] & 0x80U) != 0U) {
            body[body_length++] = (unsigned char)(length + 1U);
            body[body_length++] = 0x00U;
        } else {
            body[body_length++] = (unsigned char)length;
        }
        memcpy(&body[body_length], &value[offset], length);
        body_length += length;
    }
    der[0] = 0x30U;
    der[1] = (unsigned char)body_length;
    memcpy(&der[2], body, body_length);
    return body_length + 2U;
}

/*
 * One produced request must be a flattened JWS whose signature verifies over
 * `protected.payload` with the fixture key, and its protected header must
 * carry the expected member (`jwk` or `kid`) and URL.
 */
static void check_request(
    const struct appliance_acme_flow *flow,
    const char *expected_url,
    const char *expected_member)
{
    char protected_b64[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    char payload_b64[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    char signature_b64[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    unsigned char signature[APPLIANCE_ACME_SIGNATURE_BYTES];
    unsigned char signing_input[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    unsigned char digest[32U];
    unsigned char decoded[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    size_t protected_length = 0U;
    size_t payload_length = 0U;
    size_t signature_length = 0U;
    size_t decoded_length = 0U;
    size_t signing_length = 0U;
    const char *cursor = flow->request;
    const char *end = flow->request + flow->request_length;
    bool saw_url = false;
    bool saw_member = false;

    CHECK(flow->request_length != 0U &&
              strncmp(flow->request, "{\"protected\":\"", 14U) == 0,
          "the request is a flattened JWS");
    /* protected */
    cursor = strchr(cursor, ':');
    cursor = cursor == NULL ? NULL : cursor + 2;
    {
        const char *closing = cursor == NULL ? NULL : strchr(cursor, '"');

        CHECK(cursor != NULL && closing != NULL, "the protected member is present");
        if (cursor == NULL || closing == NULL) {
            return;
        }
        protected_length = (size_t)(closing - cursor);
        memcpy(protected_b64, cursor, protected_length);
        protected_b64[protected_length] = '\0';
        cursor = closing + 1;
    }
    /* payload */
    cursor = strstr(cursor, ",\"payload\":\"");
    cursor = cursor == NULL ? NULL : cursor + strlen(",\"payload\":\"");
    {
        const char *closing = cursor == NULL ? NULL : strchr(cursor, '"');

        CHECK(cursor != NULL && closing != NULL, "the payload member is present");
        if (cursor == NULL || closing == NULL) {
            return;
        }
        payload_length = (size_t)(closing - cursor);
        memcpy(payload_b64, cursor, payload_length);
        payload_b64[payload_length] = '\0';
        cursor = closing + 1;
    }
    /* signature */
    cursor = strstr(cursor, ",\"signature\":\"");
    cursor = cursor == NULL ? NULL : cursor + strlen(",\"signature\":\"");
    {
        const char *closing = cursor == NULL ? NULL : strchr(cursor, '"');

        CHECK(cursor != NULL && closing != NULL &&
                  closing < end,
              "the signature member is present");
        if (cursor == NULL || closing == NULL || closing >= end) {
            return;
        }
        signature_length = (size_t)(closing - cursor);
        memcpy(signature_b64, cursor, signature_length);
        signature_b64[signature_length] = '\0';
    }
    CHECK(appliance_acme_base64url_decode(
              protected_b64, protected_length, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_OK,
          "the protected header decodes");
    decoded[decoded_length] = '\0';
    saw_url = strstr((const char *)decoded, expected_url) != NULL;
    saw_member = strstr((const char *)decoded, expected_member) != NULL;
    CHECK(saw_url, "the protected header names the request URL");
    CHECK(saw_member, "the protected header carries the expected key member");

    memcpy(signing_input, protected_b64, protected_length);
    signing_input[protected_length] = (unsigned char)'.';
    memcpy(&signing_input[protected_length + 1U], payload_b64,
           payload_length);
    signing_length = protected_length + 1U + payload_length;
    CHECK(mbedtls_sha256(signing_input, signing_length, digest, 0) == 0,
          "the signing input hashes");
    CHECK(appliance_acme_base64url_decode(
              signature_b64, signature_length, signature,
              sizeof(signature),
              &decoded_length) == APPLIANCE_ACME_BASE64_OK &&
              decoded_length == APPLIANCE_ACME_SIGNATURE_BYTES,
          "the signature is the raw 64-byte R || S");
    {
        mbedtls_pk_context context;
        unsigned char der_signature[80U];
        const size_t der_length =
            raw_signature_to_der(signature, der_signature);

        mbedtls_pk_init(&context);
        CHECK(mbedtls_pk_parse_key(
                  &context, fixture_chained_key,
                  sizeof(fixture_chained_key), NULL, 0U, fixture_rng,
                  NULL) == 0,
              "the fixture key parses for verification");
        CHECK(mbedtls_pk_verify(
                  &context, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                  der_signature, der_length) == 0,
              "the request signature verifies over protected.payload");
        mbedtls_pk_free(&context);
    }
}

static void test_unvalidated_clock_starts_nothing(void)
{
    struct appliance_acme_flow flow;

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U, 0U) ==
              APPLIANCE_ACME_FLOW_BAD_CLOCK,
          "an unvalidated clock refuses the order");
    CHECK(flow.state == APPLIANCE_ACME_FLOW_STATE_AWAITING_CLOCK &&
              flow.request_length == 0U,
          "no request is built on an unvalidated clock");
    CHECK(strstr(flow.record, "[ACME-FLOW] state=AWAITING_CLOCK") != NULL &&
              strstr(flow.record, "status=BAD_CLOCK") != NULL,
          "the refusal is published as a record");
}

static void test_the_order_spine(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    static const char *const names[] = { "example.test" };
    static const size_t name_lengths[] = { 12U };
    char authorization[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    size_t authorization_length = 0U;

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_READY,
          "a validated clock reads the directory and opens the flow");
    CHECK(appliance_acme_flow_declare_identifiers(
              &flow, names, name_lengths, 1U) == APPLIANCE_ACME_FLOW_OK,
          "the order's identifiers are declared");

    CHECK(appliance_acme_key_import(
              &key, fixture_chained_key,
              sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK,
          "the fixture account key binds");
    CHECK(appliance_acme_flow_begin_account(
              &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING,
          "the newAccount request is built");
    check_request(&flow, "https://acme.example/new-account", "\"jwk\":");
    CHECK(strstr(flow.record, "step=ACCOUNT") != NULL &&
              strstr(flow.record, "state=ACCOUNT_PENDING") != NULL,
          "the account step is recorded");
    CHECK(strstr(flow.record, "nonce-1") == NULL,
          "the record never carries the nonce");

    CHECK(appliance_acme_flow_account_result(
              &flow, account_json, sizeof(account_json) - 1U,
              "https://acme.example/acct/1", 27U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.account_url.length == 27U,
          "the account reply and its Location URL are accepted");
    CHECK(appliance_acme_flow_begin_order(
              &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING,
          "the newOrder request is built");
    check_request(&flow, "https://acme.example/new-order", "\"kid\":");
    CHECK(strstr(flow.request, "example.test") == NULL,
          "the request carries the identifier in the payload, not the header");

    CHECK(appliance_acme_flow_order_result(
              &flow, order_json, sizeof(order_json) - 1U,
              order_url, sizeof(order_url) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.order.authorization_count == 1U,
          "the order reply is accepted");
    CHECK(appliance_acme_flow_begin_authorization(
              &flow, &key, "nonce-3", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK,
          "the authorization request is built as POST-as-GET");
    check_request(&flow, "https://acme.example/authz/1", "\"kid\":");
    {
        const char *payload = strstr(flow.request, "\",\"payload\":\"");

        CHECK(payload != NULL &&
                  strncmp(payload + strlen("\",\"payload\":\""), "\"", 1U) ==
                      0,
              "POST-as-GET carries the empty payload");
    }
    CHECK(appliance_acme_flow_authorization_result(
              &flow, authorization_json,
              sizeof(authorization_json) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.authorization.has_http01,
          "the authorization reply carries the http-01 challenge");
    CHECK(appliance_acme_flow_key_authorization(
              &flow, &key, authorization, sizeof(authorization),
              &authorization_length) == APPLIANCE_ACME_FLOW_OK &&
              strcmp(authorization,
                     "tok3n-ABC_123."
                     "3ysHIvd-KKLESSZogF4Qw7dQqZjtIvjQ36NVdQDqe_o") == 0,
          "the key authorization is the token and the thumbprint");
    CHECK(strstr(flow.record, "tok3n") == NULL,
          "the record never carries the challenge token");
}

/*
 * ITEM-508 increment 30: the order path arms the listener.  The listener is
 * armed from the flow's own parsed challenge - the token accessor and the key
 * authorization both read the same authorization object - and the decision
 * module then serves exactly that key authorization on exactly that path.  No
 * caller literal takes part.
 */
static void test_the_listener_arms_from_the_flow(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_flow without_challenge;
    struct appliance_acme_key key;
    struct appliance_production_acme_http01 http01;
    struct appliance_production_acme_http01 uninitialized;
    struct hfoc_lwip_netif netif;
    struct network_ipv4_config config;
    static const char *const names[] = { "example.test" };
    static const size_t name_lengths[] = { 12U };
    static const char expected_path[] =
        "GET /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.1\r\n";
    static const char foreign_path[] =
        "GET /.well-known/acme-challenge/other-token HTTP/1.1\r\n";
    static const char expected_answer[] =
        "tok3n-ABC_123.3ysHIvd-KKLESSZogF4Qw7dQqZjtIvjQ36NVdQDqe_o";
    char token[APPLIANCE_ACME_TOKEN_MAX + 1U];
    char small[4U];
    size_t token_length = 0U;
    size_t small_length = 0U;
    const char *body = NULL;
    size_t body_length = 0U;

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_declare_identifiers(
                  &flow, names, name_lengths, 1U) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_key_import(
                  &key, fixture_chained_key,
                  sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK &&
              appliance_acme_flow_begin_account(
                  &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_account_result(
                  &flow, account_json, sizeof(account_json) - 1U,
                  "https://acme.example/acct/1", 27U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_order(
                  &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_order_result(
                  &flow, order_json, sizeof(order_json) - 1U, order_url,
                  sizeof(order_url) - 1U) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_authorization(
                  &flow, &key, "nonce-3", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_authorization_result(
                  &flow, authorization_json,
                  sizeof(authorization_json) - 1U) == APPLIANCE_ACME_FLOW_OK,
          "the flow reaches the parsed http-01 authorization");

    CHECK(appliance_acme_flow_http01_token(
              &flow, token, sizeof(token), &token_length) ==
              APPLIANCE_ACME_FLOW_OK &&
              token_length == 13U && strcmp(token, "tok3n-ABC_123") == 0,
          "the token accessor returns exactly the parsed challenge token");
    memset(small, 0, sizeof(small));
    CHECK(appliance_acme_flow_http01_token(
              &flow, small, sizeof(small), &small_length) ==
              APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL &&
              small_length == 0U && small[0] == '\0',
          "a token that does not fit is refused without a partial copy");
    CHECK(appliance_acme_flow_http01_token(
              NULL, token, sizeof(token), &token_length) ==
              APPLIANCE_ACME_FLOW_NULL &&
              appliance_acme_flow_http01_token(
                  &flow, NULL, 0U, &token_length) ==
                  APPLIANCE_ACME_FLOW_NULL &&
              appliance_acme_flow_http01_token(
                  &flow, token, sizeof(token), NULL) ==
                  APPLIANCE_ACME_FLOW_NULL,
          "the token accessor refuses null arguments");
    memset(&without_challenge, 0, sizeof(without_challenge));
    without_challenge.initialized = true;
    CHECK(appliance_acme_flow_http01_token(
              &without_challenge, token, sizeof(token), &token_length) ==
              APPLIANCE_ACME_FLOW_BAD_STATE,
          "a flow without the http-01 challenge has no token");

    make_live_netif(&netif);
    make_config(&config);
    CHECK(appliance_production_acme_http01_init(
              &http01, &netif, &config) ==
              APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING &&
              !appliance_production_acme_http01_armed(&http01),
          "the listener comes up listening and unarmed");
    CHECK(appliance_production_acme_http01_arm_flow(
              &http01, &flow, &key, FIXTURE_NOW) ==
              APPLIANCE_ACME_CHALLENGE_OK &&
              appliance_production_acme_http01_armed(&http01),
          "the order path arms the listener from the flow");
    CHECK(appliance_acme_challenge_decide_line(
              &http01.challenge, expected_path, sizeof(expected_path) - 1U,
              FIXTURE_NOW, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_OK &&
              body != NULL && body_length == sizeof(expected_answer) - 1U &&
              strcmp(body, expected_answer) == 0,
          "the listener serves exactly the flow's key authorization");
    CHECK(appliance_acme_challenge_decide_line(
              &http01.challenge, foreign_path, sizeof(foreign_path) - 1U,
              FIXTURE_NOW, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_NOT_FOUND &&
              body == NULL && body_length == 0U,
          "a foreign token is refused with an empty body");

    memset(&uninitialized, 0, sizeof(uninitialized));
    CHECK(appliance_production_acme_http01_arm_flow(
              NULL, &flow, &key, FIXTURE_NOW) ==
              APPLIANCE_ACME_CHALLENGE_NULL &&
              appliance_production_acme_http01_arm_flow(
                  &http01, NULL, &key, FIXTURE_NOW) ==
                  APPLIANCE_ACME_CHALLENGE_NULL &&
              appliance_production_acme_http01_arm_flow(
                  &http01, &flow, NULL, FIXTURE_NOW) ==
                  APPLIANCE_ACME_CHALLENGE_NULL,
          "arm_flow refuses null arguments");
    CHECK(appliance_production_acme_http01_arm_flow(
              &http01, &without_challenge, &key, FIXTURE_NOW) ==
              APPLIANCE_ACME_CHALLENGE_BAD_STATE &&
              appliance_production_acme_http01_arm_flow(
                  &uninitialized, &flow, &key, FIXTURE_NOW) ==
                  APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "a flow without a challenge and a listener that is not up refuse");
    CHECK(appliance_acme_challenge_decide_line(
              &http01.challenge, expected_path, sizeof(expected_path) - 1U,
              FIXTURE_NOW, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_OK,
          "a refused arm leaves the armed answer untouched");

    appliance_production_acme_http01_disarm(&http01);
    CHECK(appliance_acme_challenge_decide_line(
              &http01.challenge, expected_path, sizeof(expected_path) - 1U,
              FIXTURE_NOW, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE,
          "disarm closes the challenge for the same path");
}

static void test_bad_inputs_refuse(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    static const char *const names[] = { "example.test" };
    static const size_t name_lengths[] = { 12U };

    CHECK(appliance_acme_flow_begin(
              &flow, "[]", 2U, FIXTURE_NOW) ==
              APPLIANCE_ACME_FLOW_BAD_REPLY &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a directory that is not an object fails the flow");
    CHECK(strstr(flow.record, "status=BAD_REPLY") != NULL,
          "the failure is published as a record");

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK,
          "the flow restarts after a failure");
    CHECK(appliance_acme_key_import(
              &key, fixture_chained_key,
              sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK,
          "the fixture account key binds again");
    CHECK(appliance_acme_flow_declare_identifiers(
              &flow, names, name_lengths, 1U) == APPLIANCE_ACME_FLOW_OK,
          "the identifiers are declared again");
    CHECK(appliance_acme_flow_begin_account(
              &flow, &key, "bad+nonce", 9U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_BAD_REPLY &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a nonce that is not base64url refuses the request");

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK,
          "the flow restarts once more");
    CHECK(appliance_acme_flow_begin_account(
              &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK,
          "the account request is built again");
    CHECK(appliance_acme_flow_account_result(
              &flow, "{\"status\":\"deactivated\"}",
              strlen("{\"status\":\"deactivated\"}"),
              "https://acme.example/acct/1", 27U) ==
              APPLIANCE_ACME_FLOW_BAD_STATUS &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "an account status that is not valid refuses the flow");

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_declare_identifiers(
                  &flow, names, name_lengths, 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_account(
                  &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_account_result(
                  &flow, account_json, sizeof(account_json) - 1U,
                  "https://acme.example/acct/1", 27U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_order(
                  &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK,
          "the flow reaches the order step");
    CHECK(appliance_acme_flow_order_result(
              &flow, order_bad_json, sizeof(order_bad_json) - 1U,
              order_url, sizeof(order_url) - 1U) ==
              APPLIANCE_ACME_FLOW_BAD_REPLY &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "an order status RFC 8555 does not define refuses the flow");
}


/*
 * ITEM-508 increment 22: the finalize carries a PKCS#10, so the proof writes
 * one from the same fixture key every request uses.  The flow treats it as
 * bytes and never parses it; the CSR's own shape is proven by the JWS host
 * test, so this buffer is only the request's payload input.
 */
static const unsigned char *fixture_csr(size_t *length_out)
{
    static unsigned char der[APPLIANCE_ACME_CSR_MAX];
    static size_t der_length;
    static bool ready;

    if (!ready) {
        struct appliance_acme_key key;

        if (appliance_acme_key_import(
                &key, fixture_chained_key,
                sizeof(fixture_chained_key)) != APPLIANCE_ACME_JWS_OK ||
            appliance_acme_key_write_csr(
                &key, "example.test", fixture_rng, NULL, der, sizeof(der),
                &der_length) != APPLIANCE_ACME_JWS_OK) {
            der_length = 0U;
        }
        ready = true;
    }
    *length_out = der_length;
    return der;
}

/*
 * The finalize payload must be exactly `{"csr":"<unpadded base64url>"}` for
 * the DER the caller supplied - the item's point is that the old `{}` can no
 * longer be sent.
 */
static void check_finalize_payload(
    const struct appliance_acme_flow *flow,
    const unsigned char *csr,
    size_t csr_length)
{
    static const char payload_prefix[] = "{\"csr\":\"";
    static const char payload_marker[] = "\",\"payload\":\"";
    const char *start = strstr(flow->request, payload_marker);
    const char *payload;
    const char *end;
    char encoded[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    char expected_csr[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    unsigned char decoded[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    size_t encoded_length = 0U;
    size_t expected_csr_length = 0U;
    size_t decoded_length = 0U;

    CHECK(start != NULL, "the finalize request carries a payload member");
    if (start == NULL) {
        return;
    }
    payload = start + sizeof(payload_marker) - 1U;
    end = strchr(payload, '"');
    CHECK(end != NULL, "the finalize payload member is closed");
    if (end == NULL) {
        return;
    }
    encoded_length = (size_t)(end - payload);
    memcpy(encoded, payload, encoded_length);
    encoded[encoded_length] = '\0';
    CHECK(appliance_acme_base64url_decode(
              encoded, encoded_length, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_OK,
          "the finalize payload decodes");
    CHECK(appliance_acme_base64url_encode(
              csr, csr_length, expected_csr, sizeof(expected_csr),
              &expected_csr_length) == APPLIANCE_ACME_BASE64_OK &&
              decoded_length ==
                  sizeof(payload_prefix) - 1U + expected_csr_length + 2U &&
              memcmp(decoded, payload_prefix, sizeof(payload_prefix) - 1U) ==
                  0 &&
              memcmp(&decoded[sizeof(payload_prefix) - 1U], expected_csr,
                     expected_csr_length) == 0 &&
              decoded[decoded_length - 2U] == (unsigned char)'"' &&
              decoded[decoded_length - 1U] == (unsigned char)'}',
          "the finalize payload is the base64url PKCS#10");
}

/*
 * The challenge and the finalize: the CA is told to validate, the order is
 * polled until the challenge is valid, the order is finalized and polled until
 * it names the certificate.  Every request is verified like the earlier ones.
 */
static void test_the_challenge_and_finalize(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    unsigned char oversized[APPLIANCE_ACME_CSR_MAX + 1U];
    const unsigned char *csr;
    size_t csr_length = 0U;
    static const char *const names[] = { "example.test" };
    static const size_t name_lengths[] = { 12U };

    memset(oversized, 0, sizeof(oversized));
    /* Written once, outside any argument list: the length is an input. */
    csr = fixture_csr(&csr_length);

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_declare_identifiers(
                  &flow, names, name_lengths, 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_key_import(
                  &key, fixture_chained_key,
                  sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK &&
              appliance_acme_flow_begin_account(
                  &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_account_result(
                  &flow, account_json, sizeof(account_json) - 1U,
                  "https://acme.example/acct/1", 27U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_order(
                  &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_order_result(
                  &flow, order_json, sizeof(order_json) - 1U,
                  order_url, sizeof(order_url) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_authorization(
                  &flow, &key, "nonce-3", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_authorization_result(
                  &flow, authorization_json,
                  sizeof(authorization_json) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK,
          "the flow reaches the challenge step");

    CHECK(appliance_acme_flow_begin_finalize(
              &flow, &key, "nonce-4", 7U, csr, csr_length, fixture_rng,
              NULL) == APPLIANCE_ACME_FLOW_BAD_STATE,
          "finalize before a valid challenge refuses");

    CHECK(appliance_acme_flow_begin_challenge_poll(
              &flow, &key, "nonce-4", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_BAD_STATE,
          "a poll before the challenge POST refuses");

    CHECK(appliance_acme_flow_begin_challenge(
              &flow, &key, "nonce-4", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING,
          "the challenge request is built");
    check_request(&flow, "https://acme.example/chall/1", "\"kid\":");
    CHECK(strstr(flow.request, "\"payload\":\"e30\"") != NULL,
          "the challenge POST carries the empty JSON object");
    CHECK(strstr(flow.record, "step=CHALLENGE") != NULL,
          "the challenge step is recorded");

    CHECK(appliance_acme_flow_challenge_result(
              &flow, challenge_pending_json,
              sizeof(challenge_pending_json) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING,
          "a processing challenge keeps the caller polling");
    CHECK(appliance_acme_flow_begin_challenge_poll(
              &flow, &key, "nonce-4", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING,
          "a processing challenge is polled as POST-as-GET");
    check_request(&flow, "https://acme.example/chall/1", "\"kid\":");
    {
        const char *payload = strstr(flow.request, "\",\"payload\":\"");

        CHECK(payload != NULL &&
                  strncmp(payload + strlen("\",\"payload\":\""), "\"", 1U) ==
                      0,
              "the challenge poll carries the empty payload");
    }
    CHECK(appliance_acme_flow_challenge_result(
              &flow, challenge_valid_json,
              sizeof(challenge_valid_json) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_READY,
          "a valid challenge opens finalize");

    CHECK(appliance_acme_flow_begin_order_poll(
              &flow, &key, "nonce-5", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_BAD_STATE,
          "an order poll before finalize refuses");

    /*
     * ITEM-508 increment 22: nothing is sent without a CSR, and an oversized
     * one is refused by name, not truncated.
     */
    CHECK(appliance_acme_flow_begin_finalize(
              &flow, &key, "nonce-5", 7U, NULL, 0U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_CSR_REQUIRED &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_READY,
          "a finalize without a CSR refuses and sends nothing");
    CHECK(appliance_acme_flow_begin_finalize(
              &flow, &key, "nonce-5", 7U, oversized, sizeof(oversized),
              fixture_rng, NULL) == APPLIANCE_ACME_FLOW_CSR_TOO_LARGE &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_READY,
          "an oversized CSR refuses by name");

    CHECK(appliance_acme_flow_begin_finalize(
              &flow, &key, "nonce-5", 7U, csr, csr_length, fixture_rng,
              NULL) == APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING,
          "the finalize request is built");
    check_request(&flow, "https://acme.example/finalize/1", "\"kid\":");
    check_finalize_payload(&flow, csr, csr_length);
    CHECK(appliance_acme_flow_finalize_result(
              &flow, order_json, sizeof(order_json) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING,
          "an order still pending keeps the caller polling");
    CHECK(appliance_acme_flow_begin_order_poll(
              &flow, &key, "nonce-6", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING,
          "a pending order is polled as POST-as-GET");
    check_request(&flow, "https://acme.example/order/1", "\"kid\":");
    {
        const char *payload = strstr(flow.request, "\",\"payload\":\"");

        CHECK(payload != NULL &&
                  strncmp(payload + strlen("\",\"payload\":\""), "\"", 1U) ==
                      0,
              "the order poll carries the empty payload");
    }
    CHECK(appliance_acme_flow_finalize_result(
              &flow, finalized_order_json,
              sizeof(finalized_order_json) - 1U) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY &&
              flow.certificate_url.length == 27U,
          "a valid order names the certificate and ends the flow");
    CHECK(strstr(flow.record, "step=CERTIFICATE") != NULL &&
              strstr(flow.record, "state=CERTIFICATE_READY") != NULL,
          "the certificate step is recorded");
    CHECK(strstr(flow.record, "tok3n") == NULL,
          "the certificate record carries no token");
}

static void test_challenge_and_finalize_refusals(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    const unsigned char *csr;
    size_t csr_length = 0U;
    static const char *const names[] = { "example.test" };
    static const size_t name_lengths[] = { 12U };

    csr = fixture_csr(&csr_length);

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_declare_identifiers(
                  &flow, names, name_lengths, 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_key_import(
                  &key, fixture_chained_key,
                  sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK &&
              appliance_acme_flow_begin_account(
                  &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_account_result(
                  &flow, account_json, sizeof(account_json) - 1U,
                  "https://acme.example/acct/1", 27U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_order(
                  &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_order_result(
                  &flow, order_json, sizeof(order_json) - 1U,
                  order_url, sizeof(order_url) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_authorization(
                  &flow, &key, "nonce-3", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_authorization_result(
                  &flow, authorization_json,
                  sizeof(authorization_json) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_challenge(
                  &flow, &key, "nonce-4", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK,
          "the flow reaches the challenge step again");
    CHECK(appliance_acme_flow_challenge_result(
              &flow, challenge_invalid_json,
              sizeof(challenge_invalid_json) - 1U) ==
              APPLIANCE_ACME_FLOW_BAD_STATUS &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a challenge the CA marked invalid fails the flow");

    CHECK(appliance_acme_flow_begin(
              &flow, directory_json, sizeof(directory_json) - 1U,
              FIXTURE_NOW) == APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_declare_identifiers(
                  &flow, names, name_lengths, 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_account(
                  &flow, &key, "nonce-1", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_account_result(
                  &flow, account_json, sizeof(account_json) - 1U,
                  "https://acme.example/acct/1", 27U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_order(
                  &flow, &key, "nonce-2", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_order_result(
                  &flow, order_json, sizeof(order_json) - 1U,
                  order_url, sizeof(order_url) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_authorization(
                  &flow, &key, "nonce-3", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_authorization_result(
                  &flow, authorization_json,
                  sizeof(authorization_json) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_challenge(
                  &flow, &key, "nonce-4", 7U, fixture_rng, NULL) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_challenge_result(
                  &flow, challenge_valid_json,
                  sizeof(challenge_valid_json) - 1U) ==
                  APPLIANCE_ACME_FLOW_OK &&
              appliance_acme_flow_begin_finalize(
                  &flow, &key, "nonce-5", 7U, csr, csr_length, fixture_rng,
                  NULL) == APPLIANCE_ACME_FLOW_OK,
          "the flow reaches the finalize step again");
    CHECK(appliance_acme_flow_finalize_result(
              &flow, finalized_order_without_certificate_json,
              sizeof(finalized_order_without_certificate_json) - 1U) ==
              APPLIANCE_ACME_FLOW_BAD_REPLY &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a valid order without a certificate URL fails the flow");
}

/*
 * ITEM-508 increment 8: the certificate step.  The fixture CA signs for
 * `hfoc-appliance`, so a flow that declared that name can trust the chain the
 * CA returns; a chain for another name, another anchor or another instant is
 * untrusted, and a body that is not a PEM chain fails the order.
 */

static bool drive_to_certificate(
    struct appliance_acme_flow *flow,
    struct appliance_acme_key *key,
    const char *name,
    size_t name_length)
{
    const char *const names[] = { name };
    size_t name_lengths[] = { name_length };
    const unsigned char *csr;
    size_t csr_length = 0U;

    csr = fixture_csr(&csr_length);

    return appliance_acme_flow_begin(flow, directory_json,
                                     sizeof(directory_json) - 1U,
                                     FIXTURE_NOW) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_declare_identifiers(flow, names, name_lengths,
                                                   1U) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_key_import(key, fixture_chained_key,
                                     sizeof(fixture_chained_key)) ==
               APPLIANCE_ACME_JWS_OK &&
           appliance_acme_flow_begin_account(flow, key, "nonce-1", 7U,
                                             fixture_rng, NULL) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_account_result(
               flow, account_json, sizeof(account_json) - 1U,
               "https://acme.example/acct/1", 27U) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_begin_order(flow, key, "nonce-2", 7U,
                                           fixture_rng, NULL) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_order_result(flow, order_json,
                                            sizeof(order_json) - 1U,
                                            order_url,
                                            sizeof(order_url) - 1U) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_begin_authorization(flow, key, "nonce-3", 7U,
                                                   fixture_rng, NULL) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_authorization_result(
               flow, authorization_json,
               sizeof(authorization_json) - 1U) == APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_begin_challenge(flow, key, "nonce-4", 7U,
                                               fixture_rng, NULL) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_challenge_result(
               flow, challenge_valid_json,
               sizeof(challenge_valid_json) - 1U) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_begin_finalize(flow, key, "nonce-5", 7U,
                                              csr, csr_length, fixture_rng,
                                              NULL) ==
               APPLIANCE_ACME_FLOW_OK &&
           appliance_acme_flow_finalize_result(
               flow, finalized_order_json,
               sizeof(finalized_order_json) - 1U) == APPLIANCE_ACME_FLOW_OK &&
           flow->state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY;
}

static void test_the_certificate_step(void)
{
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    struct appliance_acme_trust trust;
    unsigned char der[APPLIANCE_ACME_TRUST_CHAIN_MAX];
    size_t der_length = 99U;
    uint32_t members = 99U;

    CHECK(appliance_acme_trust_init(&trust, fixture_chain_ca,
                                    sizeof(fixture_chain_ca)) ==
              APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the certificate step");

    CHECK(drive_to_certificate(&flow, &key, "hfoc-appliance", 14U),
          "the flow reaches the certificate step");
    CHECK(appliance_acme_flow_begin_certificate(
              &flow, &key, "nonce-6", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY,
          "the certificate POST-as-GET request is built");
    check_request(&flow, "https://acme.example/cert/1", "\"kid\":");
    {
        const char *payload = strstr(flow.request, "\",\"payload\":\"");

        CHECK(payload != NULL &&
                  strncmp(payload + strlen("\",\"payload\":\""), "\"", 1U) ==
                      0,
              "the certificate POST-as-GET carries the empty payload");
    }
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, NULL, 0U, FIXTURE_NOW, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_NULL,
          "a missing certificate body refuses");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_NOW, der, 64U,
              &der_length, &members) ==
              APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY,
          "a body buffer too small refuses and leaves the order usable");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, 0U, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_BAD_CLOCK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY,
          "an unvalidated clock refuses the certificate and leaves the order");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_NOW, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_OK &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_VERIFIED &&
              flow.certificate_verified && members == 2U &&
              der_length == sizeof(fixture_chained_certificate) +
                                sizeof(fixture_chain_ca) &&
              memcmp(der, fixture_chained_certificate,
                     sizeof(fixture_chained_certificate)) == 0,
          "the chain the CA returned verifies and its DER is published");
    CHECK(strstr(flow.record, "step=CERTIFICATE") != NULL &&
              strstr(flow.record, "state=CERTIFICATE_VERIFIED") != NULL,
          "the verified certificate is recorded");
    CHECK(strstr(flow.record, "BEGIN CERTIFICATE") == NULL,
          "the record never carries the certificate itself");
    CHECK(appliance_acme_flow_begin_certificate(
              &flow, &key, "nonce-7", 7U, fixture_rng, NULL) ==
              APPLIANCE_ACME_FLOW_BAD_STATE,
          "the certificate request is out of order after it verified");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_NOW, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_BAD_STATE,
          "a second certificate download is out of order");
    CHECK(der_length == 0U && members == 0U,
          "a refused certificate publishes no DER length");

    CHECK(drive_to_certificate(&flow, &key, "other-name", 10U),
          "a flow with another declared name reaches the certificate step");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_NOW, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_UNTRUSTED &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a chain for another name is untrusted and fails the order");

    CHECK(drive_to_certificate(&flow, &key, "hfoc-appliance", 14U) &&
              appliance_acme_trust_init(&trust, fixture_chain_unrelated,
                                        sizeof(fixture_chain_unrelated)) ==
                  APPLIANCE_ACME_TRUST_OK,
          "a flow reaches the certificate step with another anchor bound");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_NOW, der, sizeof(der),
              &der_length, &members) == APPLIANCE_ACME_FLOW_UNTRUSTED &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a chain from another CA is untrusted");

    CHECK(drive_to_certificate(&flow, &key, "hfoc-appliance", 14U),
          "a flow reaches the certificate step for the malformed body");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, "not a certificate chain\n", 24U, FIXTURE_NOW,
              der, sizeof(der), &der_length, &members) ==
              APPLIANCE_ACME_FLOW_BAD_CHAIN &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a body that is not a certificate chain fails the order");

    CHECK(drive_to_certificate(&flow, &key, "hfoc-appliance", 14U) &&
              appliance_acme_trust_init(&trust, fixture_chain_ca,
                                        sizeof(fixture_chain_ca)) ==
                  APPLIANCE_ACME_TRUST_OK,
          "a flow reaches the certificate step for the window proof");
    CHECK(appliance_acme_flow_certificate_result(
              &flow, &trust, fixture_chain_pem,
              sizeof(fixture_chain_pem) - 1U, FIXTURE_AFTER, der,
              sizeof(der), &der_length, &members) ==
              APPLIANCE_ACME_FLOW_UNTRUSTED &&
              flow.state == APPLIANCE_ACME_FLOW_STATE_FAILED,
          "a chain outside its validity window is untrusted");
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: the mbedTLS pools do not initialize\n");
        return 1;
    }
    test_unvalidated_clock_starts_nothing();
    test_the_order_spine();
    test_the_listener_arms_from_the_flow();
    test_bad_inputs_refuse();
    test_the_challenge_and_finalize();
    test_challenge_and_finalize_refusals();
    test_the_certificate_step();
    hfoc_mbedtls_alloc_shutdown();

    if (failures != 0) {
        fprintf(stderr, "appliance acme flow host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme flow host test: OK\n");
    return 0;
}
