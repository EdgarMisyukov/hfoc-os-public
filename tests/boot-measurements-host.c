#ifdef HFOC_BOOT_MEASUREMENTS_HOST_TEST

#include "boot_measurements.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

static void boot_measurements_host_reset_and_extend(void)
{
    struct boot_measurements measurements;
    struct sha256 hash;
    unsigned char expected[SHA256_DIGEST_SIZE];
    unsigned char value[SHA256_DIGEST_SIZE];
    unsigned char read_back[SHA256_DIGEST_SIZE];
    const unsigned char component[] = "boot-payload";
    const unsigned char digest_a[SHA256_DIGEST_SIZE] = { 0xA5U };

    CHECK(boot_measurements_reset(NULL) == BOOT_MEASUREMENTS_NULL,
          "null reset is refused");
    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "reset publishes an empty register set");
    CHECK(measurements.version == BOOT_MEASUREMENTS_VERSION,
          "reset records the register-set version");
    CHECK(!boot_measurements_is_published(&measurements),
          "a reset register set is not published");
    CHECK(boot_measurements_register_count(&measurements) ==
              BOOT_MEASUREMENTS_REGISTER_COUNT,
          "the register count is bounded and fixed");

    CHECK(boot_measurements_measure(&measurements, 1U, 1U, component,
                                    sizeof(component) - 1U) ==
              BOOT_MEASUREMENTS_OK,
          "a component measures into a register");
    CHECK(boot_measurements_log_count(&measurements) == 1U,
          "a measured component is logged");
    CHECK(boot_measurements_log_entry(&measurements, 0U) != NULL &&
              boot_measurements_log_entry(&measurements, 0U)
                      ->component_kind == 1U &&
              boot_measurements_log_entry(&measurements, 0U)
                      ->component_length == sizeof(component) - 1U,
          "the log entry carries the component identity and length");

    /* measure() extends with SHA-256(data), so the reference chain is
     * register = SHA-256(zero-register || SHA-256(component)). */
    CHECK(sha256_compute(component, sizeof(component) - 1U, value) ==
              SHA256_OK,
          "the component digest computes");
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char[SHA256_DIGEST_SIZE]){ 0 },
                  SHA256_DIGEST_SIZE);
    sha256_update(&hash, value, SHA256_DIGEST_SIZE);
    CHECK(sha256_final(&hash, expected) == SHA256_OK,
          "the reference digest computes");
    CHECK(boot_measurements_extend(&measurements, 1U, digest_a) ==
              BOOT_MEASUREMENTS_OK,
          "a caller-supplied digest extends a register");

    sha256_init(&hash);
    sha256_update(&hash, expected, SHA256_DIGEST_SIZE);
    sha256_update(&hash, digest_a, SHA256_DIGEST_SIZE);
    CHECK(sha256_final(&hash, value) == SHA256_OK,
          "the extended reference digest computes");

    CHECK(boot_measurements_read(&measurements, 1U, read_back) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "registers are not readable before publication");
    CHECK(boot_measurements_aggregate(&measurements, read_back) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "the aggregate is not readable before publication");
    CHECK(boot_measurements_publish(&measurements) ==
              BOOT_MEASUREMENTS_OK,
          "the register set publishes");
    CHECK(boot_measurements_is_published(&measurements),
          "the published register set is readable");
    CHECK(boot_measurements_read(&measurements, 1U, read_back) ==
              BOOT_MEASUREMENTS_OK &&
              memcmp(read_back, value, SHA256_DIGEST_SIZE) == 0,
          "extend binds the register to its whole history");
    CHECK(boot_measurements_read(&measurements, 0U, read_back) ==
              BOOT_MEASUREMENTS_OK,
          "an untouched register reads back");
    CHECK(boot_measurements_read(&measurements,
                                 BOOT_MEASUREMENTS_REGISTER_COUNT,
                                 read_back) ==
              BOOT_MEASUREMENTS_BAD_INDEX,
          "an out-of-range register is refused");
    CHECK(boot_measurements_read(NULL, 0U, read_back) ==
              BOOT_MEASUREMENTS_NULL,
          "a null register set is refused");
    CHECK(boot_measurements_read(&measurements, 0U, NULL) ==
              BOOT_MEASUREMENTS_NULL,
          "a null output buffer is refused");
    CHECK(boot_measurements_measure(&measurements, 0U, 2U, component,
                                    sizeof(component) - 1U) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "measuring into a published register set is refused");
    CHECK(boot_measurements_extend(&measurements, 0U, digest_a) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "extending a published register set is refused");
    CHECK(boot_measurements_publish(&measurements) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "publishing twice is refused");
}

static void boot_measurements_host_failure_state(void)
{
    struct boot_measurements measurements;
    struct boot_measurements other;
    unsigned char aggregate[SHA256_DIGEST_SIZE];
    unsigned char other_aggregate[SHA256_DIGEST_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE] = { 0 };
    unsigned char component[4] = { 1U, 2U, 3U, 4U };
    uint32_t index;

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "reset for the failure fixture");
    CHECK(boot_measurements_measure(&measurements, 0U, 0U, NULL, 1U) ==
              BOOT_MEASUREMENTS_NULL,
          "measuring null bytes is refused");
    CHECK(!boot_measurements_failed(&measurements),
          "a rejected call does not fail the register set");

    for (index = 0U; index < BOOT_MEASUREMENTS_LOG_CAPACITY; ++index) {
        CHECK(boot_measurements_measure(&measurements, 0U, index, component,
                                        sizeof(component)) ==
                  BOOT_MEASUREMENTS_OK,
              "the log fills to its declared capacity");
    }
    CHECK(boot_measurements_measure(&measurements, 0U, 0U, component,
                                    sizeof(component)) ==
              BOOT_MEASUREMENTS_CAPACITY,
          "log overflow is refused");
    CHECK(boot_measurements_failed(&measurements),
          "a log overflow marks the register set failed");
    CHECK(!boot_measurements_is_published(&measurements),
          "a failed register set is never published");
    CHECK(boot_measurements_publish(&measurements) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "a failed register set cannot publish");
    CHECK(boot_measurements_read(&measurements, 0U, digest) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "a failed register set is not readable");

    CHECK(boot_measurements_reset(&other) == BOOT_MEASUREMENTS_OK,
          "reset for the aggregate fixture");
    CHECK(boot_measurements_measure(&other, 0U, 0U, component,
                                    sizeof(component)) ==
              BOOT_MEASUREMENTS_OK,
          "the aggregate fixture measures a component");
    CHECK(boot_measurements_publish(&other) == BOOT_MEASUREMENTS_OK,
          "the aggregate fixture publishes");
    CHECK(boot_measurements_aggregate(&other, other_aggregate) ==
              BOOT_MEASUREMENTS_OK,
          "the aggregate is readable after publication");

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "reset for the aggregate comparison");
    CHECK(boot_measurements_measure(&measurements, 0U, 0U, component,
                                    sizeof(component) - 1U) ==
              BOOT_MEASUREMENTS_OK,
          "a different component measures");
    CHECK(boot_measurements_publish(&measurements) == BOOT_MEASUREMENTS_OK,
          "the different component publishes");
    CHECK(boot_measurements_aggregate(&measurements, aggregate) ==
              BOOT_MEASUREMENTS_OK,
          "the different aggregate is readable");
    CHECK(memcmp(aggregate, other_aggregate, SHA256_DIGEST_SIZE) != 0,
          "a different measurement yields a different aggregate");

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "reset for the canonical aggregate");
    CHECK(boot_measurements_measure(&measurements, 0U, 0U, component,
                                    sizeof(component) - 1U) ==
              BOOT_MEASUREMENTS_OK,
          "the canonical component measures");
    CHECK(boot_measurements_publish(&measurements) == BOOT_MEASUREMENTS_OK,
          "the canonical register set publishes");
    CHECK(boot_measurements_aggregate(&measurements, aggregate) ==
              BOOT_MEASUREMENTS_OK,
          "the canonical aggregate is readable");
    CHECK(boot_measurements_fail(&measurements) == BOOT_MEASUREMENTS_OK,
          "a published register set can be failed explicitly");
    CHECK(boot_measurements_failed(&measurements) &&
              !boot_measurements_is_published(&measurements),
          "an explicitly failed register set is untrustworthy");
    CHECK(boot_measurements_aggregate(&measurements, aggregate) ==
              BOOT_MEASUREMENTS_BAD_STATE,
          "a failed register set publishes no aggregate");

    CHECK(strcmp(boot_measurements_status_string(BOOT_MEASUREMENTS_OK),
                 "OK") == 0 &&
              strcmp(boot_measurements_status_string(
                         BOOT_MEASUREMENTS_BAD_INDEX),
                     "BAD_INDEX") == 0,
          "status strings are exact");
}

int main(void)
{
    boot_measurements_host_reset_and_extend();
    boot_measurements_host_failure_state();

    if (failures != 0) {
        fprintf(stderr, "Boot measurements host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Boot measurements host tests: ALL OK (%d checks)\n", checks);
    return 0;
}

#else
int boot_measurements_host_translation_unit_marker;
#endif
