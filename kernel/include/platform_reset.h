#ifndef HFOC_PLATFORM_RESET_H
#define HFOC_PLATFORM_RESET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi.h"

/*
 * ITEM-411: reboot the machine from the kernel.
 *
 * On the acceptance host the install completes and the machine then carries on
 * with the ordinary boot *of the installation medium*, because nothing ever
 * reset it and the firmware prefers the removable medium: the operator sees the
 * appliance from the stick, and has to power-cycle by hand to reach the system
 * that was just installed.  This module is the reset the flow was missing.
 *
 * The mechanisms are tried in the documented order below, and every one of them
 * falls through to the next when the platform ignores it:
 *
 *   1. ACPI  - the FADT's own reset register, when the table declares one
 *              (`RESET_REG_SUP`, `RESET_REG`, `RESET_VALUE`).  This is the only
 *              mechanism the firmware describes rather than the kernel
 *              guessing, so it is tried first.
 *   2. PCI   - a write of the reset control register at port 0xCF9, the
 *              documented chipset sequence (set SYS_RST, then SYS_RST|RST_CPU).
 *              The step is taken when a PCI host bridge answered on the
 *              configuration mechanism, because the reset control register
 *              belongs to that chipset.
 *   3. I8042 - command 0xFE on port 0x64, the classic keyboard-controller
 *              reset.  It is tried **only** when the i8042 itself answered at
 *              ports 0x60/0x64 during boot: the acceptance host has no
 *              controller at all (`PS2: UNAVAILABLE`), where this write would
 *              go nowhere.
 *   4. FAULT - a triple fault through a null IDT, which needs nothing from the
 *              platform and is therefore always the last step of the plan.
 *
 * Nothing here can hang the machine: every mechanism is a bounded write
 * followed by a bounded settle delay, a mechanism the platform ignores simply
 * ends its step, and the last step is the reset the CPU performs by itself.
 *
 * What happens is published, not assumed.  Before the first write the plan is
 * published as one record, and before every attempt the mechanism is published
 * again:
 *
 *   [PLATFORM-RESET] plan reset=PCI steps=PCI>I8042>FAULT
 *   [PLATFORM-RESET] attempt=1/3 reset=PCI port=0xCF9 value=0x06
 *   [PLATFORM-RESET] attempt=2/3 reset=I8042 port=0x64 command=0xFE
 *
 * A mechanism that resets the machine does so inside its own step, so the last
 * `reset=` record in a boot log is the mechanism the platform honoured.
 */

enum platform_reset_mechanism {
    PLATFORM_RESET_MECHANISM_NONE = 0,
    PLATFORM_RESET_MECHANISM_ACPI,
    PLATFORM_RESET_MECHANISM_PCI,
    PLATFORM_RESET_MECHANISM_I8042,
    PLATFORM_RESET_MECHANISM_FAULT
};

/* Four mechanisms, and a plan never holds more than one of each. */
#define PLATFORM_RESET_STEP_CAPACITY 4U

struct platform_reset_plan {
    size_t count;
    enum platform_reset_mechanism steps[PLATFORM_RESET_STEP_CAPACITY];
};

/*
 * The platform facts the plan is decided from.  Every field is evidence: the
 * ACPI fields come from the FADT the kernel already parsed, `pci_reset_available`
 * from the host bridge answering the PCI configuration mechanism, and
 * `i8042_present` from the keyboard driver's own observation.  Nothing is
 * assumed about a controller or a register the machine did not describe.
 */
struct platform_reset_environment {
    bool acpi_declared;
    uint8_t acpi_address_space; /* enum acpi_address_space */
    uint8_t acpi_bit_width;
    uint8_t acpi_access_size;
    uint64_t acpi_address;
    uint8_t acpi_value;
    bool pci_reset_available;
    bool i8042_present;
};

enum platform_reset_status {
    PLATFORM_RESET_OK = 0,
    PLATFORM_RESET_NULL_ARGUMENT
};

/* Reference numbers, so a record, a proof and the documentation name one set. */
#define PLATFORM_RESET_PCI_PORT UINT16_C(0xCF9)
#define PLATFORM_RESET_PCI_HARD_RESET UINT8_C(0x02)
#define PLATFORM_RESET_PCI_CPU_RESET UINT8_C(0x04)
#define PLATFORM_RESET_PCI_CONFIG_ADDRESS UINT16_C(0xCF8)
#define PLATFORM_RESET_PCI_CONFIG_DATA UINT16_C(0xCFC)
#define PLATFORM_RESET_I8042_PORT UINT16_C(0x64)
#define PLATFORM_RESET_I8042_COMMAND UINT8_C(0xFE)
/* Bounded settle delays: the platform is given this long to obey, no longer. */
#define PLATFORM_RESET_ACPI_SETTLE_MS 100U
#define PLATFORM_RESET_PCI_SETTLE_MS 100U
#define PLATFORM_RESET_I8042_SETTLE_MS 250U
/*
 * How long the kernel waits after it has published what it is about to do, so
 * an operator can read the record and pull the installation medium out.  It is
 * a delay, never a prompt: the local keyboard may be unavailable, so no reset
 * path may wait for input.
 */
#define PLATFORM_RESET_OPERATOR_DELAY_MS 5000U

/*
 * The delay is measured on PIT channel 0, not on the tick counter: the install
 * flow runs before the kernel enables interrupts (`cpu_sti()` is reached after
 * it), so a tick-based wait would never end on time and a spin-based one would
 * depend on the processor.  ITEM-417: the channel is not assumed to be
 * counting - the flow also runs before the timer programs it, and the
 * firmware's counter may be stopped - so the delay puts channel 0 into a
 * counting state itself and measures what it programmed.
 */
#define PLATFORM_RESET_PIT_BASE_HZ 1193182U
#define PLATFORM_RESET_PIT_CHANNEL0_PORT UINT16_C(0x40)
#define PLATFORM_RESET_PIT_COMMAND_PORT UINT16_C(0x43)
#define PLATFORM_RESET_PIT_LATCH_CHANNEL0 UINT8_C(0x00)

/* How many PIT counts a delay of `milliseconds` lasts. */
uint64_t platform_reset_pit_counts_for_ms(uint32_t milliseconds);

/*
 * The counts a down-counter advanced between two latch reads: channel 0 counts
 * down and reloads, so the difference wraps at 16 bits.
 */
uint32_t platform_reset_pit_delta(uint16_t previous, uint16_t current);

/*
 * A delay on that counter, measured in counts of the base frequency.  It ends
 * when the counts are in; a counter that stops moving ends it too, so a
 * platform whose PIT is dead cannot park a caller here, but a live one is
 * never cut short: the wait is the delay that was asked for.
 */
void platform_reset_delay_ms(uint32_t milliseconds);
/* True only when the requested PIT counts elapsed; false on bounded stall. */
bool platform_reset_delay_ms_checked(uint32_t milliseconds);

/*
 * The decision, pure and host-testable: which mechanisms this machine will be
 * reset through, in order.  A plan always ends in FAULT, so a build never comes
 * back empty and a caller never has to invent a fallback.
 */
enum platform_reset_status platform_reset_plan_build(
    const struct platform_reset_environment *environment,
    struct platform_reset_plan *out);

/* The first mechanism the plan will try: what a record states before the reset. */
enum platform_reset_mechanism platform_reset_plan_primary(
    const struct platform_reset_plan *plan);

/*
 * The ACPI register width in bytes: the FADT's access size when it declares one
 * (1 byte, 2 word, 3 dword, 4 qword), otherwise the declared bit width rounded
 * up to whole bytes, and one byte when the table declares neither.
 */
size_t platform_reset_acpi_width_bytes(
    uint8_t bit_width, uint8_t access_size);

/* `ACPI`, `PCI`, `I8042`, `FAULT` - the record vocabulary. */
const char *platform_reset_mechanism_string(
    enum platform_reset_mechanism mechanism);

/* This machine's environment and plan, from what boot observed. */
void platform_reset_environment_current(
    struct platform_reset_environment *out);
enum platform_reset_status platform_reset_plan_current(
    struct platform_reset_plan *out);

/*
 * Perform the plan.  Each step publishes what it is about to do, performs one
 * bounded write and waits a bounded moment; a step the platform ignores falls
 * through to the next.  This function does not return.
 */
void platform_reset_execute(const struct platform_reset_plan *plan)
    __attribute__((noreturn));

#endif
