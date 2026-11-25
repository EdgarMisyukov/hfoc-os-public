#ifndef HFOC_LAPIC_H
#define HFOC_LAPIC_H

#include <stdbool.h>
#include <stdint.h>

struct limine_memmap_response;
struct acpi_madt_info;

enum lapic_access_mode {
    LAPIC_ACCESS_XAPIC_MMIO,
    LAPIC_ACCESS_X2APIC_MSR,
};

struct lapic_cpu_state {
    volatile uintptr_t mmio_base;
    uint8_t apic_id;
    enum lapic_access_mode access_mode;
    bool initialized;
    bool x2apic_published;
};

enum lapic_status {
    LAPIC_OK = 0,
    LAPIC_NO_CPUID_APIC = 1,
    LAPIC_NO_ACPI_SNAPSHOT = 2,
    LAPIC_BAD_LAPIC_ADDRESS = 3,
    LAPIC_X2APIC_DETECTED = 4,
    LAPIC_MSR_BASE_MISMATCH = 5,
    LAPIC_HHDM_OVERFLOW = 6,
    LAPIC_RANGE_NOT_MAPPED = 7,
    LAPIC_BAD_VERSION = 8,
    LAPIC_ENABLE_FAILED = 9,
    LAPIC_MMIO_MAP_FAILED = 10,
    LAPIC_LINT0_FAILED = 11,
    LAPIC_DISABLE_FAILED = 12,
    LAPIC_APIC_ID_MISMATCH = 13,
    LAPIC_AP_READBACK_FAILED = 14,
};

enum lapic_x2apic_status {
    LAPIC_X2APIC_OK = 0,
    LAPIC_X2APIC_UNSUPPORTED = 1,
    LAPIC_X2APIC_NOT_READY = 2,
    LAPIC_X2APIC_ALREADY_ACTIVE = 3,
    LAPIC_X2APIC_ENABLE_READBACK_FAILED = 4,
    LAPIC_X2APIC_ROLLBACK_FAILED = 5,
    LAPIC_X2APIC_STATE_READBACK_FAILED = 6,
};

enum lapic_status lapic_init(
    const struct acpi_madt_info *madt,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset);
enum lapic_status lapic_init_ap(
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset,
    uint32_t expected_lapic_id);

void lapic_eoi(void);
bool lapic_is_enabled(void);
bool lapic_x2apic_capable(void);
enum lapic_x2apic_status lapic_x2apic_transition(void);
enum lapic_x2apic_status lapic_x2apic_rollback(void);
bool lapic_x2apic_active(void);
const char *lapic_x2apic_status_string(enum lapic_x2apic_status status);
bool lapic_timer_hw_arm(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_hw_current_count(uint32_t *out_count);
bool lapic_timer_hw_program_masked_periodic(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_hw_readback(
    uint32_t *out_lvt,
    uint32_t *out_divide,
    uint32_t *out_initial_count);
bool lapic_timer_hw_unmask_periodic(uint8_t vector);
bool lapic_timer_hw_mask_periodic(uint8_t vector);
bool lapic_timer_hw_program_masked_oneshot(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_hw_unmask_oneshot(uint8_t vector);
uint8_t lapic_bsp_apic_id(void);
const char *lapic_status_string(enum lapic_status status);

#ifdef HFOC_LAPIC_HOST_TEST
/* Test-only API: reset internal static state between test cases.
 * Defined in kernel/src/lapic.c; called by the test harness. */
void lapic_test_reset(void);
#endif

#endif
