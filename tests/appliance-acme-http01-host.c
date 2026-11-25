/*
 * ITEM-508 increment 28: the HTTP-01 listener on the host.
 *
 * The wire half of the module is driven here as far as the host can honestly
 * drive it: the exact response bytes for every named decision, the refusal
 * when the answer does not fit, the request-line boundary of one received
 * segment (increment 29), the production boot state (listening, unarmed, port
 * 80) and the arm/disarm lifecycle through the increment-7 decision module.
 * It does not fake a TCP exchange: the connection callbacks are the kernel's,
 * proven by the kernel build and the QEMU wire proof.
 */
#include "appliance_acme_challenge.h"
#include "appliance_production_acme_http01.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_ACME_HTTP01_HOST_TEST

static int failures;
static int checks;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

u32_t sys_now(void)
{
    return 1000U;
}

/*
 * The host proof links no time owner: the module's activity clock is stubbed
 * to a fixed tick, exactly as the wrapper's own host proof stubs `sys_now`.
 */
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

static void test_response_bytes(void)
{
    static const char answer[] = "token.thumbprint";
    char out[APPLIANCE_PRODUCTION_ACME_HTTP01_RESPONSE_MAX];
    size_t length = 0U;
    static const char expected_get[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 16\r\n"
        "\r\n"
        "token.thumbprint";
    static const char expected_head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 16\r\n"
        "\r\n";
    static const char expected_404[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    static const char expected_405[] =
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    static const char expected_400[] =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_OK, true, answer,
              sizeof(answer) - 1U, out, sizeof(out), &length) &&
              length == sizeof(expected_get) - 1U &&
              memcmp(out, expected_get, length) == 0,
          "GET 200 carries exactly the key authorization");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_OK, false, answer,
              sizeof(answer) - 1U, out, sizeof(out), &length) &&
              length == sizeof(expected_head) - 1U &&
              memcmp(out, expected_head, length) == 0,
          "HEAD 200 names the body length and carries no body");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE, false, NULL, 0U, out,
              sizeof(out), &length) &&
              length == sizeof(expected_404) - 1U &&
              memcmp(out, expected_404, length) == 0,
          "an unarmed challenge is an empty 404");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_NOT_FOUND, false, NULL, 0U, out,
              sizeof(out), &length) &&
              length == sizeof(expected_404) - 1U &&
              memcmp(out, expected_404, length) == 0,
          "a wrong path is an empty 404");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_EXPIRED, false, NULL, 0U, out,
              sizeof(out), &length) &&
              length == sizeof(expected_404) - 1U &&
              memcmp(out, expected_404, length) == 0,
          "an expired answer is an empty 404");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED, false, NULL, 0U,
              out, sizeof(out), &length) &&
              length == sizeof(expected_405) - 1U &&
              memcmp(out, expected_405, length) == 0,
          "a POST is an empty 405");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_BAD_REQUEST, false, NULL, 0U, out,
              sizeof(out), &length) &&
              length == sizeof(expected_400) - 1U &&
              memcmp(out, expected_400, length) == 0,
          "a malformed request line is an empty 400");
    CHECK(appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL, false, NULL, 0U,
              out, sizeof(out), &length) &&
              length == sizeof(expected_400) - 1U &&
              memcmp(out, expected_400, length) == 0,
          "an oversized request line is an empty 400");
    CHECK(!appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_OK, true, answer,
              sizeof(answer) - 1U, out, 16U, &length) && length == 0U,
          "a response that does not fit is refused, not truncated");
    CHECK(!appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_OK, true, NULL, 0U, out, sizeof(out),
              &length),
          "an empty key authorization is refused, not served");
    CHECK(!appliance_production_acme_http01_build_response(
              APPLIANCE_ACME_CHALLENGE_OK, true, answer,
              sizeof(answer) - 1U, NULL, sizeof(out), &length),
          "a null output buffer is refused");
}

static void test_line_bytes(void)
{
    /*
     * A real HTTP/1.1 client sends the request line and its header lines in
     * one segment.  The first wire-proof run fed the whole segment to the
     * parser, whose control-character rule then refused every request as a
     * malformed `400`; these checks pin the line boundary and the negative
     * control that made the defect visible.
     */
    static const char segment[] =
        "GET /.well-known/acme-challenge/token HTTP/1.1\r\n"
        "Host: 10.0.2.15\r\n"
        "Connection: close\r\n"
        "\r\n";
    static const char complete_line[] =
        "HEAD /.well-known/acme-challenge/token HTTP/1.1\r\n";
    static const char partial_line[] =
        "GET /.well-known/acme-challenge/to";
    const char *first_newline = strchr(segment, '\n');
    struct appliance_acme_challenge_request request;

    CHECK(appliance_production_acme_http01_line_bytes(NULL, 12U) == 0U,
          "a null segment carries no line");
    CHECK(appliance_production_acme_http01_line_bytes(segment, 0U) == 0U,
          "an empty segment carries no line");
    CHECK(first_newline != NULL &&
              appliance_production_acme_http01_line_bytes(
                  segment, sizeof(segment) - 1U) ==
                  (size_t)(first_newline - segment) + 1U,
          "only the request line is taken from a segment with headers");
    CHECK(appliance_production_acme_http01_line_bytes(
              complete_line, sizeof(complete_line) - 1U) ==
              sizeof(complete_line) - 1U,
          "a complete lone line is taken whole");
    CHECK(appliance_production_acme_http01_line_bytes(
              partial_line, sizeof(partial_line) - 1U) ==
              sizeof(partial_line) - 1U,
          "a line with no ending stays incomplete");
    /*
     * The negative control: the raw segment is exactly what the parser
     * refuses, so the boundary above is what makes the wire answer possible.
     */
    CHECK(appliance_acme_challenge_read_request_line(
              segment, sizeof(segment) - 1U, &request) ==
              APPLIANCE_ACME_CHALLENGE_BAD_REQUEST,
          "the raw segment with headers is refused as malformed");
    CHECK(appliance_acme_challenge_read_request_line(
              segment,
              appliance_production_acme_http01_line_bytes(
                  segment, sizeof(segment) - 1U),
              &request) == APPLIANCE_ACME_CHALLENGE_OK &&
              request.method_length == 3U &&
              memcmp(request.method, "GET", 3U) == 0 &&
              request.path_length == 33U &&
              memcmp(request.path, "/.well-known/acme-challenge/token", 33U) ==
                  0,
          "the extracted request line parses to the exact target");
}

static void test_init_and_lifecycle(void)
{
    struct appliance_production_acme_http01 http01;
    struct hfoc_lwip_netif netif;
    struct network_ipv4_config config;
    const char *record;
    static const char token[] = "token-value";
    static const char answer[] = "token-value.thumbprint";

    CHECK(appliance_production_acme_http01_port() == 80U,
          "the listener port is 80");
    CHECK(appliance_production_acme_http01_init(NULL, NULL, NULL) ==
              APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED,
          "init rejects a null owner");
    memset(&http01, 0, sizeof(http01));
    CHECK(appliance_production_acme_http01_arm(
              &http01, token, sizeof(token) - 1U, answer, sizeof(answer) - 1U,
              1000U) == APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "arming before the listener exists is BAD_STATE");
    make_live_netif(&netif);
    make_config(&config);
    CHECK(appliance_production_acme_http01_init(&http01, &netif, &config) ==
              APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING &&
              appliance_production_acme_http01_listening(&http01),
          "the live wire publishes the listener");
    CHECK(!appliance_production_acme_http01_armed(&http01),
          "the production listener comes up unarmed");
    CHECK(appliance_production_acme_http01_active_count(&http01) == 0U &&
              appliance_production_acme_http01_accept_count(&http01) == 0U,
          "a fresh listener holds no connection");
    record = appliance_production_acme_http01_record(&http01);
    CHECK(record != NULL && strstr(record, "[ACME-HTTP01] v=1") == record &&
              strstr(record, "state=LISTENING") != NULL &&
              strstr(record, "armed=0") != NULL &&
              strstr(record, "port=80") != NULL,
          "the unarmed production record names state, arming and port");
    CHECK(appliance_production_acme_http01_arm(
              &http01, token, sizeof(token) - 1U, answer, sizeof(answer) - 1U,
              1000U) == APPLIANCE_ACME_CHALLENGE_OK &&
              appliance_production_acme_http01_armed(&http01),
          "the order path can arm exactly one token");
    record = appliance_production_acme_http01_record(&http01);
    CHECK(record != NULL && strstr(record, "armed=1") != NULL,
          "the record follows the arming");
    CHECK(appliance_production_acme_http01_arm(
              &http01, "", 0U, answer, sizeof(answer) - 1U, 1000U) !=
              APPLIANCE_ACME_CHALLENGE_OK,
          "an empty token is refused by the decision module");
    appliance_production_acme_http01_disarm(&http01);
    CHECK(!appliance_production_acme_http01_armed(&http01),
          "disarm closes the challenge");
    {
        struct appliance_production_acme_http01 failed;

        memset(&failed, 0, sizeof(failed));
        CHECK(appliance_production_acme_http01_init(
                  &failed, NULL, &config) ==
                  APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED,
              "init without a live netif fails");
        record = appliance_production_acme_http01_record(&failed);
        CHECK(record != NULL && strstr(record, "state=FAILED") != NULL &&
                  strstr(record, "reason=") != NULL,
              "a failed listener names its reason in the record");
    }
}

int main(void)
{
    test_response_bytes();
    test_line_bytes();
    test_init_and_lifecycle();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("ALL OK (%d checks)\n", checks);
    return 0;
}

#else
typedef int appliance_acme_http01_host_test_guard;
#endif
