#ifndef HFOC_PLATFORM_POWER_H
#define HFOC_PLATFORM_POWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-418: reboot and power off the machine from the kernel.
 *
 * `platform_reset` (ITEM-411) answers "how does this machine reset itself", and
 * the installer uses it to leave the medium behind.  Nothing answered "how does
 * it power *off*", and nothing in the product offered either action to an
 * operator: the appliance could be started and then only stopped by hand.
 *
 * Reboot reuses the reset chain that is already proven on hardware - the FADT's
 * reset register, then the PCI reset control register, then the i8042, then a
 * triple fault - so this kernel keeps one reset implementation, not two.  Power
 * off has exactly one honest mechanism on a PC: ACPI S5, which takes two things
 * from the firmware and neither from a guess:
 *
 *   1. the PM1 control block, `PM1a_CNT_BLK` (and `PM1b_CNT_BLK` when the FADT
 *      declares one), read out of the FADT;
 *   2. the sleep type of the `\_S5` object, read out of the DSDT.
 *
 * A platform that declares no control block is refused, not guessed at: this
 * module never writes to port zero and never uses a sleep type the firmware did
 * not state.  What happens is published, never assumed:
 *
 *   [PLATFORM-POWER] plan action=SHUTDOWN mechanism=ACPI_S5 pm1a=0x0604
 *                    pm1b=NONE slp_typ=0x0005 slp_typ_b=0x0005 dsdt=OK
 *   [PLATFORM-POWER] attempt=1/2 action=SHUTDOWN space=IO address=0x0604
 *                    value=0x3400
 *   [PLATFORM-POWER] result=STILL_RUNNING action=SHUTDOWN attempts=2
 *
 * The machine powers off inside the attempt the platform honours, so the last
 * `[PLATFORM-POWER]` record of a boot log is the request it was asked to carry
 * out.  A machine that is still running when the module returns publishes
 * `result=STILL_RUNNING`, which is a finding rather than a silence; a plan that
 * has no mechanism at all is refused before any write and says why.
 */

enum platform_power_action {
    PLATFORM_POWER_ACTION_NONE = 0,
    PLATFORM_POWER_ACTION_REBOOT,
    PLATFORM_POWER_ACTION_SHUTDOWN
};

enum platform_power_mechanism {
    PLATFORM_POWER_MECHANISM_NONE = 0,
    PLATFORM_POWER_MECHANISM_RESET_CHAIN,
    PLATFORM_POWER_MECHANISM_ACPI_S5
};

enum platform_power_status {
    PLATFORM_POWER_OK = 0,
    PLATFORM_POWER_NULL = 1,
    PLATFORM_POWER_BAD_ACTION = 2,
    PLATFORM_POWER_NO_MECHANISM = 3
};

/* ACPI PM1 control register bits (ACPI 6.x, 4.8.2.1). */
#define PLATFORM_POWER_SLP_EN UINT16_C(0x2000)
#define PLATFORM_POWER_SLP_TYP_SHIFT 10U
/* The bounded settle delay after a PM1 write, before it is called ignored. */
#define PLATFORM_POWER_PM1_SETTLE_MS 250U
/* What the module waits after publishing its plan, so an operator can read it.
 * A delay, never a prompt: the local keyboard may be unavailable. */
#define PLATFORM_POWER_OPERATOR_DELAY_MS 2000U

/*
 * What the firmware of this machine declares about power, as one value the
 * proof can drive and the kernel fills from ACPI.  It is a description, never a
 * verdict: whether the platform honours the write is the platform's answer.
 */
struct platform_power_environment {
    bool acpi_present;
    bool pm1a_declared;
    bool pm1b_declared;
    uint16_t pm1a_control;
    uint16_t pm1b_control;
    bool dsdt_present;
    bool sleep_type_declared;
    uint16_t sleep_type_a;
    uint16_t sleep_type_b;
    uint32_t sleep_type_offset;
    /* The table itself, for the bounded diagnostics a refusal publishes. */
    const void *dsdt;
    uint32_t dsdt_length;
};

struct platform_power_plan {
    enum platform_power_action action;
    enum platform_power_mechanism mechanism;
    bool pm1b_declared;
    uint16_t pm1a_control;
    uint16_t pm1b_control;
    uint16_t sleep_type_a;
    uint16_t sleep_type_b;
    uint32_t sleep_type_offset;
};

/*
 * The `\_S5` sleep type of a validated DSDT, with the offset it was found at so
 * a record can quote the table it came from.  Fail-closed: a DSDT without the
 * object, with an object this kernel will not parse, or with a package the
 * search bound cuts off leaves the sleep type undeclared.
 */
bool platform_power_sleep_type_find(
    const void *dsdt, uint32_t dsdt_length,
    uint16_t *sleep_type_a, uint16_t *sleep_type_b,
    uint32_t *offset);

/* The decision, pure and host-testable. */
enum platform_power_status platform_power_plan_build(
    enum platform_power_action action,
    const struct platform_power_environment *environment,
    struct platform_power_plan *out);

/* This machine's environment and plan, from what ACPI declared at boot. */
void platform_power_environment_current(
    struct platform_power_environment *out);
enum platform_power_status platform_power_plan_current(
    enum platform_power_action action,
    struct platform_power_plan *out);

const char *platform_power_action_string(enum platform_power_action action);
const char *platform_power_mechanism_string(
    enum platform_power_mechanism mechanism);
const char *platform_power_status_string(enum platform_power_status status);

/*
 * Carry the plan out.  A reboot never returns: it runs the reset chain.  A
 * shutdown writes the PM1 control blocks the plan names and returns only when
 * the platform ignored every one of them, publishing that verdict first.
 */
void platform_power_execute(const struct platform_power_plan *plan);

#endif
