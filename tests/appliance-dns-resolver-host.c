#include "appliance_dns_resolver.h"

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_DNS_RESOLVER_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct network_ipv4_config make_config(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static struct network_ipv4_config make_invalid_config(void)
{
    struct network_ipv4_config config = make_config();

    config.dns.octets[0] = 0U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 0U;
    config.dns.octets[3] = 0U;
    return config;
}

static struct network_ipv4_address make_answer(void)
{
    struct network_ipv4_address answer;

    memset(&answer, 0, sizeof(answer));
    answer.octets[0] = 10U;
    answer.octets[2] = 2U;
    answer.octets[3] = 15U;
    return answer;
}

static void test_status_strings(void)
{
    enum appliance_dns_resolver_status status;
    enum appliance_dns_resolver_state state;

    for (status = APPLIANCE_DNS_RESOLVER_OK;
         status <= APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
         ++status) {
        const char *string =
            appliance_dns_resolver_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_DNS_RESOLVER_OK ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_OK") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_NULL ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_NULL") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BAD_STATE") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BAD_VERSION") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BAD_CONFIG") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_BAD_ANSWER ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BAD_ANSWER") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_STOPPED ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_STOPPED") == 0 :
                   status == APPLIANCE_DNS_RESOLVER_EXHAUSTED ?
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_EXHAUSTED") == 0 :
                       strcmp(string,
                              "APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL") == 0),
              "DNS resolver status string is exact for every status");
    }
    CHECK(appliance_dns_resolver_status_string(
              (enum appliance_dns_resolver_status)10U) == NULL,
          "DNS resolver rejects undefined status string");

    for (state = APPLIANCE_DNS_RESOLVER_STATE_NONE;
         state <= APPLIANCE_DNS_RESOLVER_STATE_FAILED;
         ++state) {
        const char *string =
            appliance_dns_resolver_state_string(state);

        CHECK(string != NULL &&
                  (state == APPLIANCE_DNS_RESOLVER_STATE_NONE ?
                       strcmp(string, "NONE") == 0 :
                   state == APPLIANCE_DNS_RESOLVER_STATE_IDLE ?
                       strcmp(string, "IDLE") == 0 :
                   state == APPLIANCE_DNS_RESOLVER_STATE_READY ?
                       strcmp(string, "READY") == 0 :
                   state == APPLIANCE_DNS_RESOLVER_STATE_QUERYING ?
                       strcmp(string, "QUERYING") == 0 :
                   state == APPLIANCE_DNS_RESOLVER_STATE_RESOLVED ?
                       strcmp(string, "RESOLVED") == 0 :
                       strcmp(string, "FAILED") == 0),
              "DNS resolver state string is exact for every state");
    }
    CHECK(appliance_dns_resolver_state_string(
              (enum appliance_dns_resolver_state)6U) == NULL,
          "DNS resolver rejects undefined state string");
}

static void test_null_and_invalid(void)
{
    struct appliance_dns_resolver resolver;
    struct network_ipv4_config config = make_config();
    struct network_ipv4_config invalid = make_invalid_config();
    struct network_ipv4_address answer = make_answer();
    char output[APPLIANCE_DNS_RESOLVER_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&resolver, 0, sizeof(resolver));
    CHECK(appliance_dns_resolver_init(
              NULL, &config, "eth0", 4U) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver init rejects null resolver");
    CHECK(appliance_dns_resolver_init(
              &resolver, NULL, "eth0", 4U) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver init rejects null active config");
    CHECK(appliance_dns_resolver_init(
              &resolver, &invalid, "eth0", 4U) ==
              APPLIANCE_DNS_RESOLVER_BAD_CONFIG,
          "DNS resolver rejects missing DNS server");
    CHECK(appliance_dns_resolver_init(
              &resolver, &config, "bad_interface", 13U) ==
              APPLIANCE_DNS_RESOLVER_BAD_CONFIG,
          "DNS resolver rejects invalid interface");
    CHECK(appliance_dns_resolver_query(
              &resolver, "api.hfoc.test", 13U) ==
              APPLIANCE_DNS_RESOLVER_BAD_STATE,
          "DNS resolver query rejects uninitialized resolver");
    CHECK(appliance_dns_resolver_query(
              NULL, "api.hfoc.test", 12U) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver query rejects null resolver");
    CHECK(appliance_dns_resolver_response(
              NULL, &answer) == APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver response rejects null resolver");
    CHECK(appliance_dns_resolver_timeout(NULL) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver timeout rejects null resolver");
    CHECK(appliance_dns_resolver_server_failure(NULL) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver server failure rejects null resolver");
    CHECK(appliance_dns_resolver_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver render rejects null resolver");
    CHECK(appliance_dns_resolver_render(
              &resolver, NULL, sizeof(output), &output_length) ==
              APPLIANCE_DNS_RESOLVER_NULL,
          "DNS resolver render rejects null output");
    CHECK(appliance_dns_resolver_state(&resolver) ==
              APPLIANCE_DNS_RESOLVER_STATE_NONE,
          "uninitialized DNS resolver state is none");
    CHECK(appliance_dns_resolver_answer(&resolver) == NULL,
          "uninitialized DNS resolver answer is null");
    CHECK(!appliance_dns_resolver_answer_published(&resolver),
          "uninitialized DNS resolver answer is unpublished");
}

static void test_lifecycle_and_render(void)
{
    struct appliance_dns_resolver resolver;
    struct network_ipv4_config config = make_config();
    struct network_ipv4_address answer = make_answer();
    struct network_ipv4_address zero;
    char output[APPLIANCE_DNS_RESOLVER_OUTPUT_MAX + 1U];
    char tiny[16U];
    char tiny_original[16U];
    char exact_no_nul[110U];
    char exact_original[110U];
    const char *expected =
        "dns_resolver v=1 interface=eth0 server=10.0.2.3 "
        "host=api.hfoc.test state=RESOLVED result=10.0.2.15 "
        "attempts=2\n";
    size_t output_length = 0U;
    size_t index;

    memset(&resolver, 0, sizeof(resolver));
    memset(&zero, 0, sizeof(zero));
    memset(tiny, 0x55, sizeof(tiny));
    memcpy(tiny_original, tiny, sizeof(tiny_original));
    memset(exact_no_nul, 0x5a, sizeof(exact_no_nul));
    memcpy(exact_original, exact_no_nul, sizeof(exact_original));

    CHECK(appliance_dns_resolver_init(
              &resolver, &config, "eth0", 4U) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.initialized &&
              resolver.version == APPLIANCE_DNS_RESOLVER_VERSION &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_READY &&
              resolver.max_attempts == APPLIANCE_DNS_RESOLVER_MAX_ATTEMPTS &&
              resolver.server.octets[3] == 3U,
          "DNS resolver init publishes validated server");
    CHECK(appliance_dns_resolver_query(
              &resolver, "bad_host", 8U) ==
              APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_READY,
          "DNS resolver rejects underscore hostname without mutation");
    CHECK(appliance_dns_resolver_query(
              &resolver, "", 0U) ==
              APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME,
          "DNS resolver rejects empty hostname");
    CHECK(appliance_dns_resolver_query(
              &resolver, "api.hfoc.test", 13U) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_QUERYING &&
              resolver.attempt_count == 1U &&
              resolver.hostname_length == 13U &&
              memcmp(resolver.hostname, "api.hfoc.test", 13U) == 0,
          "DNS resolver query enters querying");
    CHECK(appliance_dns_resolver_response(
              &resolver, &zero) ==
              APPLIANCE_DNS_RESOLVER_BAD_ANSWER &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_QUERYING,
          "DNS resolver rejects zero answer without mutation");
    CHECK(appliance_dns_resolver_timeout(&resolver) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_QUERYING &&
              resolver.attempt_count == 2U,
          "DNS resolver timeout keeps bounded retry in querying");
    CHECK(appliance_dns_resolver_response(
              &resolver, &answer) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_RESOLVED &&
              resolver.answer_published &&
              appliance_dns_resolver_answer(&resolver) != NULL &&
              memcmp(&resolver.answer, &answer,
                     sizeof(resolver.answer)) == 0,
          "DNS resolver response publishes validated answer");

    output_length = 0U;
    CHECK(appliance_dns_resolver_render(
              &resolver, output, sizeof(output), &output_length) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              output_length == strlen(expected) &&
              output[output_length] == '\0' &&
              memcmp(output, expected, strlen(expected)) == 0,
          "DNS resolver canonical resolved render is exact");

    output_length = 77U;
    CHECK(appliance_dns_resolver_render(
              &resolver, tiny, sizeof(tiny), &output_length) ==
              APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL &&
              output_length == 77U &&
              memcmp(tiny, tiny_original, sizeof(tiny)) == 0,
          "DNS resolver render overflow preserves caller buffer");
    output_length = 77U;
    CHECK(appliance_dns_resolver_render(
              &resolver, exact_no_nul, sizeof(exact_no_nul),
              &output_length) ==
              APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL &&
              output_length == 77U &&
              memcmp(exact_no_nul, exact_original,
                     sizeof(exact_no_nul)) == 0,
          "DNS resolver render requires terminator capacity");

    CHECK(appliance_dns_resolver_clear(&resolver) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_READY &&
              !resolver.answer_published &&
              resolver.server.octets[3] == 3U,
          "DNS resolver clear preserves server and returns ready");
    CHECK(appliance_dns_resolver_query(
              &resolver, "api.hfoc.test", 13U) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              appliance_dns_resolver_timeout(&resolver) ==
                  APPLIANCE_DNS_RESOLVER_OK &&
              resolver.attempt_count == 2U &&
              appliance_dns_resolver_timeout(&resolver) ==
                  APPLIANCE_DNS_RESOLVER_EXHAUSTED &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_FAILED &&
              !resolver.answer_published,
          "DNS resolver timeout retry is bounded and fails closed");
    CHECK(appliance_dns_resolver_clear(&resolver) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_READY &&
              resolver.hostname_length == 0U,
          "DNS resolver failure can be cleared for a fresh query");

    CHECK(appliance_dns_resolver_query(
              &resolver, "api.hfoc.test", 13U) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              appliance_dns_resolver_server_failure(&resolver) ==
                  APPLIANCE_DNS_RESOLVER_OK &&
              resolver.attempt_count == 2U &&
              appliance_dns_resolver_server_failure(&resolver) ==
                  APPLIANCE_DNS_RESOLVER_EXHAUSTED &&
              resolver.state == APPLIANCE_DNS_RESOLVER_STATE_FAILED,
          "DNS resolver server failure is bounded");
    CHECK(appliance_dns_resolver_reset(&resolver) ==
              APPLIANCE_DNS_RESOLVER_OK &&
              !resolver.initialized,
          "DNS resolver reset returns to unpublished state");

    for (index = 0U; index < strlen(expected); ++index) {
        if (output[index] == '\0') {
            break;
        }
    }
    (void)index;
}

int main(void)
{
    test_status_strings();
    test_null_and_invalid();
    test_lifecycle_and_render();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr, "FAIL: DNS resolver performed heap allocation\n");
        ++failures;
    }
    if (failures != 0) {
        fprintf(stderr, "DNS resolver host test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("DNS resolver host test: OK\n");
    return 0;
}

#endif
