#ifndef HFOC_IOAPIC_H
#define HFOC_IOAPIC_H

#include <stdbool.h>
#include <stdint.h>

struct acpi_madt_info;

enum ioapic_status {
    IOAPIC_OK = 0,
    IOAPIC_NO_ACPI_SNAPSHOT = 1,
    IOAPIC_NO_IOAPIC = 2,
    IOAPIC_BAD_MMIO_ADDRESS = 3,
    IOAPIC_HHDM_OVERFLOW = 4,
    IOAPIC_MMIO_MAP_FAILED = 5,
    IOAPIC_BAD_VERSION = 6,
    IOAPIC_BAD_ISO = 7,
    IOAPIC_NOT_INITIALIZED = 8,
    IOAPIC_LAPIC_NOT_ENABLED = 9,
    IOAPIC_UNSUPPORTED_IRQ = 10,
    IOAPIC_ROUTE_FAILED = 11,
};

#define IOAPIC_IRQ0_VECTOR 0x40U
#define IOAPIC_IRQ1_VECTOR 0x41U
#define IOAPIC_ISA_IRQ_COUNT 16U

enum ioapic_status ioapic_init(
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset);

/* IOAPIC route API for IRQ0 and IRQ1; implementation lives in ioapic.c. */
enum ioapic_status ioapic_route_irq(
    uint8_t irq,
    uint8_t vector);
enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector);

bool ioapic_is_probed(void);
bool ioapic_is_isa_irq_routed(uint8_t irq);
bool ioapic_is_irq1_routed(void);
bool ioapic_is_irq0_routed(void);
bool ioapic_irq0_mask_save(uint64_t *out_previous_rte);
bool ioapic_irq0_restore(uint64_t previous_rte);
const char *ioapic_status_string(enum ioapic_status status);

#ifdef HFOC_IOAPIC_HOST_TEST
/* Test-only API: reset internal static state between test cases.
 * Defined in kernel/src/ioapic.c; called by the test harness. */
void ioapic_test_reset(void);
bool ioapic_test_isa_irq_desc_valid(uint8_t irq);
uint32_t ioapic_test_isa_irq_gsi(uint8_t irq);
#endif

#endif
