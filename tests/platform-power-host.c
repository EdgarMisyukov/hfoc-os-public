#include "platform_power.h"

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

/*
 * ITEM-418: what the machine is asked to do, and where the sleep type comes
 * from.
 *
 * A reboot is the reset chain and needs nothing from the firmware, so it is a
 * plan on every platform.  A power off is ACPI S5 and needs both the PM1
 * control block and the `\_S5` sleep type: a platform that declares either
 * without the other is refused, because a sleep type this kernel guessed is a
 * value written into a register whose meaning nobody stated.
 */

/* `Name (_S5, Package (2) { a, b })` with byte-encoded elements. */
static size_t build_s5_bytes(
    unsigned char *out, size_t capacity,
    unsigned char a, unsigned char b)
{
    static const unsigned char prefix[4U] = {'D', 'S', 'D', 'T'};

    if (capacity < 16U) {
        return 0U;
    }
    memcpy(out, prefix, sizeof(prefix));
    out[4U] = 0x08U; /* NameOp */
    out[5U] = 0x5FU;
    out[6U] = 0x53U;
    out[7U] = 0x35U;
    out[8U] = 0x5FU;
    out[9U] = 0x12U; /* PackageOp */
    out[10U] = 0x05U; /* PkgLength: NumElements + two 2-byte elements */
    out[11U] = 0x02U; /* NumElements */
    out[12U] = 0x0AU; /* BytePrefix */
    out[13U] = a;
    out[14U] = 0x0AU;
    out[15U] = b;
    return 16U;
}

/* `Name (_S5, Package (2) { dword a, dword b })`. */
static size_t build_s5_dwords(
    unsigned char *out, size_t capacity, uint32_t a, uint32_t b)
{
    if (capacity < 20U) {
        return 0U;
    }
    memset(out, 0, 20U);
    out[0U] = 0x08U;
    out[1U] = 0x5FU;
    out[2U] = 0x53U;
    out[3U] = 0x35U;
    out[4U] = 0x5FU;
    out[5U] = 0x12U;
    out[6U] = 0x0BU; /* 1 + 5 + 5 */
    out[7U] = 0x02U;
    out[8U] = 0x0CU;
    out[9U] = (unsigned char)(a & 0xFFU);
    out[10U] = (unsigned char)((a >> 8U) & 0xFFU);
    out[11U] = (unsigned char)((a >> 16U) & 0xFFU);
    out[12U] = (unsigned char)((a >> 24U) & 0xFFU);
    out[13U] = 0x0CU;
    out[14U] = (unsigned char)(b & 0xFFU);
    out[15U] = (unsigned char)((b >> 8U) & 0xFFU);
    out[16U] = (unsigned char)((b >> 16U) & 0xFFU);
    out[17U] = (unsigned char)((b >> 24U) & 0xFFU);
    return 20U;
}

static void test_sleep_type_walk(void)
{
    unsigned char dsdt[64U];
    uint16_t a = 0U;
    uint16_t b = 0U;
    uint32_t offset = 0U;
    size_t length;

    memset(dsdt, 0xAA, sizeof(dsdt));
    length = build_s5_bytes(dsdt, sizeof(dsdt), 5U, 5U);
    CHECK(length != 0U &&
              platform_power_sleep_type_find(
                  dsdt, (uint32_t)length, &a, &b, &offset) &&
              a == 5U && b == 5U,
          "a byte-encoded \\_S5 package is read as its two sleep types");
    CHECK(offset == 4U,
          "the walk reports the offset it read the object from");

    memset(dsdt, 0x11, sizeof(dsdt));
    length = build_s5_dwords(dsdt, sizeof(dsdt), 7U, 3U);
    CHECK(length != 0U &&
              platform_power_sleep_type_find(
                  dsdt, (uint32_t)length, &a, &b, &offset) &&
              a == 7U && b == 3U,
          "a dword-encoded \\_S5 package is read as well");

    /* No \_S5 at all: the walk reports nothing rather than a guess. */
    memset(dsdt, 0x22, sizeof(dsdt));
    CHECK(!platform_power_sleep_type_find(
              dsdt, (uint32_t)sizeof(dsdt), &a, &b, &offset),
          "a DSDT without \\_S5 leaves the sleep type undeclared");

    /* The name without the package: not a declaration this kernel reads. */
    memset(dsdt, 0, sizeof(dsdt));
    dsdt[0U] = 0x08U;
    dsdt[1U] = 0x5FU;
    dsdt[2U] = 0x53U;
    dsdt[3U] = 0x35U;
    dsdt[4U] = 0x5FU;
    dsdt[5U] = 0x00U; /* a Zero, where the package has to be */
    CHECK(!platform_power_sleep_type_find(
              dsdt, (uint32_t)sizeof(dsdt), &a, &b, &offset),
          "a \\_S5 that is not a package is refused");

    /*
     * The four-element package ACPI 1.0-era firmware emits, and that QEMU's
     * own DSDT is: `Package (4) { a, b, 0, 0 }`.  The first two elements are
     * the sleep types; the rest are what the older format wrote there.
     */
    memset(dsdt, 0, sizeof(dsdt));
    dsdt[0U] = 0x08U;
    dsdt[1U] = 0x5FU;
    dsdt[2U] = 0x53U;
    dsdt[3U] = 0x35U;
    dsdt[4U] = 0x5FU;
    dsdt[5U] = 0x12U;
    dsdt[6U] = 0x06U;
    dsdt[7U] = 0x04U; /* four elements */
    dsdt[8U] = 0x00U; /* Zero: the PM1a sleep type QEMU declares */
    dsdt[9U] = 0x00U;
    dsdt[10U] = 0x00U;
    dsdt[11U] = 0x00U;
    CHECK(platform_power_sleep_type_find(
              dsdt, (uint32_t)sizeof(dsdt), &a, &b, &offset) &&
              a == 0U && b == 0U,
          "the four-element \\_S5 package reads as its first two types");

    /* A package longer than that is refused rather than guessed at. */
    memset(dsdt, 0, sizeof(dsdt));
    dsdt[0U] = 0x08U;
    dsdt[1U] = 0x5FU;
    dsdt[2U] = 0x53U;
    dsdt[3U] = 0x35U;
    dsdt[4U] = 0x5FU;
    dsdt[5U] = 0x12U;
    dsdt[6U] = 0x07U;
    dsdt[7U] = 0x05U; /* five elements */
    dsdt[8U] = 0x00U;
    dsdt[9U] = 0x00U;
    dsdt[10U] = 0x00U;
    dsdt[11U] = 0x00U;
    dsdt[12U] = 0x00U;
    CHECK(!platform_power_sleep_type_find(
              dsdt, (uint32_t)sizeof(dsdt), &a, &b, &offset),
          "a \\_S5 package this kernel does not know is refused");

    /* An element that is not an integer constant. */
    memset(dsdt, 0, sizeof(dsdt));
    dsdt[0U] = 0x08U;
    dsdt[1U] = 0x5FU;
    dsdt[2U] = 0x53U;
    dsdt[3U] = 0x35U;
    dsdt[4U] = 0x5FU;
    dsdt[5U] = 0x12U;
    dsdt[6U] = 0x05U;
    dsdt[7U] = 0x02U;
    dsdt[8U] = 0x5BU; /* a name, not a constant */
    CHECK(!platform_power_sleep_type_find(
              dsdt, (uint32_t)sizeof(dsdt), &a, &b, &offset),
          "a \\_S5 whose element is not a constant is refused");
}

static void power_environment(
    struct platform_power_environment *out,
    bool acpi, bool pm1a, bool pm1b, bool dsdt, bool sleep)
{
    memset(out, 0, sizeof(*out));
    out->acpi_present = acpi;
    out->pm1a_declared = pm1a;
    out->pm1b_declared = pm1b;
    out->pm1a_control = pm1a ? 0x0604U : 0U;
    out->pm1b_control = pm1b ? 0x0605U : 0U;
    out->dsdt_present = dsdt;
    out->sleep_type_declared = sleep;
    out->sleep_type_a = 5U;
    out->sleep_type_b = 4U;
    out->sleep_type_offset = 0x1234U;
}

static void test_plan_decisions(void)
{
    struct platform_power_environment environment;
    struct platform_power_plan plan;

    /* A reboot needs nothing from the platform. */
    power_environment(&environment, false, false, false, false, false);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_REBOOT, &environment, &plan) ==
              PLATFORM_POWER_OK &&
              plan.mechanism == PLATFORM_POWER_MECHANISM_RESET_CHAIN,
          "a reboot is a plan on a platform that declares no ACPI at all");

    /* A shutdown needs both the register and the sleep type. */
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_NO_MECHANISM,
          "a shutdown without ACPI is refused, not guessed");
    power_environment(&environment, true, true, false, true, false);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_NO_MECHANISM,
          "a shutdown without a declared sleep type is refused");
    power_environment(&environment, true, true, false, false, true);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_NO_MECHANISM,
          "a shutdown whose DSDT is missing is refused");
    power_environment(&environment, true, false, false, true, true);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_NO_MECHANISM,
          "a shutdown without a PM1a control block is refused");

    /* And with both, the plan carries what the platform declared. */
    power_environment(&environment, true, true, false, true, true);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_OK &&
              plan.mechanism == PLATFORM_POWER_MECHANISM_ACPI_S5 &&
              plan.pm1a_control == 0x0604U && !plan.pm1b_declared &&
              plan.sleep_type_a == 5U && plan.sleep_type_b == 5U,
          "a declared platform plans one PM1a write with its sleep type");
    power_environment(&environment, true, true, true, true, true);
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_SHUTDOWN, &environment, &plan) ==
              PLATFORM_POWER_OK && plan.pm1b_declared &&
              plan.pm1b_control == 0x0605U && plan.sleep_type_b == 4U,
          "a second PM1 block keeps its own sleep type");

    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_NONE, &environment, &plan) ==
              PLATFORM_POWER_BAD_ACTION,
          "an action this module does not perform is refused");
    CHECK(platform_power_plan_build(
              PLATFORM_POWER_ACTION_REBOOT, NULL, &plan) ==
              PLATFORM_POWER_NULL &&
              platform_power_plan_build(
                  PLATFORM_POWER_ACTION_REBOOT, &environment, NULL) ==
                  PLATFORM_POWER_NULL,
          "a plan needs both an environment and a destination");
}

static void test_vocabulary(void)
{
    CHECK(strcmp(platform_power_action_string(
                     PLATFORM_POWER_ACTION_REBOOT), "REBOOT") == 0 &&
              strcmp(platform_power_action_string(
                         PLATFORM_POWER_ACTION_SHUTDOWN), "SHUTDOWN") == 0 &&
              strcmp(platform_power_action_string(
                         PLATFORM_POWER_ACTION_NONE), "NONE") == 0,
          "the action vocabulary is exact");
    CHECK(strcmp(platform_power_mechanism_string(
                     PLATFORM_POWER_MECHANISM_RESET_CHAIN),
                 "RESET_CHAIN") == 0 &&
              strcmp(platform_power_mechanism_string(
                         PLATFORM_POWER_MECHANISM_ACPI_S5),
                     "ACPI_S5") == 0,
          "the mechanism vocabulary is exact");
    CHECK(strcmp(platform_power_status_string(
                     PLATFORM_POWER_NO_MECHANISM),
                 "PLATFORM_POWER_NO_MECHANISM") == 0,
          "the status vocabulary is exact");
}

int main(void)
{
    test_sleep_type_walk();
    test_plan_decisions();
    test_vocabulary();

    if (failures != 0) {
        fprintf(stderr, "platform power host test: %d FAILURES\n", failures);
        return 1;
    }
    puts("platform power host test: ALL OK");
    return 0;
}
