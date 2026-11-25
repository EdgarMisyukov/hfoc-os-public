#include "appliance_config_validation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool hostname_is(const struct appliance_config_validation_state *state,
                        const char *expected)
{
    char output[
        APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX + 1U];
    size_t length = 0U;
    size_t expected_length = strlen(expected);

    return appliance_config_validation_hostname(
               state, output, sizeof(output), &length) ==
               APPLIANCE_CONFIG_VALIDATION_OK &&
           length == expected_length &&
           memcmp(output, expected, expected_length) == 0;
}

static bool language_is(const struct appliance_config_validation_state *state,
                        enum appliance_language expected)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_COUNT;

    return appliance_config_validation_language(state, &language) ==
               APPLIANCE_CONFIG_VALIDATION_OK &&
           language == expected;
}

static void test_status_strings(void)
{
    static const enum appliance_config_validation_status statuses[] = {
        APPLIANCE_CONFIG_VALIDATION_OK,
        APPLIANCE_CONFIG_VALIDATION_NULL,
        APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
        APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
        APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION,
        APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY,
        APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
        APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL,
        APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE,
        APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT,
    };
    static const char *expected[] = {
        "APPLIANCE_CONFIG_VALIDATION_OK",
        "APPLIANCE_CONFIG_VALIDATION_NULL",
        "APPLIANCE_CONFIG_VALIDATION_BAD_STATE",
        "APPLIANCE_CONFIG_VALIDATION_BAD_RECORD",
        "APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION",
        "APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY",
        "APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME",
        "APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL",
        "APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE",
        "APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT",
    };
    size_t index;

    CHECK(sizeof(statuses) / sizeof(statuses[0]) ==
              sizeof(expected) / sizeof(expected[0]),
          "validation status table is complete");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(strcmp(appliance_config_validation_status_string(
                         statuses[index]),
                     expected[index]) == 0,
              "validation status string is exact");
    }
    CHECK(appliance_config_validation_status_string(
              (enum appliance_config_validation_status)10U) == NULL,
          "validation rejects undefined status string");
}

static void test_valid_record(void)
{
    static const char record[] = "v1:system.hostname=node-304\n";
    struct appliance_config_validation_state state;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              record, sizeof(record) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_source(&state) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              hostname_is(&state, "node-304"),
          "valid configuration record publishes committed hostname");
    CHECK(language_is(&state, APPLIANCE_LANGUAGE_EN),
          "hostname-only record reads as the default language");
}

/*
 * ITEM-509 increment 2: the same record carries the language as an optional
 * second line; a record of the previous format keeps reading, and a language
 * the catalog does not name is refused by name.
 */
static void test_language_record(void)
{
    static const char russian[] =
        "v1:system.hostname=node-304\nv1:system.language=ru\n";
    static const char english[] =
        "v1:system.hostname=node-304\nv1:system.language=en\n";
    static const char bad_language[] =
        "v1:system.hostname=node-304\nv1:system.language=de\n";
    static const char uppercase[] =
        "v1:system.hostname=node-304\nv1:system.language=EN\n";
    static const char empty_language[] =
        "v1:system.hostname=node-304\nv1:system.language=\n";
    static const char unknown_second[] =
        "v1:system.hostname=node-304\nv1:system.other=ru\n";
    static const char third_line[] =
        "v1:system.hostname=node-304\nv1:system.language=ru\n"
        "v1:system.language=en\n";
    static const char second_version[] =
        "v1:system.hostname=node-304\nv2:system.language=ru\n";
    struct appliance_config_validation_state state;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              russian, sizeof(russian) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              hostname_is(&state, "node-304") &&
              language_is(&state, APPLIANCE_LANGUAGE_RU),
          "language line publishes committed Russian");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              english, sizeof(english) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              language_is(&state, APPLIANCE_LANGUAGE_EN),
          "explicit English language line is accepted");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              bad_language, sizeof(bad_language) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE,
          "validation refuses a language the catalog does not name");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              uppercase, sizeof(uppercase) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE,
          "validation refuses an upper-case language code");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              empty_language, sizeof(empty_language) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE,
          "validation refuses an empty language value");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              unknown_second, sizeof(unknown_second) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY,
          "validation refuses an unknown second key");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              third_line, sizeof(third_line) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
          "validation refuses a third record line");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              second_version, sizeof(second_version) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION,
          "validation refuses an unsupported second-line version");
}

static void test_language_publication(void)
{
    struct appliance_config_validation_state state;
    enum appliance_language language = APPLIANCE_LANGUAGE_COUNT;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_language(&state, &language) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
          "language getter refuses an unpublished state");
    CHECK(appliance_config_validation_publish_language(
              NULL, APPLIANCE_LANGUAGE_RU,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "language publication rejects null state");
    CHECK(appliance_config_validation_publish_language(
              &state, APPLIANCE_LANGUAGE_RU,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
          "language publication rejects a default source");
    CHECK(appliance_config_validation_publish_language(
              &state, (enum appliance_language)7,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE,
          "language publication rejects an unknown language");
    CHECK(appliance_config_validation_publish_language(
              &state, APPLIANCE_LANGUAGE_RU,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              language_is(&state, APPLIANCE_LANGUAGE_RU),
          "language publication commits a known language");
}

static void test_invalid_records(void)
{
    static const char bad_version[] = "v2:system.hostname=node-304\n";
    static const char unknown_key[] = "v1:system.other=node-304\n";
    static const char invalid_value[] = "v1:system.hostname=bad_name\n";
    static const char missing_newline[] = "v1:system.hostname=node-304";
    static const char trailing_bytes[] =
        "v1:system.hostname=node-304\nextra\n";
    static const char empty_record[] = "";
    struct appliance_config_validation_state state;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              bad_version, sizeof(bad_version) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION,
          "validation rejects unsupported version");
    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              unknown_key, sizeof(unknown_key) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY,
          "validation rejects unknown configuration key");
    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              invalid_value, sizeof(invalid_value) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects invalid hostname record");
    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              missing_newline, sizeof(missing_newline) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
          "validation rejects missing trailing newline");
    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              trailing_bytes, sizeof(trailing_bytes) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
          "validation rejects extra record bytes");
    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              empty_record, sizeof(empty_record) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
          "validation rejects empty record");
}

static void test_validate_hostname(void)
{
    char too_long[APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX + 2U];

    memset(too_long, 'a', sizeof(too_long));
    CHECK(appliance_config_validation_validate_hostname(
              "node-304", 8U) == APPLIANCE_CONFIG_VALIDATION_OK,
          "validation accepts bounded DNS hostname");
    CHECK(appliance_config_validation_validate_hostname(
              "", 0U) == APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects empty hostname");
    CHECK(appliance_config_validation_validate_hostname(
              "-node", 5U) == APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects leading hyphen");
    CHECK(appliance_config_validation_validate_hostname(
              "node-", 5U) == APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects trailing hyphen");
    CHECK(appliance_config_validation_validate_hostname(
              "bad_name", 8U) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects underscore hostname");
    CHECK(appliance_config_validation_validate_hostname(
              too_long, sizeof(too_long)) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME,
          "validation rejects over-limit hostname");
    CHECK(appliance_config_validation_validate_hostname(
              NULL, 0U) == APPLIANCE_CONFIG_VALIDATION_NULL,
          "validation rejects null hostname");
}

/*
 * ITEM-516: the third key.  `open` publishes the policy, an absent key means
 * the default closed gate, a `closed` value is legal for a record another
 * writer produced, and any other value is refused by name.
 */
static void test_panel_after_boot_record(void)
{
    static const char open_record[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=open\n";
    static const char closed_record[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=closed\n";
    static const char open_with_language[] =
        "v1:system.hostname=node-304\n"
        "v1:system.language=ru\n"
        "v1:security.panel.after-boot=open\n";
    static const char language_after_panel[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=open\n"
        "v1:system.language=ru\n";
    static const char bad_value[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=maybe\n";
    static const char empty_value[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=\n";
    static const char uppercase_value[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=OPEN\n";
    static const char repeated[] =
        "v1:system.hostname=node-304\n"
        "v1:security.panel.after-boot=open\n"
        "v1:security.panel.after-boot=open\n";
    struct appliance_config_validation_state state;
    bool open = true;
    bool open_default = true;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              open_record, sizeof(open_record) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_panel_after_boot(&state, &open) ==
                  APPLIANCE_CONFIG_VALIDATION_OK &&
              open == true && state.panel_after_boot_present,
          "the open record publishes the enabled policy");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              closed_record, sizeof(closed_record) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_panel_after_boot(&state, &open) ==
                  APPLIANCE_CONFIG_VALIDATION_OK &&
              open == false && state.panel_after_boot_present,
          "the explicit closed record publishes the default policy");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              "v1:system.hostname=node-304\n",
              sizeof("v1:system.hostname=node-304\n") - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_panel_after_boot(
                  &state, &open_default) == APPLIANCE_CONFIG_VALIDATION_OK &&
              open_default == false && !state.panel_after_boot_present,
          "a record without the key reads as the default closed gate");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              open_with_language, sizeof(open_with_language) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              language_is(&state, APPLIANCE_LANGUAGE_RU) &&
              appliance_config_validation_panel_after_boot(&state, &open) ==
                  APPLIANCE_CONFIG_VALIDATION_OK &&
              open == true && state.panel_after_boot_present,
          "language and policy share one record");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              language_after_panel, sizeof(language_after_panel) - 1U,
              &state) == APPLIANCE_CONFIG_VALIDATION_OK &&
              language_is(&state, APPLIANCE_LANGUAGE_RU) &&
              state.panel_after_boot_open,
          "the two optional keys are accepted in either order");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              bad_value, sizeof(bad_value) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT,
          "an unknown policy value is refused by name");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              empty_value, sizeof(empty_value) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT,
          "an empty policy value is refused by name");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              uppercase_value, sizeof(uppercase_value) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT,
          "an upper-case policy value is refused by name");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              repeated, sizeof(repeated) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_RECORD,
          "a repeated policy key is a malformed record");
}

static void test_panel_after_boot_publication(void)
{
    struct appliance_config_validation_state state;
    bool open = true;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_panel_after_boot(&state, &open) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
          "the policy getter refuses an unpublished state");
    CHECK(appliance_config_validation_publish_panel_after_boot(
              NULL, true, APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "policy publication rejects a null state");
    CHECK(appliance_config_validation_publish_panel_after_boot(
              &state, true,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
          "policy publication rejects a default source");
    CHECK(appliance_config_validation_publish_panel_after_boot(
              &state, true,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_panel_after_boot(&state, &open) ==
                  APPLIANCE_CONFIG_VALIDATION_OK &&
              open == true,
          "policy publication commits the enabled setting");
    CHECK(strcmp(appliance_config_validation_panel_after_boot_value(true),
                 "open") == 0 &&
              strcmp(appliance_config_validation_panel_after_boot_value(false),
                     "closed") == 0,
          "the policy value names the record bytes exactly");
    CHECK(appliance_config_validation_validate_panel_after_boot(
              "open", 4U, &open) == APPLIANCE_CONFIG_VALIDATION_OK &&
              open == true,
          "the policy value parser accepts open");
    CHECK(appliance_config_validation_validate_panel_after_boot(
              "closed", 6U, &open) == APPLIANCE_CONFIG_VALIDATION_OK &&
              open == false,
          "the policy value parser accepts closed");
    CHECK(appliance_config_validation_validate_panel_after_boot(
              NULL, 0U, &open) == APPLIANCE_CONFIG_VALIDATION_NULL,
          "the policy value parser rejects null input");
}

static void test_default_publication(void)
{
    struct appliance_config_validation_state state;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_publish_default(
              &state,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_source(&state) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
              hostname_is(
                  &state,
                  APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME),
          "missing configuration publishes safe default");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_publish_default(
              &state,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID) ==
              APPLIANCE_CONFIG_VALIDATION_OK &&
              appliance_config_validation_source(&state) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
              hostname_is(
                  &state,
                  APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME),
          "invalid configuration publishes safe default");

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_publish_default(
              &state,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) ==
              APPLIANCE_CONFIG_VALIDATION_BAD_STATE,
          "default publication rejects committed source");
}

static void test_publication_state_errors(void)
{
    static const char record[] = "v1:system.hostname=node-304\n";
    struct appliance_config_validation_state state;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_validate_record(
              NULL, sizeof(record) - 1U, &state) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "validation rejects null record");
    CHECK(appliance_config_validation_validate_record(
              record, sizeof(record) - 1U, NULL) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "validation rejects null state");
    CHECK(appliance_config_validation_publish_default(
              NULL,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "default publication rejects null state");
    CHECK(appliance_config_validation_source(NULL) ==
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
          "source getter rejects null state");
    CHECK(appliance_config_validation_hostname(
              &state, NULL, 0U, NULL) ==
              APPLIANCE_CONFIG_VALIDATION_NULL,
          "hostname getter rejects null arguments");
}

static void test_hostname_copy_bounds(void)
{
    struct appliance_config_validation_state state;
    char small[1] = {'K'};
    size_t length = 99U;

    zero_bytes(&state, sizeof(state));
    CHECK(appliance_config_validation_publish_default(
              &state,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING) ==
              APPLIANCE_CONFIG_VALIDATION_OK,
          "hostname bounds fixture publishes default");
    CHECK(appliance_config_validation_hostname(
              &state, small, sizeof(small), &length) ==
              APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL &&
              small[0] == 'K' && length == 99U,
          "validation hostname copy rejects small buffer unchanged");
}

int main(void)
{
    test_status_strings();
    test_valid_record();
    test_language_record();
    test_language_publication();
    test_invalid_records();
    test_validate_hostname();
    test_default_publication();
    test_panel_after_boot_record();
    test_panel_after_boot_publication();
    test_publication_state_errors();
    test_hostname_copy_bounds();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "configuration validation performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance configuration validation host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance configuration validation host tests: ALL OK");
    return 0;
}
