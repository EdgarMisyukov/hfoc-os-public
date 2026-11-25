/*
 * ITEM-447 host proof for the network the machine's own menu sets.
 *
 * The module under test asks the four questions, validates the answer with the
 * one IPv4 owner and renders the records; the console (proved separately)
 * performs the commit.  This proof is deliberately exhaustive on the refusals,
 * because "the operator's answer is committed or refused, never guessed" is
 * only as good as these decisions.
 */
#ifdef HFOC_LOCAL_NETWORK_SETUP_HOST_TEST

#include "local_network_setup.h"

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

/* Answer one line and return the action the caller has to take. */
static enum local_network_setup_action feed(
    struct local_network_setup *setup,
    const char *line,
    char *output,
    size_t capacity,
    size_t *length,
    struct network_ipv4_config *config,
    enum local_network_setup_status *status_out)
{
    enum local_network_setup_action action = LOCAL_NETWORK_SETUP_ACTION_NONE;

    *status_out = local_network_setup_feed(
        setup, line, line != NULL ? strlen(line) : 0U, &action,
        output, capacity, length, config);
    return action;
}

static void test_begin_and_prompts(void)
{
    struct local_network_setup setup;
    char output[LOCAL_NETWORK_SETUP_PROMPT_MAX];
    size_t length = 0U;

    memset(&setup, 0, sizeof(setup));
    CHECK(!local_network_setup_active(&setup) &&
              local_network_setup_step(&setup) ==
                  LOCAL_NETWORK_SETUP_STEP_IDLE,
          "an unstarted answer is idle");

    local_network_setup_begin(&setup);
    CHECK(local_network_setup_active(&setup) &&
              local_network_setup_step(&setup) ==
                  LOCAL_NETWORK_SETUP_STEP_MODE,
          "the answer starts on the mode question");
    CHECK(local_network_setup_prompt(
              &setup, output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output, "network mode [dhcp|static] (empty cancels): ") ==
                  0,
          "the mode question names both modes and the cancel");

    local_network_setup_abort(&setup);
    CHECK(!local_network_setup_active(&setup),
          "an aborted answer is idle again");
}

static void test_static_answer(void)
{
    struct local_network_setup setup;
    struct network_ipv4_config config;
    enum local_network_setup_status status = LOCAL_NETWORK_SETUP_OK;
    enum local_network_setup_action action;
    char output[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t length = 0U;

    local_network_setup_begin(&setup);
    action = feed(&setup, "static", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_PROMPT &&
              strcmp(output, "address a.b.c.d/prefix: ") == 0 &&
              local_network_setup_step(&setup) ==
                  LOCAL_NETWORK_SETUP_STEP_ADDRESS,
          "the mode answer asks for the address");

    action = feed(&setup, "10.0.2.15/24", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_PROMPT &&
              strcmp(output, "gateway: ") == 0,
          "the address answer asks for the gateway");

    action = feed(&setup, "10.0.2.2", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_PROMPT &&
              strcmp(output, "dns: ") == 0,
          "the gateway answer asks for the DNS");

    action = feed(&setup, "10.0.2.3", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_COMMIT_STATIC &&
              !local_network_setup_active(&setup) &&
              config.prefix_length == 24U &&
              config.address.octets[0] == 10U &&
              config.address.octets[3] == 15U &&
              config.gateway.octets[3] == 2U &&
              config.dns.octets[3] == 3U,
          "the completed answer commits the validated configuration");

    CHECK(local_network_setup_record(
              true, &config, true, "DHCP", 4U,
              output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] mode=STATIC "
                     "address=10.0.2.15/24 gateway=10.0.2.2 dns=10.0.2.3 "
                     "persisted=OK applies=reboot current=DHCP\n") == 0,
          "the record names the committed answer and when it applies, and "
          "what the interface still runs");

    CHECK(local_network_setup_record(
              true, &config, false, "STATIC address=10.0.2.15/24", 28U,
              output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strstr(output, "persisted=FAILED") != NULL &&
              strstr(output, "current=STATIC address=10.0.2.15/24") != NULL,
          "a record that could not be stored says so and still names what "
          "the interface has");
}

static void test_dhcp_answer(void)
{
    struct local_network_setup setup;
    struct network_ipv4_config config;
    enum local_network_setup_status status = LOCAL_NETWORK_SETUP_OK;
    enum local_network_setup_action action;
    char output[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t length = 0U;

    local_network_setup_begin(&setup);
    action = feed(&setup, "dhcp", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_COMMIT_DHCP &&
              !local_network_setup_active(&setup),
          "the DHCP answer commits discovery without more questions");
    CHECK(local_network_setup_record(
              false, NULL, true, "STATIC address=10.0.2.15/24", 28U,
              output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] mode=DHCP persisted=OK "
                     "applies=reboot current=STATIC address=10.0.2.15/24\n") ==
                  0,
          "the DHCP record names the mode, no address, and the address the "
          "interface still runs until the reboot");
}

static void test_cancel_and_refusals(void)
{
    struct local_network_setup setup;
    struct network_ipv4_config config;
    enum local_network_setup_status status = LOCAL_NETWORK_SETUP_OK;
    enum local_network_setup_action action;
    char output[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t length = 0U;

    /* An empty answer cancels at every step, and commits nothing. */
    local_network_setup_begin(&setup);
    action = feed(&setup, "", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_CANCEL &&
              !local_network_setup_active(&setup),
          "an empty mode answer cancels the whole answer");

    local_network_setup_begin(&setup);
    (void)feed(&setup, "static", output, sizeof(output), &length,
               &config, &status);
    action = feed(&setup, "", output, sizeof(output), &length,
                  &config, &status);
    CHECK(action == LOCAL_NETWORK_SETUP_ACTION_CANCEL &&
              !local_network_setup_active(&setup),
          "an empty address answer cancels the whole answer");

    /* A mode that is not a mode is refused, not read as the default. */
    local_network_setup_begin(&setup);
    action = feed(&setup, "ethernet", output, sizeof(output), &length,
                  &config, &status);
    CHECK(status == LOCAL_NETWORK_SETUP_OK &&
              action == LOCAL_NETWORK_SETUP_ACTION_REFUSED &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=REFUSED reason=MODE\n") == 0 &&
              !local_network_setup_active(&setup),
          "an unknown mode is refused with its reason");

    /* Each value is validated by the IPv4 owner, and the reason is its own. */
    {
        static const struct {
            const char *address;
            const char *gateway;
            const char *dns;
            const char *reason;
        } cases[] = {
            { "10.0.2.15", "10.0.2.2", "10.0.2.3",
              "NETWORK_IPV4_MALFORMED_FIELD" },
            { "10.0.2.15/33", "10.0.2.2", "10.0.2.3",
              "NETWORK_IPV4_INVALID_PREFIX" },
            { "10.0.2.15/24", "10.0.3.1", "10.0.2.3",
              "NETWORK_IPV4_INVALID_ROUTE" },
            { "10.0.2.15/24", "10.0.2.2", "not-an-address",
              "NETWORK_IPV4_INVALID_DNS" },
            { "0.0.0.0/24", "0.0.0.1", "10.0.2.3",
              "NETWORK_IPV4_INVALID_ADDRESS" },
        };
        size_t index;

        for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            local_network_setup_begin(&setup);
            (void)feed(&setup, "static", output, sizeof(output), &length,
                       &config, &status);
            (void)feed(&setup, cases[index].address, output, sizeof(output),
                       &length, &config, &status);
            (void)feed(&setup, cases[index].gateway, output, sizeof(output),
                       &length, &config, &status);
            action = feed(&setup, cases[index].dns, output, sizeof(output),
                          &length, &config, &status);
            CHECK(action == LOCAL_NETWORK_SETUP_ACTION_REFUSED &&
                      strstr(output, cases[index].reason) != NULL &&
                      !local_network_setup_active(&setup),
                  "an invalid static answer is refused with the IPv4 reason");
        }
    }

    /* A value longer than the field is refused, not truncated. */
    local_network_setup_begin(&setup);
    (void)feed(&setup, "static", output, sizeof(output), &length,
               &config, &status);
    action = feed(&setup,
                  "10.0.2.15/24-0123456789012345678901234567890123456789",
                  output, sizeof(output), &length, &config, &status);
    CHECK(action == LOCAL_NETWORK_SETUP_ACTION_REFUSED &&
              strstr(output, "ADDRESS_TOO_LONG") != NULL,
          "an over-long address is refused instead of truncated");
}

static void test_records_and_status_strings(void)
{
    char output[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t length = 0U;

    CHECK(local_network_setup_refusal(
              "NETWORK_IPV4_INVALID_PREFIX", output, sizeof(output),
              &length) == LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=REFUSED "
                     "reason=NETWORK_IPV4_INVALID_PREFIX\n") == 0,
          "the refusal record carries the reason");
    CHECK(local_network_setup_unavailable(
              output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=UNAVAILABLE "
                     "reason=NO_RECORD_OWNER\n") == 0,
          "an unbound record owner is published, not hidden");
    CHECK(local_network_setup_offer(
              "DHCP", 4U, output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=OFFERED current=DHCP\n") == 0,
          "the offer record names what the interface has before the "
          "questions");
    CHECK(local_network_setup_offer(
              "STATIC address=10.0.2.15/24", 28U, output, sizeof(output),
              &length) == LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=OFFERED "
                     "current=STATIC address=10.0.2.15/24\n") == 0,
          "the offer record names a static address");
    CHECK(local_network_setup_offer(
              NULL, 0U, output, sizeof(output), &length) ==
              LOCAL_NETWORK_SETUP_OK &&
              strcmp(output,
                     "[PRODUCTION-NET-SET] state=OFFERED "
                     "current=UNAVAILABLE\n") == 0,
          "an unknown current configuration is published as UNAVAILABLE, "
          "not guessed");
    CHECK(local_network_setup_record(
              false, NULL, false, NULL, 0U, NULL, 0U, &length) ==
              LOCAL_NETWORK_SETUP_NULL &&
              local_network_setup_offer(
                  "DHCP", 4U, NULL, 0U, &length) ==
                  LOCAL_NETWORK_SETUP_NULL &&
              local_network_setup_prompt(NULL, output, sizeof(output),
                                         &length) ==
                  LOCAL_NETWORK_SETUP_NULL &&
              local_network_setup_feed(
                  NULL, "dhcp", 4U, NULL, output, sizeof(output), &length,
                  NULL) == LOCAL_NETWORK_SETUP_NULL,
          "null inputs are refused");
    CHECK(strcmp(local_network_setup_status_string(
                     LOCAL_NETWORK_SETUP_OK),
                 "LOCAL_NETWORK_SETUP_OK") == 0 &&
              local_network_setup_status_string(
                  (enum local_network_setup_status)99) == NULL,
          "the status strings are exact");
}

int main(void)
{
    test_begin_and_prompts();
    test_static_answer();
    test_dhcp_answer();
    test_cancel_and_refusals();
    test_records_and_status_strings();
    if (failures != 0) {
        fprintf(stderr, "Local network setup host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("Local network setup host tests: ALL OK\n");
    return 0;
}

#else

typedef int local_network_setup_host_unused;

#endif
