#include "appliance_network_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_HOST_TEST

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

static struct network_ipv4_config make_static_config(void)
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

static void test_status_strings(void)
{
    enum appliance_network_config_status status;
    enum appliance_network_config_mode mode;

    for (status = APPLIANCE_NETWORK_CONFIG_OK;
         status <= APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL;
         ++status) {
        const char *string =
            appliance_network_config_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_NETWORK_CONFIG_OK ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_OK") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_NULL ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_NULL") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_BAD_STATE") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_BAD_VERSION") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_INVALID_MODE ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_INVALID_MODE") == 0 :
                   status == APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG") == 0 :
                       strcmp(string,
                              "APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL") == 0),
              "network config status string is exact for every status");
    }
    CHECK(appliance_network_config_status_string(
              (enum appliance_network_config_status)8U) == NULL,
          "network config rejects undefined status string");

    for (mode = APPLIANCE_NETWORK_CONFIG_MODE_NONE;
         mode <= APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE;
         ++mode) {
        const char *string =
            appliance_network_config_mode_string(mode);

        CHECK(string != NULL &&
                  (mode == APPLIANCE_NETWORK_CONFIG_MODE_NONE ?
                       strcmp(string, "NONE") == 0 :
                   mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC ?
                       strcmp(string, "STATIC") == 0 :
                   mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP ?
                       strcmp(string, "DHCP") == 0 :
                       strcmp(string, "OFFLINE") == 0),
              "network config mode string is exact for every mode");
    }
    CHECK(appliance_network_config_mode_string(
              (enum appliance_network_config_mode)99U) == NULL,
          "network config rejects undefined mode string");
}

static void test_null_and_state(void)
{
    struct appliance_network_config config;
    struct network_ipv4_config static_config = make_static_config();
    char interface[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    size_t interface_length = 0U;
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    CHECK(appliance_network_config_publish_default(NULL) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config default rejects null config");
    CHECK(appliance_network_config_publish_dhcp(
              NULL, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_NULL,
          "network config dhcp rejects null config");
    CHECK(appliance_network_config_publish_dhcp(
              &config, NULL, 4U) == APPLIANCE_NETWORK_CONFIG_NULL,
          "network config dhcp rejects null interface");
    CHECK(appliance_network_config_publish_static(
              NULL, "eth0", 4U, &static_config) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config static rejects null config");
    CHECK(appliance_network_config_publish_static(
              &config, NULL, 4U, &static_config) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config static rejects null interface");
    CHECK(appliance_network_config_publish_static(
              &config, "eth0", 4U, NULL) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config static rejects null static config");
    CHECK(appliance_network_config_validate(NULL) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config validate rejects null config");
    CHECK(appliance_network_config_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config render rejects null config");
    CHECK(appliance_network_config_render(
              &config, NULL, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config render rejects null output");
    CHECK(appliance_network_config_render(
              &config, output, sizeof(output), NULL) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config render rejects null output length");
    CHECK(appliance_network_config_interface(
              &config, NULL, 1U, &interface_length) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config interface rejects null output");
    CHECK(appliance_network_config_interface(
              &config, interface, sizeof(interface), NULL) ==
              APPLIANCE_NETWORK_CONFIG_NULL,
          "network config interface rejects null length");

    memset(&config, 0, sizeof(config));
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_BAD_STATE,
          "unpublished network config fails validation");
    CHECK(appliance_network_config_mode(&config) ==
              APPLIANCE_NETWORK_CONFIG_MODE_NONE,
          "unpublished network config mode is none");
    CHECK(appliance_network_config_static_config(&config) == NULL,
          "unpublished network config static accessor is null");
}

static void test_interface_validation(void)
{
    struct appliance_network_config config;
    char too_long[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 2U];

    memset(&config, 0, sizeof(config));
    memset(too_long, 'a', sizeof(too_long));

    CHECK(appliance_network_config_publish_dhcp(
              &config, "", 0U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects empty interface without publication");
    CHECK(appliance_network_config_publish_dhcp(
              &config, too_long, sizeof(too_long)) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects over-limit interface without publication");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "-eth0", 5U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects leading hyphen interface");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "eth0-", 5U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects trailing hyphen interface");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "_eth0", 5U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects leading underscore interface");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "eth0_", 5U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects trailing underscore interface");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "ETH0", 4U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects uppercase interface");
    CHECK(appliance_network_config_publish_dhcp(
              &config, "eth 0", 5U) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE &&
              !config.initialized,
          "network config rejects whitespace interface");
}

static void test_default_and_render(void)
{
    struct appliance_network_config config;
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&config, 0, sizeof(config));
    CHECK(appliance_network_config_publish_default(&config) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              config.initialized &&
              config.version == APPLIANCE_NETWORK_CONFIG_VERSION &&
              config.mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
              config.interface_length == 4U &&
              memcmp(config.interface,
                     APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE,
                     4U) == 0 &&
              !config.static_present,
          "network config default publishes eth0 DHCP");
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "published DHCP default validates");
    CHECK(appliance_network_config_mode(&config) ==
              APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
          "published DHCP default reports DHCP mode");
    CHECK(appliance_network_config_static_config(&config) == NULL,
          "DHCP default has no static config");

    output_length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_network_config_render(
              &config, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              strcmp(output,
                     "network v=1 interface=eth0 mode=DHCP\n") == 0 &&
              output_length == strlen(
                  "network v=1 interface=eth0 mode=DHCP\n"),
          "network config default renders exact canonical record");
}

static void test_dhcp_and_static_publication(void)
{
    struct appliance_network_config config;
    struct appliance_network_config saved;
    struct network_ipv4_config static_config = make_static_config();
    struct network_ipv4_config invalid = static_config;
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&config, 0, sizeof(config));
    CHECK(appliance_network_config_publish_default(&config) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "static publication fixture starts from default");
    saved = config;

    invalid.address.octets[0] = 192U;
    invalid.address.octets[1] = 168U;
    CHECK(appliance_network_config_publish_static(
              &config, "lan0", 4U, &invalid) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG &&
              memcmp(&config, &saved, sizeof(config)) == 0,
          "invalid static publication leaves previous network config unchanged");

    CHECK(appliance_network_config_publish_static(
              &config, "lan0", 4U, &static_config) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              config.mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
              config.static_present &&
              config.interface_length == 4U &&
              memcmp(config.interface, "lan0", 4U) == 0,
          "valid static publication copies exact interface and config");
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "published static network config validates");
    CHECK(appliance_network_config_mode(&config) ==
              APPLIANCE_NETWORK_CONFIG_MODE_STATIC,
          "published static config reports static mode");
    CHECK(appliance_network_config_static_config(&config) != NULL &&
              memcmp(appliance_network_config_static_config(&config),
                     &static_config,
                     sizeof(static_config)) == 0,
          "static config accessor returns exact immutable config");

    output_length = 0U;
    memset(output, 0, sizeof(output));
    CHECK(appliance_network_config_render(
              &config, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              strcmp(output,
                     "network v=1 interface=lan0 mode=STATIC "
                     "ipv4_address=10.0.2.15 ipv4_prefix=24 "
                     "ipv4_gateway=10.0.2.2 ipv4_dns=10.0.2.3\n") == 0,
          "static network config renders exact canonical record");

    saved = config;
    CHECK(appliance_network_config_publish_dhcp(
              &config, "eth0", 4U) == APPLIANCE_NETWORK_CONFIG_OK &&
              config.mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
              !config.static_present &&
              appliance_network_config_validate(&config) ==
                  APPLIANCE_NETWORK_CONFIG_OK,
          "DHCP publication clears stale static state");
    CHECK(memcmp(&config, &saved, sizeof(config)) != 0,
          "successful DHCP publication changes published state");
}

static void test_render_failure_and_interface_copy(void)
{
    struct appliance_network_config config;
    struct network_ipv4_config static_config = make_static_config();
    char output[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX + 1U];
    char interface[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    char small[4] = {'K', 'E', 'E', 'P'};
    size_t output_length = 0U;
    size_t interface_length = 0U;

    memset(&config, 0, sizeof(config));
    (void)appliance_network_config_publish_static(
        &config, "lan0", 4U, &static_config);

    CHECK(appliance_network_config_render(
              &config, small, sizeof(small), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL &&
              memcmp(small, "KEEP", sizeof(small)) == 0,
          "network config render rejects small buffer without mutation");
    CHECK(appliance_network_config_render(
              &config, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_CONFIG_OK &&
              appliance_network_config_interface(
                  &config, interface, sizeof(interface),
                  &interface_length) == APPLIANCE_NETWORK_CONFIG_OK &&
              interface_length == 4U &&
              memcmp(interface, "lan0", 4U) == 0 &&
              interface[interface_length] == '\0',
          "network config interface copies bounded name");

    memset(interface, 0, sizeof(interface));
    interface_length = 0U;
    CHECK(appliance_network_config_interface(
              &config, interface, 4U, &interface_length) ==
              APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL,
          "network config interface rejects too-small output");
}

static void test_manual_poisoning(void)
{
    struct appliance_network_config config;

    memset(&config, 0, sizeof(config));
    (void)appliance_network_config_publish_dhcp(
        &config, "eth0", 4U);
    config.mode = APPLIANCE_NETWORK_CONFIG_MODE_STATIC;
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_BAD_STATE,
          "network config rejects static mode without static config");

    memset(&config, 0, sizeof(config));
    (void)appliance_network_config_publish_dhcp(
        &config, "eth0", 4U);
    config.mode = APPLIANCE_NETWORK_CONFIG_MODE_STATIC;
    config.static_present = true;
    config.static_config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG,
          "network config rejects invalid static config");

    memset(&config, 0, sizeof(config));
    (void)appliance_network_config_publish_dhcp(
        &config, "eth0", 4U);
    config.mode = APPLIANCE_NETWORK_CONFIG_MODE_DHCP;
    config.static_present = true;
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_BAD_STATE,
          "network config rejects stale static state in DHCP mode");

    memset(&config, 0, sizeof(config));
    (void)appliance_network_config_publish_dhcp(
        &config, "eth0", 4U);
    config.version = 0U;
    CHECK(appliance_network_config_validate(&config) ==
              APPLIANCE_NETWORK_CONFIG_BAD_VERSION,
          "network config rejects bad version");
}

int main(void)
{
    test_status_strings();
    test_null_and_state();
    test_interface_validation();
    test_default_and_render();
    test_dhcp_and_static_publication();
    test_render_failure_and_interface_copy();
    test_manual_poisoning();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network config model performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance network config host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance network config host tests: ALL OK");
    return 0;
}

#endif
