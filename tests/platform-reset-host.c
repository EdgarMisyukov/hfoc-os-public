/*
 * ITEM-411 host proof: which reset mechanism this machine is rebooted through.
 *
 * The decision is a pure function of what the platform declared, so the shapes
 * that matter - the acceptance host (no i8042, no ACPI reset register), a
 * machine whose FADT declares one, and a platform that declares nothing at all -
 * are pinned here without a chipset, a keyboard controller or a reboot.  Which
 * mechanism the acceptance host *honours* can only be observed on the host
 * itself, and the boot record says which one reset it.
 */

#include "platform_reset.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define TEST(name) do { printf("  %s: ", name); } while (0)
#define PASS() do { puts("PASS"); } while (0)
#define FAIL(msg) do { puts("FAIL: " msg); ++failures; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)

static struct platform_reset_environment environment(void) {
    struct platform_reset_environment value;

    memset(&value, 0, sizeof(value));
    return value;
}

/* The acceptance host: the FADT declares no reset register, the i8042 never
 * answered (`PS2: UNAVAILABLE`), and the PCI host bridge does. */
static void test_acceptance_host(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("acceptance host shape: no ACPI reset register, no i8042 -> PCI");
    env.pci_reset_available = true;
    env.i8042_present = false;
    env.acpi_declared = false;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 2U, "the plan should hold PCI and the fault");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_PCI,
          "PCI should be the mechanism this machine resets through");
    CHECK(plan.steps[1U] == PLATFORM_RESET_MECHANISM_FAULT,
          "the fault should be the last resort");
    CHECK(platform_reset_plan_primary(&plan) == PLATFORM_RESET_MECHANISM_PCI,
          "the published mechanism should be PCI");
    PASS();
}

static void test_acpi_declared(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a declared ACPI reset register is tried first");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    env.acpi_bit_width = 8U;
    env.acpi_access_size = 1U;
    env.acpi_address = 0xCF9U;
    env.acpi_value = 0x06U;
    env.pci_reset_available = true;
    env.i8042_present = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 4U, "every declared mechanism should be in the plan");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_ACPI,
          "ACPI should come first");
    CHECK(plan.steps[1U] == PLATFORM_RESET_MECHANISM_PCI,
          "PCI should follow ACPI");
    CHECK(plan.steps[2U] == PLATFORM_RESET_MECHANISM_I8042,
          "the i8042 should follow PCI");
    CHECK(plan.steps[3U] == PLATFORM_RESET_MECHANISM_FAULT,
          "the fault should be the last resort");
    CHECK(platform_reset_plan_primary(&plan) == PLATFORM_RESET_MECHANISM_ACPI,
          "the published mechanism should be ACPI");
    PASS();
}

static void test_acpi_declared_without_i8042(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a declared ACPI register and no i8042 leaves the 8042 step out");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_MEMORY;
    env.acpi_bit_width = 32U;
    env.acpi_access_size = 3U;
    env.acpi_address = UINT64_C(0x1FEDF1000);
    env.acpi_value = 0xABU;
    env.pci_reset_available = true;
    env.i8042_present = false;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 3U, "ACPI, PCI and the fault");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_ACPI &&
          plan.steps[1U] == PLATFORM_RESET_MECHANISM_PCI &&
          plan.steps[2U] == PLATFORM_RESET_MECHANISM_FAULT,
          "a machine with no controller must not be sent 0xFE");
    PASS();
}

static void test_nothing_declared(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a platform that declares nothing is reset by the fault alone");
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 1U, "the fault is always available");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_FAULT,
          "nothing else can be tried");
    CHECK(platform_reset_plan_primary(&plan) ==
          PLATFORM_RESET_MECHANISM_FAULT,
          "the published mechanism should be FAULT");
    PASS();
}

static void test_i8042_only(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("an i8042 with no ACPI register and no PCI bridge");
    env.i8042_present = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 2U &&
          plan.steps[0U] == PLATFORM_RESET_MECHANISM_I8042 &&
          plan.steps[1U] == PLATFORM_RESET_MECHANISM_FAULT,
          "the controller reset should precede the fault");
    PASS();
}

static void test_acpi_space_not_reachable(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a reset register this kernel cannot write is not claimed");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_PCI_CONFIGURATION;
    env.acpi_bit_width = 8U;
    env.acpi_access_size = 1U;
    env.acpi_address = 0x40U;
    env.acpi_value = 0x02U;
    env.pci_reset_available = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 2U && plan.steps[0U] == PLATFORM_RESET_MECHANISM_PCI,
          "an unreachable register must not shadow the mechanism that works");
    PASS();
}

static void test_acpi_address_zero(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a declared register with no address is not a mechanism");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    env.acpi_bit_width = 8U;
    env.acpi_access_size = 0U;
    env.acpi_address = 0U;
    env.acpi_value = 0x06U;
    env.pci_reset_available = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_PCI,
          "address zero is not a register");
    PASS();
}

static void test_acpi_not_declared_flag(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a FADT without RESET_REG_SUP contributes no step");
    env.acpi_declared = false;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    env.acpi_address = 0xCF9U;
    env.acpi_value = 0x06U;
    env.pci_reset_available = true;
    env.i8042_present = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count == 3U &&
          plan.steps[0U] == PLATFORM_RESET_MECHANISM_PCI,
          "an undeclared register must not be written");
    PASS();
}

static void test_null_arguments(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("null arguments are refused, never guessed");
    CHECK(platform_reset_plan_build(NULL, &plan) ==
          PLATFORM_RESET_NULL_ARGUMENT,
          "a missing environment should be refused");
    CHECK(platform_reset_plan_build(&env, NULL) ==
          PLATFORM_RESET_NULL_ARGUMENT,
          "a missing plan should be refused");
    CHECK(platform_reset_plan_primary(NULL) == PLATFORM_RESET_MECHANISM_NONE,
          "a missing plan has no primary mechanism");
    plan.count = 0U;
    CHECK(platform_reset_plan_primary(&plan) == PLATFORM_RESET_MECHANISM_NONE,
          "an empty plan has no primary mechanism");
    PASS();
}

static void test_acpi_width(void) {
    TEST("the ACPI register width comes from the table it wrote");
    CHECK(platform_reset_acpi_width_bytes(8U, 1U) == 1U,
          "byte access is one byte");
    CHECK(platform_reset_acpi_width_bytes(16U, 2U) == 2U,
          "word access is two bytes");
    CHECK(platform_reset_acpi_width_bytes(32U, 3U) == 4U,
          "dword access is four bytes");
    CHECK(platform_reset_acpi_width_bytes(64U, 4U) == 8U,
          "qword access is eight bytes");
    CHECK(platform_reset_acpi_width_bytes(8U, 0U) == 1U,
          "an undefined access size falls back to the bit width");
    CHECK(platform_reset_acpi_width_bytes(16U, 0U) == 2U,
          "16 bits is two bytes");
    CHECK(platform_reset_acpi_width_bytes(32U, 0U) == 4U,
          "32 bits is four bytes");
    CHECK(platform_reset_acpi_width_bytes(64U, 0U) == 8U,
          "64 bits is eight bytes");
    CHECK(platform_reset_acpi_width_bytes(0U, 0U) == 1U,
          "a table that declares nothing is written byte-wise");
    CHECK(platform_reset_acpi_width_bytes(8U, 3U) == 4U,
          "the access size wins over the bit width");
    PASS();
}

static void test_mechanism_strings(void) {
    TEST("the record vocabulary is the documented one");
    CHECK(strcmp(platform_reset_mechanism_string(
                     PLATFORM_RESET_MECHANISM_ACPI), "ACPI") == 0,
          "ACPI");
    CHECK(strcmp(platform_reset_mechanism_string(
                     PLATFORM_RESET_MECHANISM_PCI), "PCI") == 0,
          "PCI");
    CHECK(strcmp(platform_reset_mechanism_string(
                     PLATFORM_RESET_MECHANISM_I8042), "I8042") == 0,
          "I8042");
    CHECK(strcmp(platform_reset_mechanism_string(
                     PLATFORM_RESET_MECHANISM_FAULT), "FAULT") == 0,
          "FAULT");
    CHECK(strcmp(platform_reset_mechanism_string(
                     PLATFORM_RESET_MECHANISM_NONE), "NONE") == 0,
          "NONE");
    PASS();
}

static void test_plan_shape(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("no plan can overflow or come back empty");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    env.acpi_address = 0xCF9U;
    env.acpi_value = 0x06U;
    env.pci_reset_available = true;
    env.i8042_present = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.count <= PLATFORM_RESET_STEP_CAPACITY,
          "the plan must fit its declared capacity");
    CHECK(plan.steps[plan.count - 1U] == PLATFORM_RESET_MECHANISM_FAULT,
          "every plan ends in the fault");
    /* Every mechanism appears at most once. */
    for (size_t left = 0U; left < plan.count; ++left) {
        for (size_t right = left + 1U; right < plan.count; ++right) {
            CHECK(plan.steps[left] != plan.steps[right],
                  "a mechanism must not be tried twice");
        }
    }
    PASS();
}

static void test_acpi_port_wider_than_port_space(void) {
    struct platform_reset_environment env = environment();
    struct platform_reset_plan plan;

    TEST("a SystemIO address wider than a port is never truncated onto one");
    env.acpi_declared = true;
    env.acpi_address_space = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    env.acpi_bit_width = 8U;
    env.acpi_access_size = 1U;
    env.acpi_address = UINT64_C(0x1FEDF1000);
    env.acpi_value = 0x06U;
    env.pci_reset_available = true;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK,
          "the plan should build");
    CHECK(plan.steps[0U] == PLATFORM_RESET_MECHANISM_PCI,
          "an unaddressable port must not shadow the mechanism that works");
    env.acpi_address = 0xFFFFU;
    CHECK(platform_reset_plan_build(&env, &plan) == PLATFORM_RESET_OK &&
          plan.steps[0U] == PLATFORM_RESET_MECHANISM_ACPI,
          "the last port address is still a port");
    PASS();
}

static void test_pit_delay_math(void) {
    TEST("the operator delay is measured on the PIT, wrap included");
    CHECK(platform_reset_pit_counts_for_ms(0U) == 0U,
          "no milliseconds is no counts");
    CHECK(platform_reset_pit_counts_for_ms(1000U) ==
              PLATFORM_RESET_PIT_BASE_HZ,
          "one second is the PIT's own frequency in counts");
    CHECK(platform_reset_pit_counts_for_ms(
              PLATFORM_RESET_OPERATOR_DELAY_MS) ==
              ((uint64_t)PLATFORM_RESET_OPERATOR_DELAY_MS *
               PLATFORM_RESET_PIT_BASE_HZ) / 1000U,
          "the operator delay is the declared few seconds");
    CHECK(platform_reset_pit_counts_for_ms(PLATFORM_RESET_PCI_SETTLE_MS) >=
              1000U,
          "a settle delay is a real wait and not zero counts");
    CHECK(platform_reset_pit_delta(0x1000U, 0x0F00U) == 0x100U,
          "a down-counter advanced by the difference");
    CHECK(platform_reset_pit_delta(0x0000U, 0xFF00U) == 0x100U,
          "a reload between samples must not wrap the difference");
    CHECK(platform_reset_pit_delta(0xFFFFU, 0xFFFFU) == 0U,
          "a stopped counter advances by nothing");
    PASS();
}

int main(void) {
    test_acceptance_host();
    test_acpi_declared();
    test_acpi_declared_without_i8042();
    test_nothing_declared();
    test_i8042_only();
    test_acpi_space_not_reachable();
    test_acpi_address_zero();
    test_acpi_not_declared_flag();
    test_null_arguments();
    test_acpi_width();
    test_mechanism_strings();
    test_plan_shape();
    test_acpi_port_wider_than_port_space();
    test_pit_delay_math();
    if (failures == 0) {
        puts("Platform reset host test: ALL OK");
        return 0;
    }
    printf("Platform reset host test: %d FAILURES\n", failures);
    return 1;
}
