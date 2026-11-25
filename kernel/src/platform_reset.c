/*
 * ITEM-411 platform reset.
 *
 * The decision (which mechanisms this machine is reset through, in which order)
 * is a pure function of what the platform declared, so it is compiled - and
 * proven - on the host.  The mechanisms themselves are the kernel's own I/O and
 * live behind HFOC_PLATFORM_RESET_HOST_TEST, so the host proof needs neither a
 * chipset nor a keyboard controller to pin the order.
 *
 * See kernel/include/platform_reset.h for the documented order, the bounds and
 * the records.
 */

#include "platform_reset.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- decision --- */

size_t platform_reset_acpi_width_bytes(
    uint8_t bit_width, uint8_t access_size)
{
    /*
     * The generic address structure's access size says how wide the register is
     * written: 1 byte, 2 word, 3 dword, 4 qword, 0 undefined.  An undefined
     * access size falls back to the declared bit width, which is what the
     * register actually is; a table that declares neither is written byte-wise,
     * because the reset value is one byte.
     */
    switch (access_size) {
    case 1U: return 1U;
    case 2U: return 2U;
    case 3U: return 4U;
    case 4U: return 8U;
    default: break;
    }
    if (bit_width <= 8U) return 1U;
    if (bit_width <= 16U) return 2U;
    if (bit_width <= 32U) return 4U;
    return 8U;
}

/*
 * An ACPI reset register is usable only when this kernel can actually write it:
 * the FADT must declare it, the address must be a real one, and the address
 * space must be one the kernel reaches - SystemIO through a port write,
 * SystemMemory through the direct map.  A register in PCI configuration space,
 * on an embedded controller, on SMBus or in fixed hardware is *declared* but not
 * writable from here, and claiming otherwise would mean a record that names a
 * mechanism this kernel never used.  A SystemIO register wider than the port
 * space is refused too: writing it would truncate the address onto an unrelated
 * port.
 */
static bool platform_reset_acpi_usable(
    const struct platform_reset_environment *environment)
{
    if (!environment->acpi_declared) return false;
    if (environment->acpi_address == 0U) return false;
    switch (environment->acpi_address_space) {
    case ACPI_ADDRESS_SPACE_SYSTEM_IO:
        return environment->acpi_address <= UINT64_C(0xFFFF);
    case ACPI_ADDRESS_SPACE_SYSTEM_MEMORY:
        return true;
    default:
        return false;
    }
}

enum platform_reset_status platform_reset_plan_build(
    const struct platform_reset_environment *environment,
    struct platform_reset_plan *out)
{
    if (environment == NULL || out == NULL) {
        return PLATFORM_RESET_NULL_ARGUMENT;
    }
    out->count = 0U;
    if (platform_reset_acpi_usable(environment)) {
        out->steps[out->count++] = PLATFORM_RESET_MECHANISM_ACPI;
    }
    if (environment->pci_reset_available) {
        out->steps[out->count++] = PLATFORM_RESET_MECHANISM_PCI;
    }
    if (environment->i8042_present) {
        out->steps[out->count++] = PLATFORM_RESET_MECHANISM_I8042;
    }
    /*
     * A triple fault needs nothing from the platform, so it is always the last
     * step: a plan that cannot come back empty is a plan that cannot leave a
     * machine running after a completed install.
     */
    out->steps[out->count++] = PLATFORM_RESET_MECHANISM_FAULT;
    return PLATFORM_RESET_OK;
}

enum platform_reset_mechanism platform_reset_plan_primary(
    const struct platform_reset_plan *plan)
{
    if (plan == NULL || plan->count == 0U) {
        return PLATFORM_RESET_MECHANISM_NONE;
    }
    return plan->steps[0];
}

const char *platform_reset_mechanism_string(
    enum platform_reset_mechanism mechanism)
{
    switch (mechanism) {
    case PLATFORM_RESET_MECHANISM_ACPI: return "ACPI";
    case PLATFORM_RESET_MECHANISM_PCI: return "PCI";
    case PLATFORM_RESET_MECHANISM_I8042: return "I8042";
    case PLATFORM_RESET_MECHANISM_FAULT: return "FAULT";
    case PLATFORM_RESET_MECHANISM_NONE: break;
    }
    return "NONE";
}

uint64_t platform_reset_pit_counts_for_ms(uint32_t milliseconds) {
    return ((uint64_t)milliseconds * PLATFORM_RESET_PIT_BASE_HZ) / 1000U;
}

uint32_t platform_reset_pit_delta(uint16_t previous, uint16_t current) {
    /*
     * Channel 0 counts down from its reload value, so the counts elapsed
     * between two samples is the *previous* value minus the current one, and
     * the subtraction wraps when the counter reloaded in between.
     */
    return (uint32_t)(uint16_t)(previous - current);
}

#ifdef HFOC_PLATFORM_RESET_HOST_TEST
/* The host proof drives the decision and the vocabulary only. */
#else

/* ---------------------------------------------------------- mechanisms --- */

#include "arch/x86_64/io.h"
#include "console.h"
#include "record_line.h"
#include "keyboard.h"
#include "kstring.h"
#include "serial.h"

/* The direct map base, published by the VMM; the ACPI memory-space register is
 * written through it, exactly as the rest of the kernel reaches physical
 * memory. */
extern uint64_t vmm_hhdm_offset(void);

static inline void platform_reset_outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t platform_reset_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/*
 * One bounded record, assembled once and published once: the serial stream and
 * the framebuffer mirror carry the same bytes, in the same order, so an
 * operator watching the screen sees what a serial capture would.
 */
static void platform_reset_publish(const char *text) {
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write(text);
    serial_sequence_end(serial_flags);
    /* ITEM-478: the record reaches the durable journal, not the screen; the
     * serial stream above stays byte for byte what this module wrote. */
    console_publish_journal(text, strlen(text));
}

static const char *platform_reset_space_string(uint8_t address_space) {
    switch (address_space) {
    case ACPI_ADDRESS_SPACE_SYSTEM_IO: return "IO";
    case ACPI_ADDRESS_SPACE_SYSTEM_MEMORY: return "MEMORY";
    case ACPI_ADDRESS_SPACE_PCI_CONFIGURATION: return "PCI_CONFIG";
    case ACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER: return "EC";
    case ACPI_ADDRESS_SPACE_SMBUS: return "SMBUS";
    case ACPI_ADDRESS_SPACE_FIXED_HARDWARE: return "FIXED";
    default: return "UNKNOWN";
    }
}

static void platform_reset_publish_plan(
    const struct platform_reset_plan *plan,
    const struct platform_reset_environment *environment)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-RESET] plan reset=");
    record_line_append(
        &line,
        platform_reset_mechanism_string(platform_reset_plan_primary(plan)));
    record_line_append(&line, " steps=");
    for (size_t index = 0U; index < plan->count; ++index) {
        if (index != 0U) {
            record_line_append(&line, ">");
        }
        record_line_append(
            &line, platform_reset_mechanism_string(plan->steps[index]));
    }
    if (environment->acpi_declared) {
        record_line_append(&line, " acpi_declared=1");
    } else {
        record_line_append(&line, " acpi_declared=0");
    }
    if (environment->pci_reset_available) {
        record_line_append(&line, " pci=1");
    } else {
        record_line_append(&line, " pci=0");
    }
    if (environment->i8042_present) {
        record_line_append(&line, " i8042=1");
    } else {
        record_line_append(&line, " i8042=0");
    }
    record_line_append(&line, "\n");
    if (!line.overflow) {
        platform_reset_publish(line.text);
    }
}

static void platform_reset_publish_attempt(
    size_t index,
    size_t count,
    const struct platform_reset_environment *environment,
    enum platform_reset_mechanism mechanism)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-RESET] attempt=");
    record_line_u64(&line, (uint64_t)(index + 1U));
    record_line_append(&line, "/");
    record_line_u64(&line, (uint64_t)count);
    record_line_append(&line, " reset=");
    record_line_append(
        &line, platform_reset_mechanism_string(mechanism));
    switch (mechanism) {
    case PLATFORM_RESET_MECHANISM_ACPI:
        record_line_append(&line, " space=");
        record_line_append(
            &line, platform_reset_space_string(environment->acpi_address_space));
        record_line_append(&line, " address=");
        record_line_hex(&line, environment->acpi_address);
        record_line_append(&line, " value=");
        record_line_hex(&line, (uint64_t)environment->acpi_value);
        record_line_append(&line, " width=");
        record_line_u64(
            &line,
            (uint64_t)platform_reset_acpi_width_bytes(
                environment->acpi_bit_width,
                environment->acpi_access_size));
        break;
    case PLATFORM_RESET_MECHANISM_PCI:
        record_line_append(&line, " port=");
        record_line_hex(&line, PLATFORM_RESET_PCI_PORT);
        record_line_append(&line, " value=");
        record_line_hex(
            &line,
            (uint64_t)(PLATFORM_RESET_PCI_HARD_RESET |
                       PLATFORM_RESET_PCI_CPU_RESET));
        break;
    case PLATFORM_RESET_MECHANISM_I8042:
        record_line_append(&line, " port=");
        record_line_hex(&line, PLATFORM_RESET_I8042_PORT);
        record_line_append(&line, " command=");
        record_line_hex(&line, PLATFORM_RESET_I8042_COMMAND);
        break;
    case PLATFORM_RESET_MECHANISM_FAULT:
        record_line_append(&line, " detail=triple-fault");
        break;
    case PLATFORM_RESET_MECHANISM_NONE:
        break;
    }
    record_line_append(&line, "\n");
    if (!line.overflow) {
        platform_reset_publish(line.text);
    }
}

/*
 * The ACPI reset register: a port write for SystemIO, a direct-map write for
 * SystemMemory, of the width the table declared, carrying the value the table
 * declared.  Only the two spaces the kernel can reach are ever planned, so this
 * switch has no silent third case.
 */
static void platform_reset_acpi_write(
    const struct platform_reset_environment *environment)
{
    const size_t width = platform_reset_acpi_width_bytes(
        environment->acpi_bit_width, environment->acpi_access_size);
    const uint64_t value = (uint64_t)environment->acpi_value;

    if (environment->acpi_address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        /* The reset value is one byte by definition, and the port is byte
         * wide; a wider declaration does not turn 0xCF9 into a wide port. */
        outb((uint16_t)environment->acpi_address, environment->acpi_value);
        return;
    }
    {
        const uint64_t hhdm = vmm_hhdm_offset();
        volatile uint8_t *base;

        if (environment->acpi_address > UINT64_MAX - hhdm) {
            /* Not addressable through the direct map: fall through to the next
             * mechanism rather than write a wrapped address. */
            return;
        }
        base = (volatile uint8_t *)(uintptr_t)(
            environment->acpi_address + hhdm);

        switch (width) {
        case 1U:
            *base = (uint8_t)value;
            break;
        case 2U:
            *(volatile uint16_t *)base = (uint16_t)value;
            break;
        case 4U:
            *(volatile uint32_t *)base = (uint32_t)value;
            break;
        default:
            *(volatile uint64_t *)base = value;
            break;
        }
    }
}

/*
 * The chipset reset control register at 0xCF9, written in the documented order:
 * the current value with the reset bits cleared, then SYS_RST (a hard reset
 * request), then SYS_RST|RST_CPU (do it).  Bits this kernel does not own are
 * preserved by the read-modify-write.
 */
static void platform_reset_pci_write(void) {
    const uint8_t current = inb(PLATFORM_RESET_PCI_PORT);
    const uint8_t base = (uint8_t)(current &
        (uint8_t)~(PLATFORM_RESET_PCI_HARD_RESET |
                   PLATFORM_RESET_PCI_CPU_RESET));

    outb(PLATFORM_RESET_PCI_PORT, (uint8_t)(base |
                                            PLATFORM_RESET_PCI_HARD_RESET));
    platform_reset_delay_ms(50U);
    outb(PLATFORM_RESET_PCI_PORT,
         (uint8_t)(base | PLATFORM_RESET_PCI_HARD_RESET |
                   PLATFORM_RESET_PCI_CPU_RESET));
}

static void platform_reset_i8042_write(void) {
    outb(PLATFORM_RESET_I8042_PORT, PLATFORM_RESET_I8042_COMMAND);
}

/*
 * The last resort: an IDT with no gates, then a breakpoint.  The processor
 * raises #BP, finds no gate, double-faults, finds no gate again and resets
 * itself.  Interrupts are disabled first so nothing but that sequence can
 * happen in between; if even this does not reset the machine, the kernel stays
 * here rather than returning into a system that has lost its IDT.
 */
static _Noreturn void platform_reset_triple_fault(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt;

    null_idt.limit = 0U;
    null_idt.base = 0U;
    cpu_cli();
    __asm__ volatile ("lidt %0" : : "m"(null_idt) : "memory");
    __asm__ volatile ("int3");
    for (;;) {
        cpu_hlt();
    }
}

void platform_reset_environment_current(
    struct platform_reset_environment *out)
{
    const struct acpi_fadt_reset *fadt;

    if (out == NULL) {
        return;
    }
    out->acpi_declared = false;
    out->acpi_address_space = 0U;
    out->acpi_bit_width = 0U;
    out->acpi_access_size = 0U;
    out->acpi_address = 0U;
    out->acpi_value = 0U;
    /*
     * The PCI configuration mechanism answering at 0:0.0 is the chipset that
     * owns the reset control register at 0xCF9; a bus with no bridge is not a
     * machine this step can reset, and the plan falls through to the fault.
     */
    platform_reset_outl(PLATFORM_RESET_PCI_CONFIG_ADDRESS,
                        UINT32_C(0x80000000));
    out->pci_reset_available =
        (platform_reset_inl(PLATFORM_RESET_PCI_CONFIG_DATA) & 0xFFFFU) !=
        0xFFFFU;
    out->i8042_present = keyboard_controller_present();

    fadt = acpi_fadt_reset();
    if (fadt != NULL) {
        out->acpi_declared = fadt->supported;
        out->acpi_address_space = fadt->address_space;
        out->acpi_bit_width = fadt->bit_width;
        out->acpi_access_size = fadt->access_size;
        out->acpi_address = fadt->address;
        out->acpi_value = fadt->value;
    }
}

enum platform_reset_status platform_reset_plan_current(
    struct platform_reset_plan *out)
{
    struct platform_reset_environment environment;

    if (out == NULL) {
        return PLATFORM_RESET_NULL_ARGUMENT;
    }
    platform_reset_environment_current(&environment);
    return platform_reset_plan_build(&environment, out);
}

/*
 * A wait on the PIT channel this function puts in a counting state itself.
 *
 * ITEM-417: the wait used to sample "the channel the timer programmed" and
 * assumed that channel was counting.  In the installer path it is not: the
 * flow runs before timer_init programs channel 0, the firmware leaves the
 * counter wherever it left it, and a stopped counter reads the same value
 * twice.  Every delta was zero, the sampling bound ended the wait after
 * 200000 useless port reads, and the countdown this item promises on the
 * screen lasted 0.318 s against the 5 s the completion record announced
 * (measured, QEMU, 2026-09-15).  A delay that a caller cannot trust is worse
 * than no delay: the operator is told to pull the medium while the machine
 * restarts, and the machine must not restart before they can read it.
 *
 * So the wait owns its time base: channel 0 is programmed as a rate generator
 * at the counter's own base frequency, which makes every count one
 * 1/1193182 s tick whatever the firmware did, and the wait ends when the
 * counts a delay of `milliseconds` lasts are in.  A stopped counter is still
 * not something a caller may hang on, so a counter that reads the same value
 * for PLATFORM_RESET_DELAY_STALL_MAX consecutive samples - far longer than a
 * live counter can hold one value - ends the wait.
 */
#define PLATFORM_RESET_DELAY_STALL_MAX UINT64_C(100000)

/*
 * Channel 0, lobyte/hibyte, mode 2 (rate generator), binary.  The reload value
 * is zero, which the counter reads as 65536: the full 16-bit range at the base
 * frequency, so accumulated counts are the base-frequency time base the
 * header's platform_reset_pit_counts_for_ms() describes.
 */
#define PLATFORM_RESET_PIT_MODE_RATE UINT8_C(0x34)
#define PLATFORM_RESET_PIT_RELOAD_FULL UINT8_C(0x00)

static uint16_t platform_reset_pit_read(void) {
    uint8_t low;
    uint8_t high;

    outb(PLATFORM_RESET_PIT_COMMAND_PORT,
         PLATFORM_RESET_PIT_LATCH_CHANNEL0);
    low = inb(PLATFORM_RESET_PIT_CHANNEL0_PORT);
    high = inb(PLATFORM_RESET_PIT_CHANNEL0_PORT);
    return (uint16_t)(((uint16_t)high << 8U) | (uint16_t)low);
}

static void platform_reset_pit_start(void) {
    outb(PLATFORM_RESET_PIT_COMMAND_PORT, PLATFORM_RESET_PIT_MODE_RATE);
    outb(PLATFORM_RESET_PIT_CHANNEL0_PORT, PLATFORM_RESET_PIT_RELOAD_FULL);
    outb(PLATFORM_RESET_PIT_CHANNEL0_PORT, PLATFORM_RESET_PIT_RELOAD_FULL);
}

bool platform_reset_delay_ms_checked(uint32_t milliseconds) {
    const uint64_t target = platform_reset_pit_counts_for_ms(milliseconds);
    uint64_t elapsed = 0U;
    uint64_t stalled = 0U;
    uint16_t previous;

    if (milliseconds == 0U) {
        return true;
    }
    platform_reset_pit_start();
    previous = platform_reset_pit_read();
    while (elapsed < target) {
        const uint16_t current = platform_reset_pit_read();
        const uint32_t delta = platform_reset_pit_delta(previous, current);

        previous = current;
        if (delta == 0U) {
            if (++stalled >= PLATFORM_RESET_DELAY_STALL_MAX) {
                return false;
            }
            continue;
        }
        stalled = 0U;
        elapsed += delta;
    }
    return true;
}

void platform_reset_delay_ms(uint32_t milliseconds) {
    (void)platform_reset_delay_ms_checked(milliseconds);
}

void platform_reset_execute(const struct platform_reset_plan *plan) {
    struct platform_reset_environment environment;
    struct platform_reset_plan fallback;
    size_t count;
    size_t index;

    if (plan == NULL || plan->count == 0U) {
        fallback.count = 1U;
        fallback.steps[0] = PLATFORM_RESET_MECHANISM_FAULT;
        plan = &fallback;
    }
    count = plan->count;
    if (count > PLATFORM_RESET_STEP_CAPACITY) {
        count = PLATFORM_RESET_STEP_CAPACITY;
    }
    platform_reset_environment_current(&environment);
    platform_reset_publish_plan(plan, &environment);

    for (index = 0U; index < count; ++index) {
        const enum platform_reset_mechanism mechanism = plan->steps[index];

        platform_reset_publish_attempt(index, count, &environment, mechanism);
        switch (mechanism) {
        case PLATFORM_RESET_MECHANISM_ACPI:
            platform_reset_acpi_write(&environment);
            platform_reset_delay_ms(PLATFORM_RESET_ACPI_SETTLE_MS);
            break;
        case PLATFORM_RESET_MECHANISM_PCI:
            platform_reset_pci_write();
            platform_reset_delay_ms(PLATFORM_RESET_PCI_SETTLE_MS);
            break;
        case PLATFORM_RESET_MECHANISM_I8042:
            platform_reset_i8042_write();
            platform_reset_delay_ms(PLATFORM_RESET_I8042_SETTLE_MS);
            break;
        case PLATFORM_RESET_MECHANISM_FAULT:
            platform_reset_triple_fault();
            break;
        case PLATFORM_RESET_MECHANISM_NONE:
            break;
        }
    }
    /* Unreachable: the plan's last step is the fault, which does not return. */
    platform_reset_triple_fault();
}

#endif /* HFOC_PLATFORM_RESET_HOST_TEST */
