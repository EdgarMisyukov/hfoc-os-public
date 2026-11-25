/*
 * ITEM-449 host proof for the forward declaration and its access rule.
 *
 * The declaration is the whole access rule: which literal target, which local
 * port, and which local-network clients may use the forward.  This proof is
 * deliberately exhaustive on the refusals, because "the operator's declaration
 * opens a port or is refused" is only as good as these decisions.
 */
#include "forward_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static enum forward_config_status parts(
    bool declared,
    bool enabled,
    const char *target,
    const char *target_port,
    const char *listen_port,
    const char *clients,
    struct forward_config *config)
{
    return forward_config_from_parts(
        declared, enabled,
        target, target != NULL ? strlen(target) : 0U,
        target_port, target_port != NULL ? strlen(target_port) : 0U,
        listen_port, listen_port != NULL ? strlen(listen_port) : 0U,
        clients, clients != NULL ? strlen(clients) : 0U,
        config);
}

static bool address_bytes(const char *text, uint8_t out[4])
{
    unsigned int a = 0U;
    unsigned int b = 0U;
    unsigned int c = 0U;
    unsigned int d = 0U;

    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static enum forward_config_status admit_text(
    const struct forward_config *config,
    const char *text)
{
    uint8_t source[4];

    memset(source, 0, sizeof(source));
    if (!address_bytes(text, source)) {
        return FORWARD_CONFIG_NULL;
    }
    return forward_config_admit(config, source);
}

static void test_absent_declaration(void)
{
    struct forward_config config;
    char rendered[64];
    size_t length = 0U;

    CHECK(parts(false, false, NULL, NULL, NULL, NULL, &config) ==
              FORWARD_CONFIG_OK,
          "an install entry that declares nothing is not a refusal");
    CHECK(!config.declared && !config.enabled,
          "nothing declared means no declaration and no forward");
    CHECK(!forward_config_enabled(&config),
          "an absent declaration is not an enabled forward");
    CHECK(forward_config_render(
              &config, rendered, sizeof(rendered), &length) ==
              FORWARD_CONFIG_OK,
          "an absent declaration still renders a record");
    CHECK(strcmp(rendered, "target=none listen=none clients=0") == 0,
          "the record names no rule instead of inventing one");
    CHECK(admit_text(&config, "10.0.0.9") ==
              FORWARD_CONFIG_DENIED_NO_DECLARATION,
          "a source is denied when nothing was declared");
}

static void test_disabled_declaration(void)
{
    struct forward_config config;
    char rendered[64];
    size_t length = 0U;

    CHECK(parts(true, false, NULL, NULL, NULL, NULL, &config) ==
              FORWARD_CONFIG_OK,
          "an explicit off is a declaration, not a malformed one");
    CHECK(config.declared && !config.enabled,
          "the record distinguishes declared-off from not declared");
    CHECK(!forward_config_enabled(&config), "declared-off opens nothing");
    CHECK(forward_config_render(
              &config, rendered, sizeof(rendered), &length) ==
              FORWARD_CONFIG_OK,
          "a disabled declaration renders");
    CHECK(strcmp(rendered, "target=none listen=none clients=0") == 0,
          "a disabled declaration renders no rule");
    CHECK(admit_text(&config, "10.0.0.9") ==
              FORWARD_CONFIG_DENIED_DISABLED,
          "a source is denied by an explicit off");
    CHECK(parts(true, false, "10.0.2.3", NULL, NULL, NULL, &config) ==
              FORWARD_CONFIG_VALUES_WITH_DISABLED,
          "a disabled declaration refuses values instead of keeping them");
    CHECK(parts(false, false, "10.0.2.3", NULL, NULL, NULL, &config) ==
              FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION,
          "a value without any declaration is refused");
}

static void test_enabled_declaration(void)
{
    struct forward_config config;
    char rendered[128];
    size_t length = 0U;

    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.9,10.0.0.8", &config) == FORWARD_CONFIG_OK,
          "a complete declaration is accepted");
    CHECK(forward_config_enabled(&config),
          "a complete declaration is the enabled forward");
    CHECK(config.target[0] == 10U && config.target[1] == 0U &&
              config.target[2] == 2U && config.target[3] == 3U,
          "the target is the declared literal address");
    CHECK(config.target_port == 8443U && config.listen_port == 9443U,
          "both ports are the declared ones");
    CHECK(config.client_count == 2U,
          "both declared clients are kept");
    CHECK(forward_config_validate(&config) == FORWARD_CONFIG_OK,
          "the composed declaration validates");
    CHECK(forward_config_render(
              &config, rendered, sizeof(rendered), &length) ==
              FORWARD_CONFIG_OK,
          "the rule renders");
    CHECK(strcmp(rendered,
                 "target=10.0.2.3:8443 listen=9443 clients=2") == 0,
          "the rule names the target, the port and the client count");
    CHECK(admit_text(&config, "10.0.0.9") == FORWARD_CONFIG_ADMITTED,
          "a declared client is admitted");
    CHECK(admit_text(&config, "10.0.0.8") == FORWARD_CONFIG_ADMITTED,
          "the second declared client is admitted too");
    CHECK(admit_text(&config, "10.0.0.7") ==
              FORWARD_CONFIG_DENIED_NOT_ALLOWED,
          "a source that is not declared is denied by name");
}

static void test_incomplete_declaration(void)
{
    struct forward_config config;

    CHECK(parts(true, true, NULL, "8443", "9443", "10.0.0.9", &config) ==
              FORWARD_CONFIG_MISSING_FIELD,
          "an enabled declaration without a target is refused");
    CHECK(parts(true, true, "10.0.2.3", NULL, "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_MISSING_FIELD,
          "an enabled declaration without a target port is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", NULL, "10.0.0.9",
                &config) == FORWARD_CONFIG_MISSING_FIELD,
          "an enabled declaration without a listen port is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443", NULL, &config) ==
              FORWARD_CONFIG_MISSING_FIELD,
          "an enabled declaration without clients is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443", "", &config) ==
              FORWARD_CONFIG_MISSING_FIELD,
          "an empty client list is a missing field, not a deny-all rule");
}

static void test_invalid_addresses(void)
{
    struct forward_config config;
    static const char *const invalid[] = {
        "10.0.2", "10.0.2.3.4", "10.0.2.256", "10.0.2.-1",
        "010.0.2.3", "10.0.2.03", "10.0.2.", ".10.0.2.3",
        "10.0.2.a", "10.0.2.3 ", " 10.0.2.3", "10.0.2.3/24",
        "0.0.0.0", "255.255.255.255", "127.0.0.1", "224.0.0.1",
        "10.0.2.3,10.0.2.4", "10.0.2.3\n", "4294967296.0.2.3",
    };
    size_t index;

    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]);
         ++index) {
        char message[128];

        snprintf(message, sizeof(message),
                 "the target %s is refused as an address", invalid[index]);
        CHECK(parts(true, true, invalid[index], "8443", "9443",
                    "10.0.0.9", &config) ==
                  FORWARD_CONFIG_INVALID_ADDRESS,
              message);
    }
}

static void test_invalid_ports(void)
{
    struct forward_config config;

    CHECK(parts(true, true, "10.0.2.3", "0", "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_INVALID_PORT,
          "port zero is refused");
    CHECK(parts(true, true, "10.0.2.3", "65536", "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_INVALID_PORT,
          "a port above 65535 is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "0", "10.0.0.9",
                &config) == FORWARD_CONFIG_INVALID_PORT,
          "listen port zero is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "65536", "10.0.0.9",
                &config) == FORWARD_CONFIG_INVALID_PORT,
          "a listen port above 65535 is refused");
    CHECK(parts(true, true, "10.0.2.3", "8a43", "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_INVALID_PORT,
          "a port that is not decimal is refused");
    CHECK(parts(true, true, "10.0.2.3", "4294967808", "9443",
                "10.0.0.9", &config) == FORWARD_CONFIG_INVALID_PORT,
          "a port that would wrap is refused instead of truncated");
    CHECK(parts(true, true, "10.0.2.3", "8443", "443", "10.0.0.9",
                &config) == FORWARD_CONFIG_RESERVED_PORT,
          "the management serving port is refused as a listen port");
    CHECK(parts(true, true, "10.0.2.3", "443", "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_OK,
          "the target may be the HTTPS port of the declared server");
}

static void test_invalid_clients(void)
{
    struct forward_config config;

    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.1,10.0.0.2,10.0.0.3,10.0.0.4,10.0.0.5",
                &config) == FORWARD_CONFIG_TOO_MANY_CLIENTS,
          "a fifth client is refused instead of dropped");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.9,10.0.0.9", &config) ==
              FORWARD_CONFIG_DUPLICATE_CLIENT,
          "a duplicated client is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.9,", &config) ==
              FORWARD_CONFIG_INVALID_ADDRESS,
          "a trailing comma is refused, not read as an empty client");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                ",10.0.0.9", &config) ==
              FORWARD_CONFIG_INVALID_ADDRESS,
          "a leading comma is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.9,,10.0.0.8", &config) ==
              FORWARD_CONFIG_INVALID_ADDRESS,
          "an empty client between two commas is refused");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "10.0.0.0/24", &config) ==
              FORWARD_CONFIG_INVALID_ADDRESS,
          "a prefix is not a client: only exact addresses are allowed");
    CHECK(parts(true, true, "10.0.2.3", "8443", "9443",
                "127.0.0.1", &config) ==
              FORWARD_CONFIG_INVALID_ADDRESS,
          "loopback is not a local-network client");
}

static void test_validate_hand_built(void)
{
    struct forward_config config;
    uint8_t source[4];

    memset(&config, 0, sizeof(config));
    CHECK(forward_config_validate(&config) == FORWARD_CONFIG_BAD_TEXT,
          "a declaration without its version is refused");
    config.version = FORWARD_CONFIG_VERSION;
    config.declared = true;
    config.enabled = true;
    config.target[0] = 10U;
    config.target[1] = 0U;
    config.target[2] = 2U;
    config.target[3] = 3U;
    config.target_port = 8443U;
    config.listen_port = 9443U;
    CHECK(forward_config_validate(&config) ==
              FORWARD_CONFIG_MISSING_FIELD,
          "an enabled declaration with no client is refused");
    config.client_count = 1U;
    config.clients[0][0] = 10U;
    config.clients[0][1] = 0U;
    config.clients[0][2] = 0U;
    config.clients[0][3] = 9U;
    CHECK(forward_config_validate(&config) == FORWARD_CONFIG_OK,
          "the same declaration with one client validates");
    config.listen_port = FORWARD_CONFIG_RESERVED_LISTEN_PORT;
    CHECK(forward_config_validate(&config) ==
              FORWARD_CONFIG_RESERVED_PORT,
          "the hand-built rule cannot claim the serving port either");
    config.listen_port = 9443U;
    config.client_count = (uint32_t)FORWARD_CONFIG_CLIENT_MAX + 1U;
    CHECK(forward_config_validate(&config) ==
              FORWARD_CONFIG_TOO_MANY_CLIENTS,
          "a client count above the bound is refused");
    config.client_count = 0U;
    CHECK(forward_config_admit(&config, source) ==
              FORWARD_CONFIG_MISSING_FIELD,
          "an invalid rule admits nobody and says why");
    CHECK(forward_config_admit(NULL, source) == FORWARD_CONFIG_NULL,
          "no rule is a null answer, not an admission");
    CHECK(forward_config_admit(&config, NULL) == FORWARD_CONFIG_NULL,
          "no source is a null answer, not an admission");
}

static void test_render_bounds(void)
{
    struct forward_config config;
    char rendered[16];
    size_t length = 0U;

    CHECK(parts(true, true, "10.0.2.3", "8443", "9443", "10.0.0.9",
                &config) == FORWARD_CONFIG_OK,
          "the rule for the bounds check is accepted");
    CHECK(forward_config_render(&config, rendered, 8U, &length) ==
              FORWARD_CONFIG_BUFFER_TOO_SMALL,
          "a buffer that cannot hold the rule is refused, not truncated");
    CHECK(length == 0U, "a refused render publishes no length");
    CHECK(forward_config_render(NULL, rendered, sizeof(rendered),
                                &length) == FORWARD_CONFIG_NULL,
          "no rule is a null render");
    CHECK(forward_config_render(&config, NULL, 0U, &length) ==
              FORWARD_CONFIG_NULL,
          "no buffer is a null render");
}

static void test_status_strings(void)
{
    static const enum forward_config_status statuses[] = {
        FORWARD_CONFIG_OK,
        FORWARD_CONFIG_NULL,
        FORWARD_CONFIG_BAD_TEXT,
        FORWARD_CONFIG_MISSING_FIELD,
        FORWARD_CONFIG_INVALID_ADDRESS,
        FORWARD_CONFIG_INVALID_PORT,
        FORWARD_CONFIG_TOO_MANY_CLIENTS,
        FORWARD_CONFIG_DUPLICATE_CLIENT,
        FORWARD_CONFIG_RESERVED_PORT,
        FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION,
        FORWARD_CONFIG_VALUES_WITH_DISABLED,
        FORWARD_CONFIG_BUFFER_TOO_SMALL,
        FORWARD_CONFIG_ADMITTED,
        FORWARD_CONFIG_DENIED_NO_DECLARATION,
        FORWARD_CONFIG_DENIED_DISABLED,
        FORWARD_CONFIG_DENIED_NOT_ALLOWED,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        char message[96];

        snprintf(message, sizeof(message),
                 "the status %u has a name for the record",
                 (unsigned int)statuses[index]);
        CHECK(forward_config_status_string(statuses[index]) != NULL,
              message);
    }
}

int main(void)
{
    test_absent_declaration();
    test_disabled_declaration();
    test_enabled_declaration();
    test_incomplete_declaration();
    test_invalid_addresses();
    test_invalid_ports();
    test_invalid_clients();
    test_validate_hand_built();
    test_render_bounds();
    test_status_strings();
    if (failures != 0) {
        fprintf(stderr, "forward config host test: %d FAILED\n", failures);
        return 1;
    }
    printf("forward config host test: ALL OK\n");
    return 0;
}
