#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_IPV4_CONFIG_HOST_TEST

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

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static const char valid_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.10\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

static const char reordered_config[] =
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n"
    "ipv4_prefix=24\n"
    "ipv4_address=192.0.2.10\n"
    "schema_version=1\n";

static void check_parse_failure(const char *input,
                                enum network_ipv4_status expected,
                                const char *message)
{
    struct network_ipv4_config config;
    unsigned char original[sizeof(config)];

    memset(&config, 0xA5, sizeof(config));
    memcpy(original, &config, sizeof(original));
    CHECK(network_ipv4_config_parse(input, strlen(input), &config) ==
              expected &&
              memcmp(&config, original, sizeof(original)) == 0,
          message);
}

static void test_status_strings(void) {
    static const struct {
        enum network_ipv4_status status;
        const char *string;
    } statuses[] = {
        {NETWORK_IPV4_OK, "NETWORK_IPV4_OK"},
        {NETWORK_IPV4_NULL, "NETWORK_IPV4_NULL"},
        {NETWORK_IPV4_TOO_LONG, "NETWORK_IPV4_TOO_LONG"},
        {NETWORK_IPV4_MISSING_FIELD, "NETWORK_IPV4_MISSING_FIELD"},
        {NETWORK_IPV4_DUPLICATE_FIELD, "NETWORK_IPV4_DUPLICATE_FIELD"},
        {NETWORK_IPV4_UNKNOWN_FIELD, "NETWORK_IPV4_UNKNOWN_FIELD"},
        {NETWORK_IPV4_MALFORMED_FIELD, "NETWORK_IPV4_MALFORMED_FIELD"},
        {NETWORK_IPV4_BAD_VERSION, "NETWORK_IPV4_BAD_VERSION"},
        {NETWORK_IPV4_INVALID_ADDRESS, "NETWORK_IPV4_INVALID_ADDRESS"},
        {NETWORK_IPV4_INVALID_PREFIX, "NETWORK_IPV4_INVALID_PREFIX"},
        {NETWORK_IPV4_INVALID_ROUTE, "NETWORK_IPV4_INVALID_ROUTE"},
        {NETWORK_IPV4_INVALID_DNS, "NETWORK_IPV4_INVALID_DNS"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = network_ipv4_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "IPv4 status string is exact for every status");
    }
    CHECK(network_ipv4_status_string(
              (enum network_ipv4_status)12U) == NULL,
          "IPv4 undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 status handling performs no allocation");
}

static void test_input_and_schema_failures(void) {
    char too_long[NETWORK_IPV4_INPUT_MAX + 1U];
    struct network_ipv4_config config;
    unsigned char original[sizeof(config)];

    memset(too_long, 'a', sizeof(too_long));
    CHECK(network_ipv4_config_parse(NULL, 0U, &config) ==
              NETWORK_IPV4_NULL,
          "IPv4 parser rejects null input");
    CHECK(network_ipv4_config_parse(valid_config,
                                    sizeof(valid_config) - 1U,
                                    NULL) == NETWORK_IPV4_NULL,
          "IPv4 parser rejects null output");
    CHECK(network_ipv4_config_parse(valid_config, 0U, &config) ==
              NETWORK_IPV4_MISSING_FIELD,
          "IPv4 parser rejects empty input");
    CHECK(network_ipv4_config_parse(too_long, sizeof(too_long), &config) ==
              NETWORK_IPV4_TOO_LONG,
          "IPv4 parser rejects overlong input");

    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_foo=192.0.2.53\n",
        NETWORK_IPV4_UNKNOWN_FIELD,
        "IPv4 parser rejects unknown fields");
    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n"
        "ipv4_address=192.0.2.11\n",
        NETWORK_IPV4_DUPLICATE_FIELD,
        "IPv4 parser rejects duplicate fields");
    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n",
        NETWORK_IPV4_MISSING_FIELD,
        "IPv4 parser rejects missing DNS field");
    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway 192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        NETWORK_IPV4_MALFORMED_FIELD,
        "IPv4 parser rejects line without single equals");
    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "\n"
        "ipv4_dns=192.0.2.53\n",
        NETWORK_IPV4_MALFORMED_FIELD,
        "IPv4 parser rejects empty embedded line");
    check_parse_failure(
        "schema_version=2\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        NETWORK_IPV4_BAD_VERSION,
        "IPv4 parser rejects unknown schema version");

    memset(&config, 0xA5, sizeof(config));
    memcpy(original, &config, sizeof(original));
    CHECK(network_ipv4_config_parse(valid_config,
                                    sizeof(valid_config) - 1U,
                                    &config) == NETWORK_IPV4_OK &&
              memcmp(&config, original, sizeof(original)) != 0,
          "IPv4 valid parse replaces poisoned output");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 input and schema handling performs no allocation");
}

static void test_address_failures(void) {
    static const char *const cases[] = {
        "schema_version=1\n"
        "ipv4_address=192.0.2\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10.5\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.256\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.010\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=0.0.0.0\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.-1\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address= 192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10 \n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=0x192.0.2.1\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        check_parse_failure(cases[index], NETWORK_IPV4_INVALID_ADDRESS,
                            "IPv4 parser rejects malformed address");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 address handling performs no allocation");
}

static void test_prefix_failures(void) {
    static const char *const cases[] = {
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=33\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24.0\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=01\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=-1\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        check_parse_failure(cases[index], NETWORK_IPV4_INVALID_PREFIX,
                            "IPv4 parser rejects invalid prefix");
    }
    check_parse_failure(
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n",
        NETWORK_IPV4_MALFORMED_FIELD,
        "IPv4 parser rejects empty prefix value as malformed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 prefix handling performs no allocation");
}

static void test_route_failures(void) {
    static const char *const cases[] = {
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.999\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.3.1\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.0\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.255\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.10\n"
        "ipv4_dns=192.0.2.53\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=0.0.0.0\n"
        "ipv4_dns=192.0.2.53\n",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        check_parse_failure(cases[index], NETWORK_IPV4_INVALID_ROUTE,
                            "IPv4 parser rejects invalid route");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 route handling performs no allocation");
}

static void test_dns_failures(void) {
    static const char *const cases[] = {
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=0.0.0.0\n",
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.256\n",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        check_parse_failure(cases[index], NETWORK_IPV4_INVALID_DNS,
                            "IPv4 parser rejects invalid DNS address");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 DNS handling performs no allocation");
}

static bool config_matches_valid(const struct network_ipv4_config *config) {
    static const uint8_t address[NETWORK_IPV4_ADDRESS_OCTETS] =
        {192U, 0U, 2U, 10U};
    static const uint8_t gateway[NETWORK_IPV4_ADDRESS_OCTETS] =
        {192U, 0U, 2U, 1U};
    static const uint8_t dns[NETWORK_IPV4_ADDRESS_OCTETS] =
        {192U, 0U, 2U, 53U};

    return config != NULL &&
           config->schema_version == NETWORK_IPV4_CONFIG_VERSION &&
           config->prefix_length == 24U &&
           memcmp(config->address.octets, address,
                  sizeof(address)) == 0 &&
           memcmp(config->gateway.octets, gateway,
                  sizeof(gateway)) == 0 &&
           memcmp(config->dns.octets, dns, sizeof(dns)) == 0;
}

static void test_valid_parses(void) {
    struct network_ipv4_config config;
    struct network_ipv4_config zero_prefix;
    static const char prefix_zero[] =
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=0\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n";

    CHECK(network_ipv4_config_parse(
              valid_config, sizeof(valid_config) - 1U,
              &config) == NETWORK_IPV4_OK &&
              config_matches_valid(&config),
          "IPv4 parser publishes exact valid configuration");
    CHECK(network_ipv4_config_parse(
              reordered_config, sizeof(reordered_config) - 1U,
              &config) == NETWORK_IPV4_OK &&
              config_matches_valid(&config),
          "IPv4 parser accepts fields in any order");
    CHECK(network_ipv4_config_parse(
              prefix_zero, sizeof(prefix_zero) - 1U,
              &zero_prefix) == NETWORK_IPV4_OK &&
              zero_prefix.schema_version ==
                  NETWORK_IPV4_CONFIG_VERSION &&
              zero_prefix.prefix_length == 0U,
          "IPv4 parser accepts prefix zero");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "IPv4 valid parsing performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_schema_failures();
    test_address_failures();
    test_prefix_failures();
    test_route_failures();
    test_dns_failures();
    test_valid_parses();

    if (failures != 0) {
        fprintf(stderr, "IPv4 config host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("IPv4 config host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_IPV4_CONFIG_HOST_TEST"
#endif
