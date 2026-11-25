#include "lapic.h"
#include "acpi.h"
#include "cpu.h"
#include "serial.h"
#include "vmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IA32_APIC_BASE_MSR   0x1BU
#define IA32_APIC_BASE_EN    (1ULL << 11U)
#define IA32_APIC_BASE_EXTD  (1ULL << 10U)
#define LAPIC_LVT_MASK       (1U << 16U)

static struct lapic_cpu_state *lapic_current_state(void) {
    return &cpu_current()->lapic;
}

/* --- MSR / MMIO / CPUID primitives --- */

#ifdef HFOC_LAPIC_HOST_TEST
/* Host test mocks are provided by tests/lapic-host.c with external linkage. */
enum vmm_status lapic_test_vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual);

uint64_t host_rdmsr(uint32_t msr);
void     host_wrmsr(uint32_t msr, uint64_t value);
uint32_t host_mmio_read32(uint64_t offset);
void     host_mmio_write32(uint64_t offset, uint32_t value);
bool     host_cpuid_has_apic_flag(void);
bool     host_cpuid_has_x2apic_flag(void);

void lapic_test_reset(void) {
    struct lapic_cpu_state *state = lapic_current_state();
    state->initialized = false;
    state->mmio_base = 0U;
    state->apic_id = 0U;
    state->access_mode = LAPIC_ACCESS_XAPIC_MMIO;
    state->x2apic_published = false;
}

#define cpuid_has_apic() host_cpuid_has_apic_flag()
#define cpuid_has_x2apic() host_cpuid_has_x2apic_flag()
#define rdmsr(msr)       host_rdmsr(msr)
#define wrmsr(msr, val)  host_wrmsr(msr, val)
#define mmio_read32(off)     host_mmio_read32(off)
#define mmio_write32(off, v) host_mmio_write32(off, v)

#else

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32U) | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low  = (uint32_t)(value & 0xFFFFFFFFULL);
    uint32_t high = (uint32_t)(value >> 32U);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static uint32_t mmio_read32(uintptr_t offset) {
    const volatile uint32_t *reg =
        (const volatile uint32_t *)(lapic_current_state()->mmio_base + offset);
    return *reg;
}

static void mmio_write32(uintptr_t offset, uint32_t value) {
    volatile uint32_t *reg =
        (volatile uint32_t *)(lapic_current_state()->mmio_base + offset);
    *reg = value;
}

static bool cpuid_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1U), "c"(0U));
    (void)ebx;
    (void)ecx;
    return (edx & (1U << 9U)) != 0U;
}

static bool cpuid_has_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1U));
    (void)eax;
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 21U)) != 0U;
}

#endif

static uint32_t x2apic_read32(uintptr_t offset) {
    return (uint32_t)rdmsr(0x800U + (offset >> 4U));
}

static void x2apic_write32(uintptr_t offset, uint32_t value) {
    wrmsr(0x800U + (offset >> 4U), value);
}

static uint32_t apic_read32(uintptr_t offset) {
    if (lapic_current_state()->access_mode == LAPIC_ACCESS_X2APIC_MSR) {
        return x2apic_read32(offset);
    }
    return mmio_read32(offset);
}

static void apic_write32(uintptr_t offset, uint32_t value) {
    if (lapic_current_state()->access_mode == LAPIC_ACCESS_X2APIC_MSR) {
        x2apic_write32(offset, value);
        return;
    }
    mmio_write32(offset, value);
}

static bool x2apic_restore_msr(uintptr_t offset, uint32_t value) {
    x2apic_write32(offset, value);
    return x2apic_read32(offset) == value;
}

static bool xapic_restore_mmio(uintptr_t offset, uint32_t value) {
    mmio_write32(offset, value);
    return mmio_read32(offset) == value;
}

struct lapic_x2apic_snapshot {
    uint64_t base;
    uint32_t svr;
    uint32_t lint0;
    uint32_t lvt;
    uint32_t divide;
    uint32_t initial_count;
};

static enum lapic_x2apic_status x2apic_rollback(
    const struct lapic_x2apic_snapshot *snapshot)
{
    struct lapic_cpu_state *state = lapic_current_state();
    state->x2apic_published = false;

    const uint64_t disabled =
        snapshot->base & ~(IA32_APIC_BASE_EN | IA32_APIC_BASE_EXTD);
    wrmsr(IA32_APIC_BASE_MSR, disabled);
    uint64_t readback = rdmsr(IA32_APIC_BASE_MSR);
    if ((readback & IA32_APIC_BASE_EN) != 0ULL ||
        (readback & IA32_APIC_BASE_EXTD) != 0ULL) {
        return LAPIC_X2APIC_ROLLBACK_FAILED;
    }

    const uint64_t xapic =
        (snapshot->base & ~IA32_APIC_BASE_EXTD) | IA32_APIC_BASE_EN;
    wrmsr(IA32_APIC_BASE_MSR, xapic);
    readback = rdmsr(IA32_APIC_BASE_MSR);
    if ((readback & IA32_APIC_BASE_EN) == 0ULL ||
        (readback & IA32_APIC_BASE_EXTD) != 0ULL) {
        return LAPIC_X2APIC_ROLLBACK_FAILED;
    }

    state->access_mode = LAPIC_ACCESS_XAPIC_MMIO;
    if (!xapic_restore_mmio(0x0F0U, snapshot->svr) ||
        !xapic_restore_mmio(0x350U, snapshot->lint0) ||
        !xapic_restore_mmio(0x320U, snapshot->lvt) ||
        !xapic_restore_mmio(0x3E0U, snapshot->divide) ||
        !xapic_restore_mmio(0x380U, snapshot->initial_count)) {
        return LAPIC_X2APIC_ROLLBACK_FAILED;
    }

    return LAPIC_X2APIC_OK;
}

/* --- Main logic --- */

enum lapic_status lapic_init(
    const struct acpi_madt_info *madt,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset)
{
    (void)memory_map; /* memory-map pre-check removed; VMM is the guard */
    struct lapic_cpu_state *state = lapic_current_state();

    /* 1. Idempotence */
    if (lapic_is_enabled()) return LAPIC_OK;

    /* 2. NULL madt */
    if (madt == NULL) return LAPIC_NO_ACPI_SNAPSHOT;

    /* 3. CPUID APIC flag */
    if (!cpuid_has_apic()) return LAPIC_NO_CPUID_APIC;

    /* 4. MADT lapic_address validation */
    if (madt->lapic_address == 0ULL
        || (madt->lapic_address & 0xFFFULL) != 0ULL)
        return LAPIC_BAD_LAPIC_ADDRESS;

    /* 5. MSR read: x2APIC check */
    uint64_t msr_val = rdmsr(0x1B);
    if (msr_val & (1ULL << 10U)) return LAPIC_X2APIC_DETECTED;

    /* 6. MSR base vs MADT */
    const uint64_t msr_base = msr_val & 0x0000000FFFFFF000ULL;
    if (msr_base != madt->lapic_address) return LAPIC_MSR_BASE_MISMATCH;

    /* 7. HHDM overflow */
    if (msr_base > UINT64_MAX - hhdm_offset) return LAPIC_HHDM_OVERFLOW;

    /* 8. VMM map (before any MSR write) */
    volatile void *mmio = NULL;
    enum vmm_status vmm_st;
#ifdef HFOC_LAPIC_HOST_TEST
    vmm_st = lapic_test_vmm_map_mmio_page(msr_base, hhdm_offset, &mmio);
#else
    vmm_st = vmm_map_mmio_page(msr_base, hhdm_offset, &mmio);
#endif
    if (vmm_st != VMM_OK) {
        serial_write("[LAPIC] unavailable: MMIO map failed (");
        serial_write(vmm_status_string(vmm_st));
        serial_write("); legacy PIC retained\n");
        return LAPIC_MMIO_MAP_FAILED;
    }
    state->mmio_base = (uintptr_t)mmio;

    /* 9. Save original MSR value for possible rollback */
    const uint64_t original_msr = msr_val;

    /* 10. Write IA32_APIC_BASE with bit 11 (APIC Global Enable) */
    wrmsr(0x1B, msr_val | (1ULL << 11U));

    /* 11. Read-back: verify bit 11 */
    msr_val = rdmsr(0x1B);
    if ((msr_val & (1ULL << 11U)) == 0ULL) {
        state->mmio_base = 0U;
        return LAPIC_ENABLE_FAILED;
    }

    /* 12. MMIO read APIC Version (offset 0x030) */
    const uint32_t version_reg = mmio_read32(0x030U);
    const uint8_t version = (uint8_t)(version_reg & 0xFFU);
    if (version == 0U) {
        wrmsr(0x1B, original_msr); /* rollback */
        state->mmio_base = 0U;
        return LAPIC_BAD_VERSION;
    }

    /* 13. MMIO read APIC ID (offset 0x020) — 0..255 valid */
    const uint32_t id_reg = mmio_read32(0x020U);
    const uint8_t apic_id = (uint8_t)(id_reg >> 24U);

    /* 14. Configure LINT0 to ExtINT and unmasked before publishing. */
    const uint32_t original_lint0 = mmio_read32(0x350U);
    const uint32_t configured_lint0 =
        (original_lint0 & ~((7U << 8U) | (1U << 16U))) | (7U << 8U);
    mmio_write32(0x350U, configured_lint0);
    const uint32_t read_lint0 = mmio_read32(0x350U);

    const bool extint_ok = ((read_lint0 >> 8U) & 0x7U) == 0x7U;
    const bool unmasked_ok = (read_lint0 & (1U << 16U)) == 0U;
    if (!extint_ok || !unmasked_ok) {
        mmio_write32(0x350U, original_lint0); /* best-effort restore */
        const uint64_t disable_value = original_msr & ~(1ULL << 11U);
        wrmsr(0x1B, disable_value);
        const uint64_t disable_readback = rdmsr(0x1B);
        state->mmio_base = 0U;
        if ((disable_readback & (1ULL << 11U)) == 0ULL) {
            serial_write("[LAPIC] LINT0 failed: status=11; "
                         "legacy PIC retained\n");
            return LAPIC_LINT0_FAILED;
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[LAPIC] LINT0 failed: status=12; Local APIC disable "
                         "verification failed; ");
            serial_write("legacy PIC availability unverified\n");
            serial_sequence_end(serial_flags);
        }
        return LAPIC_DISABLE_FAILED;
    }

    /* 15. Program Spurious Interrupt Vector Register (offset 0x0F0) */
    const uint32_t svr_val = mmio_read32(0x0F0U);
    mmio_write32(0x0F0U, (svr_val & 0xFFFFFF00U) | 0xEFU | (1U << 8U));

    /* 16. Publish enabled state */
    state->initialized = true;
    state->apic_id = apic_id;

    /* 17. COM1 diagnostics */
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[LAPIC] enabled: id=");
        serial_write_u64(apic_id);
        serial_write(" version=");
        serial_write_u64(version);
        serial_write(" base=");
        serial_write_hex(msr_base);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    serial_write("[LAPIC] SVR configured: vector=239; legacy PIC retained\n");
    serial_write("[LAPIC] LINT0 configured: delivery=ExtINT mask=0; "
                 "legacy PIC fallback ready\n");

    return LAPIC_OK;
}

enum lapic_status lapic_init_ap(
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset,
    uint32_t expected_lapic_id)
{
    struct lapic_cpu_state *state = lapic_current_state();
    if (state->initialized) {
        return LAPIC_OK;
    }
    if (madt == NULL) {
        return LAPIC_NO_ACPI_SNAPSHOT;
    }
    if (madt->lapic_address == 0ULL ||
        (madt->lapic_address & 0xFFFULL) != 0ULL) {
        return LAPIC_BAD_LAPIC_ADDRESS;
    }
    if (!cpuid_has_apic()) {
        return LAPIC_NO_CPUID_APIC;
    }

    uint64_t msr_val = rdmsr(IA32_APIC_BASE_MSR);
    if ((msr_val & IA32_APIC_BASE_EXTD) != 0ULL) {
        return LAPIC_X2APIC_DETECTED;
    }
    const uint64_t msr_base = msr_val & 0x0000000FFFFFF000ULL;
    if (msr_base != madt->lapic_address) {
        return LAPIC_MSR_BASE_MISMATCH;
    }
    if (msr_base > UINT64_MAX - hhdm_offset) {
        return LAPIC_HHDM_OVERFLOW;
    }

    volatile void *mmio = NULL;
    enum vmm_status vmm_st;
#ifdef HFOC_LAPIC_HOST_TEST
    vmm_st = lapic_test_vmm_map_mmio_page(msr_base, hhdm_offset, &mmio);
#else
    vmm_st = vmm_map_mmio_page(msr_base, hhdm_offset, &mmio);
#endif
    if (vmm_st != VMM_OK) {
        return LAPIC_MMIO_MAP_FAILED;
    }
    state->mmio_base = (uintptr_t)mmio;

    const uint64_t original_msr = msr_val;
    if ((msr_val & IA32_APIC_BASE_EN) == 0ULL) {
        wrmsr(IA32_APIC_BASE_MSR, msr_val | IA32_APIC_BASE_EN);
        msr_val = rdmsr(IA32_APIC_BASE_MSR);
        if ((msr_val & IA32_APIC_BASE_EN) == 0ULL) {
            state->mmio_base = 0U;
            return LAPIC_ENABLE_FAILED;
        }
    }

    const uint32_t version_reg = mmio_read32(0x030U);
    const uint8_t version = (uint8_t)(version_reg & 0xFFU);
    if (version == 0U) {
        if ((original_msr & IA32_APIC_BASE_EN) == 0ULL) {
            wrmsr(IA32_APIC_BASE_MSR, original_msr);
        }
        state->mmio_base = 0U;
        return LAPIC_BAD_VERSION;
    }

    const uint32_t id_reg = mmio_read32(0x020U);
    const uint8_t apic_id = (uint8_t)(id_reg >> 24U);
    if (apic_id != expected_lapic_id) {
        if ((original_msr & IA32_APIC_BASE_EN) == 0ULL) {
            wrmsr(IA32_APIC_BASE_MSR, original_msr);
        }
        state->mmio_base = 0U;
        return LAPIC_APIC_ID_MISMATCH;
    }

    const uint32_t svr =
        (mmio_read32(0x0F0U) & 0xFFFFFF00U) | 0xEFU | (1U << 8U);
    mmio_write32(0x0F0U, svr);
    if (mmio_read32(0x0F0U) != svr) {
        state->mmio_base = 0U;
        serial_write("[SMP ] AP LAPIC SVR readback failed\n");
        return LAPIC_AP_READBACK_FAILED;
    }

    static const uintptr_t lvt_offsets[] = {
        0x320U, 0x330U, 0x340U, 0x350U, 0x360U, 0x370U
    };
    const uint8_t max_lvt = (uint8_t)((version_reg >> 16U) & 0xFFU);
    size_t lvt_count = sizeof(lvt_offsets) / sizeof(lvt_offsets[0]);
    if ((size_t)max_lvt + 1U < lvt_count) {
        lvt_count = (size_t)max_lvt + 1U;
    }
    for (size_t index = 0U; index < lvt_count;
         ++index) {
        const uintptr_t offset = lvt_offsets[index];
        const uint32_t masked = mmio_read32(offset) | LAPIC_LVT_MASK;
        mmio_write32(offset, masked);
        if (mmio_read32(offset) != masked) {
            state->mmio_base = 0U;
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[SMP ] AP LAPIC LVT readback failed: ");
                serial_write_hex(offset);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            return LAPIC_AP_READBACK_FAILED;
        }
    }
    if (max_lvt >= 6U) {
        const uintptr_t offset = 0x2F0U;
        const uint32_t masked = mmio_read32(offset) | LAPIC_LVT_MASK;
        mmio_write32(offset, masked);
        if (mmio_read32(offset) != masked) {
            state->mmio_base = 0U;
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[SMP ] AP LAPIC LVT readback failed: ");
                serial_write_hex(offset);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            return LAPIC_AP_READBACK_FAILED;
        }
    }

    state->initialized = true;
    state->apic_id = apic_id;
    state->access_mode = LAPIC_ACCESS_XAPIC_MMIO;
    state->x2apic_published = false;

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SMP ] AP LAPIC ready: id=");
        serial_write_u64(apic_id);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    return LAPIC_OK;
}

void lapic_eoi(void) {
    if (!lapic_is_enabled()) return;
    apic_write32(0x0B0U, 0U);
}

bool lapic_timer_hw_arm(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U, (uint32_t)vector | (1U << 16U));
    apic_write32(0x3E0U, divide_config);
    apic_write32(0x380U, initial_count);
    return true;
}

bool lapic_timer_hw_current_count(uint32_t *out_count) {
    if (!lapic_is_enabled() || out_count == NULL) {
        return false;
    }
    *out_count = apic_read32(0x390U);
    return true;
}

bool lapic_timer_hw_program_masked_periodic(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U,
                 (uint32_t)vector | (1U << 17U) | (1U << 16U));
    apic_write32(0x3E0U, divide_config);
    apic_write32(0x380U, initial_count);
    return true;
}

bool lapic_timer_hw_readback(
    uint32_t *out_lvt,
    uint32_t *out_divide,
    uint32_t *out_initial_count)
{
    if (!lapic_is_enabled() ||
        out_lvt == NULL ||
        out_divide == NULL ||
        out_initial_count == NULL) {
        return false;
    }
    *out_lvt = apic_read32(0x320U);
    *out_divide = apic_read32(0x3E0U);
    *out_initial_count = apic_read32(0x380U);
    return true;
}

bool lapic_timer_hw_unmask_periodic(uint8_t vector) {
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U, (uint32_t)vector | (1U << 17U));
    return true;
}

bool lapic_timer_hw_mask_periodic(uint8_t vector) {
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U,
                 (uint32_t)vector | (1U << 17U) | (1U << 16U));
    return true;
}

bool lapic_timer_hw_program_masked_oneshot(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U, (uint32_t)vector | (1U << 16U));
    apic_write32(0x3E0U, divide_config);
    apic_write32(0x380U, initial_count);
    return true;
}

bool lapic_timer_hw_unmask_oneshot(uint8_t vector) {
    if (!lapic_is_enabled()) {
        return false;
    }
    apic_write32(0x320U, (uint32_t)vector);
    return true;
}

bool lapic_is_enabled(void) {
    return lapic_current_state()->initialized;
}

bool lapic_x2apic_capable(void) {
    return cpuid_has_x2apic();
}

enum lapic_x2apic_status lapic_x2apic_transition(void) {
    struct lapic_cpu_state *state = lapic_current_state();
    if (!state->initialized) {
        return LAPIC_X2APIC_NOT_READY;
    }
    if (state->access_mode == LAPIC_ACCESS_X2APIC_MSR) {
        return LAPIC_X2APIC_ALREADY_ACTIVE;
    }
    if (!cpuid_has_x2apic()) {
        return LAPIC_X2APIC_UNSUPPORTED;
    }

    const uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & IA32_APIC_BASE_EN) == 0ULL ||
        (base & IA32_APIC_BASE_EXTD) != 0ULL) {
        return LAPIC_X2APIC_NOT_READY;
    }

    struct lapic_x2apic_snapshot snapshot;
    snapshot.base = base;
    snapshot.svr = apic_read32(0x0F0U);
    snapshot.lint0 = apic_read32(0x350U);
    snapshot.lvt = apic_read32(0x320U);
    snapshot.divide = apic_read32(0x3E0U);
    snapshot.initial_count = apic_read32(0x380U);

    const uint32_t masked_lvt = snapshot.lvt | LAPIC_LVT_MASK;
    apic_write32(0x320U, masked_lvt);
    if (apic_read32(0x320U) != masked_lvt) {
        return LAPIC_X2APIC_STATE_READBACK_FAILED;
    }

    wrmsr(IA32_APIC_BASE_MSR,
          base | IA32_APIC_BASE_EN | IA32_APIC_BASE_EXTD);
    uint64_t readback = rdmsr(IA32_APIC_BASE_MSR);
    if ((readback & IA32_APIC_BASE_EN) == 0ULL ||
        (readback & IA32_APIC_BASE_EXTD) == 0ULL) {
        const enum lapic_x2apic_status rollback =
            x2apic_rollback(&snapshot);
        if (rollback != LAPIC_X2APIC_OK) {
            return LAPIC_X2APIC_ROLLBACK_FAILED;
        }
        return LAPIC_X2APIC_ENABLE_READBACK_FAILED;
    }

    const uint32_t version_reg = x2apic_read32(0x030U);
    const uint32_t id_reg = x2apic_read32(0x020U);
    const uint8_t version = (uint8_t)(version_reg & 0xFFU);
    const uint8_t apic_id = (uint8_t)(id_reg >> 24U);
    if (version == 0U || apic_id != state->apic_id) {
        const enum lapic_x2apic_status rollback =
            x2apic_rollback(&snapshot);
        if (rollback != LAPIC_X2APIC_OK) {
            return LAPIC_X2APIC_ROLLBACK_FAILED;
        }
        return LAPIC_X2APIC_STATE_READBACK_FAILED;
    }

    if (!x2apic_restore_msr(0x0F0U, snapshot.svr) ||
        !x2apic_restore_msr(0x350U, snapshot.lint0) ||
        !x2apic_restore_msr(0x3E0U, snapshot.divide) ||
        !x2apic_restore_msr(0x380U, snapshot.initial_count) ||
        !x2apic_restore_msr(0x320U, snapshot.lvt)) {
        const enum lapic_x2apic_status rollback =
            x2apic_rollback(&snapshot);
        if (rollback != LAPIC_X2APIC_OK) {
            return LAPIC_X2APIC_ROLLBACK_FAILED;
        }
        return LAPIC_X2APIC_STATE_READBACK_FAILED;
    }

    state->access_mode = LAPIC_ACCESS_X2APIC_MSR;
    state->x2apic_published = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[X2APIC] enabled: id=");
        serial_write_u64(state->apic_id);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    serial_write("[X2APIC] active; xAPIC MMIO retained as fallback\n");
    return LAPIC_X2APIC_OK;
}

enum lapic_x2apic_status lapic_x2apic_rollback(void) {
    struct lapic_cpu_state *state = lapic_current_state();
    if (!state->initialized) {
        return LAPIC_X2APIC_NOT_READY;
    }
    if (!state->x2apic_published) {
        return LAPIC_X2APIC_OK;
    }

    struct lapic_x2apic_snapshot snapshot;
    snapshot.base = rdmsr(IA32_APIC_BASE_MSR);
    snapshot.svr = apic_read32(0x0F0U);
    snapshot.lint0 = apic_read32(0x350U);
    snapshot.lvt = apic_read32(0x320U);
    snapshot.divide = apic_read32(0x3E0U);
    snapshot.initial_count = apic_read32(0x380U);

    return x2apic_rollback(&snapshot);
}

bool lapic_x2apic_active(void) {
    return lapic_current_state()->x2apic_published;
}

const char *lapic_x2apic_status_string(enum lapic_x2apic_status status) {
    switch (status) {
    case LAPIC_X2APIC_OK:                     return "OK";
    case LAPIC_X2APIC_UNSUPPORTED:            return "unsupported";
    case LAPIC_X2APIC_NOT_READY:              return "not ready";
    case LAPIC_X2APIC_ALREADY_ACTIVE:         return "already active";
    case LAPIC_X2APIC_ENABLE_READBACK_FAILED: return "enable readback failed";
    case LAPIC_X2APIC_ROLLBACK_FAILED:        return "rollback failed";
    case LAPIC_X2APIC_STATE_READBACK_FAILED:  return "state readback failed";
    default:                                  return "unknown";
    }
}

uint8_t lapic_bsp_apic_id(void) {
    struct lapic_cpu_state *state = lapic_current_state();
    return state->initialized ? state->apic_id : 0U;
}

const char *lapic_status_string(enum lapic_status status) {
    switch (status) {
    case LAPIC_OK:                  return "OK";
    case LAPIC_NO_CPUID_APIC:       return "no CPUID APIC";
    case LAPIC_NO_ACPI_SNAPSHOT:    return "no ACPI snapshot";
    case LAPIC_BAD_LAPIC_ADDRESS:   return "bad LAPIC address";
    case LAPIC_X2APIC_DETECTED:     return "x2APIC detected";
    case LAPIC_MSR_BASE_MISMATCH:   return "MSR base mismatch";
    case LAPIC_HHDM_OVERFLOW:       return "HHDM overflow";
    case LAPIC_RANGE_NOT_MAPPED:    return "range not mapped";
    case LAPIC_BAD_VERSION:         return "bad version";
    case LAPIC_ENABLE_FAILED:       return "enable failed";
    case LAPIC_MMIO_MAP_FAILED:     return "MMIO map failed";
    case LAPIC_LINT0_FAILED:        return "LINT0 failed";
    case LAPIC_DISABLE_FAILED:      return "disable failed";
    case LAPIC_APIC_ID_MISMATCH:    return "APIC ID mismatch";
    case LAPIC_AP_READBACK_FAILED:  return "AP readback failed";
    default:                        return "unknown";
    }
}
