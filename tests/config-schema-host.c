#include "config_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CONFIG_SCHEMA_HOST_TEST

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

static void poison_output(struct appliance_config *config) {
    memset(config, 0xA5, sizeof(*config));
}

static bool output_unchanged(const struct appliance_config *config) {
    const unsigned char *bytes = (const unsigned char *)config;
    size_t index;

    for (index = 0U; index < sizeof(*config); ++index) {
        if (bytes[index] != 0xA5U) {
            return false;
        }
    }
    return true;
}

static enum config_status parse_values(
    uint32_t schema_version,
    uint32_t recovery_mode,
    uint32_t max_processes,
    uint32_t max_user_memory_pages,
    uint32_t audit_ring_capacity,
    uint32_t boot_timeout_ms,
    uint32_t restart_limit,
    uint32_t service_health_timeout_ms,
    struct appliance_config *config_out)
{
    char text[512];
    int length;

    length = snprintf(text, sizeof(text),
                      "schema_version=%u\n"
                      "recovery_mode=%u\n"
                      "max_processes=%u\n"
                      "max_user_memory_pages=%u\n"
                      "audit_ring_capacity=%u\n"
                      "boot_timeout_ms=%u\n"
                      "restart_limit=%u\n"
                      "service_health_timeout_ms=%u\n",
                      schema_version, recovery_mode, max_processes,
                      max_user_memory_pages, audit_ring_capacity,
                      boot_timeout_ms, restart_limit,
                      service_health_timeout_ms);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        return CONFIG_TOO_LONG;
    }
    return config_schema_parse(text, (size_t)length, config_out);
}

static void test_validation_and_too_long(void) {
    struct appliance_config output;
    char too_long[CONFIG_INPUT_MAX + 1U];

    memset(&output, 0, sizeof(output));
    CHECK(config_schema_parse(NULL, 0U, &output) == CONFIG_NULL,
          "schema parser rejects null input");
    CHECK(config_schema_parse("", 0U, NULL) == CONFIG_NULL,
          "schema parser rejects null output");
    CHECK(config_schema_parse(NULL, 0U, NULL) == CONFIG_NULL,
          "schema parser rejects null input and output");

    memset(too_long, 'x', sizeof(too_long));
    CHECK(config_schema_parse(too_long, sizeof(too_long), &output) ==
              CONFIG_TOO_LONG,
          "schema parser rejects input over maximum length");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "schema parser validation performs no allocation");
}

static void test_valid_schema(void) {
    struct appliance_config output;

    poison_output(&output);
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OK,
          "valid schema parses successfully");
    CHECK(output.schema_version == CONFIG_SCHEMA_VERSION &&
              output.recovery_mode == 1U &&
              output.max_processes == 32U &&
              output.max_user_memory_pages == 512U &&
              output.audit_ring_capacity == 64U &&
              output.boot_timeout_ms == 5000U &&
              output.restart_limit == 3U &&
              output.service_health_timeout_ms == 10000U,
          "valid schema publishes exact field values");
    CHECK(config_status_string(CONFIG_OK) != NULL &&
              config_status_string(CONFIG_OUT_OF_RANGE) != NULL,
          "schema status strings are present");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid schema parse performs no allocation");
}

static void test_unknown_duplicate_and_truncated(void) {
    struct appliance_config output;
    const char *unknown =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "restart_limit=3\n"
        "service_health_timeout_ms=10000\n"
        "unknown_field=1\n";
    const char *duplicate =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "restart_limit=3\n"
        "service_health_timeout_ms=10000\n"
        "max_processes=32\n";
    const char *truncated =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=\n";

    poison_output(&output);
    CHECK(config_schema_parse(unknown, strlen(unknown), &output) ==
              CONFIG_UNKNOWN_FIELD &&
              output_unchanged(&output),
          "unknown schema field fails without output publication");

    poison_output(&output);
    CHECK(config_schema_parse(duplicate, strlen(duplicate), &output) ==
              CONFIG_DUPLICATE_FIELD &&
              output_unchanged(&output),
          "duplicate schema field fails without output publication");

    poison_output(&output);
    CHECK(config_schema_parse(truncated, strlen(truncated), &output) ==
              CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "truncated schema value fails without output publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "unknown, duplicate, and truncated parsing performs no allocation");
}

static void test_malformed_lines(void) {
    struct appliance_config output;
    const char *no_delimiter =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes 32\n";
    const char *extra_delimiter =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32=64\n";
    const char *blank_line =
        "\n"
        "schema_version=1\n";
    const char *invalid_key_char =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes!=32\n";
    const char *invalid_value_char =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=3x\n";
    const char *whitespace_value =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32 \n";

    poison_output(&output);
    CHECK(config_schema_parse(no_delimiter, strlen(no_delimiter), &output) ==
              CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "line without delimiter is rejected");

    poison_output(&output);
    CHECK(config_schema_parse(extra_delimiter, strlen(extra_delimiter),
                              &output) == CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "line with extra delimiter is rejected");

    poison_output(&output);
    CHECK(config_schema_parse(blank_line, strlen(blank_line), &output) ==
              CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "blank line is rejected");

    poison_output(&output);
    CHECK(config_schema_parse(invalid_key_char, strlen(invalid_key_char),
                              &output) == CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "invalid key character is rejected");

    poison_output(&output);
    CHECK(config_schema_parse(invalid_value_char, strlen(invalid_value_char),
                              &output) == CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "invalid value character is rejected");

    poison_output(&output);
    CHECK(config_schema_parse(whitespace_value, strlen(whitespace_value),
                              &output) == CONFIG_MALFORMED_FIELD &&
              output_unchanged(&output),
          "whitespace in value is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-line parsing performs no allocation");
}

static void test_missing_fields(void) {
    struct appliance_config output;
    const char *missing_restart =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "service_health_timeout_ms=10000\n";
    const char *missing_schema =
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "restart_limit=3\n"
        "service_health_timeout_ms=10000\n";

    poison_output(&output);
    CHECK(config_schema_parse(missing_restart, strlen(missing_restart),
                              &output) == CONFIG_MISSING_FIELD &&
              output_unchanged(&output),
          "missing restart limit fails without output publication");

    poison_output(&output);
    CHECK(config_schema_parse(missing_schema, strlen(missing_schema),
                              &output) == CONFIG_MISSING_FIELD &&
              output_unchanged(&output),
          "missing schema version fails without output publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "missing-field parsing performs no allocation");
}

static void test_bad_version_and_ranges(void) {
    struct appliance_config output;

    poison_output(&output);
    CHECK(parse_values(2U, 1U, 32U, 512U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_BAD_VERSION &&
              output_unchanged(&output),
          "wrong schema version fails");

    poison_output(&output);
    CHECK(parse_values(1U, 3U, 32U, 512U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE &&
              output_unchanged(&output),
          "out-of-range recovery mode fails");

    CHECK(parse_values(1U, 1U, 0U, 512U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "zero max processes is out of range");
    CHECK(parse_values(1U, 1U, 65U, 512U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "max processes above capacity is out of range");
    CHECK(parse_values(1U, 1U, 32U, 15U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "user memory pages below minimum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 4097U, 64U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "user memory pages above maximum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 15U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "audit ring capacity below minimum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 65U, 5000U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "audit ring capacity above maximum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 999U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "boot timeout below minimum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 60001U, 3U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "boot timeout above maximum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 5000U, 17U, 10000U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "restart limit above maximum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 5000U, 3U, 999U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "service health timeout below minimum is out of range");
    CHECK(parse_values(1U, 1U, 32U, 512U, 64U, 5000U, 3U, 30001U,
                       &output) == CONFIG_OUT_OF_RANGE,
          "service health timeout above maximum is out of range");
}

static void test_boundaries_and_overflow(void) {
    struct appliance_config output;
    const char *overflow =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=4294967296\n";

    CHECK(parse_values(1U, 0U, 1U, 16U, 16U, 1000U, 0U, 1000U,
                       &output) == CONFIG_OK &&
              output.schema_version == CONFIG_SCHEMA_VERSION &&
              output.recovery_mode == 0U &&
              output.max_processes == 1U &&
              output.max_user_memory_pages == 16U &&
              output.audit_ring_capacity == 16U &&
              output.boot_timeout_ms == 1000U &&
              output.restart_limit == 0U &&
              output.service_health_timeout_ms == 1000U,
          "minimum schema boundary values succeed");
    CHECK(parse_values(1U, 2U, 64U, 4096U, 64U, 60000U, 16U, 30000U,
                       &output) == CONFIG_OK &&
              output.recovery_mode == 2U &&
              output.max_processes == 64U &&
              output.max_user_memory_pages == 4096U &&
              output.audit_ring_capacity == 64U &&
              output.boot_timeout_ms == 60000U &&
              output.restart_limit == 16U &&
              output.service_health_timeout_ms == 30000U,
          "maximum schema boundary values succeed");

    poison_output(&output);
    CHECK(config_schema_parse(overflow, strlen(overflow), &output) ==
              CONFIG_OUT_OF_RANGE &&
              output_unchanged(&output),
          "numeric overflow fails without output publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boundary and overflow parsing performs no allocation");
}

int main(void) {
    test_validation_and_too_long();
    test_valid_schema();
    test_unknown_duplicate_and_truncated();
    test_malformed_lines();
    test_missing_fields();
    test_bad_version_and_ranges();
    test_boundaries_and_overflow();

    if (failures != 0) {
        fprintf(stderr, "Configuration schema host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Configuration schema host tests: ALL OK");
    return 0;
}

#endif
