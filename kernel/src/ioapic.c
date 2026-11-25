#include "ioapic.h"

#include "acpi.h"
#include "lapic.h"
#include "serial.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ioapic_route_desc {
    bool valid;
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
};

static bool probed;
static uintptr_t ioapic_mmio_base;
static uint8_t ioapic_id;
static uint32_t ioapic_gsi_base;
static uint8_t ioapic_version;
static uint16_t ioapic_redirection_entries;
static struct ioapic_route_desc isa_desc[IOAPIC_ISA_IRQ_COUNT];
static bool isa_irq_routed[IOAPIC_ISA_IRQ_COUNT];

#ifdef HFOC_IOAPIC_HOST_TEST
enum vmm_status ioapic_test_vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual);

uint32_t host_ioapic_read32(uintptr_t offset);
void host_ioapic_write32(uintptr_t offset, uint32_t value);
bool host_lapic_is_enabled(void);
uint8_t host_lapic_bsp_apic_id(void);

#define ioapic_map(phys, hhdm, out) \
    ioapic_test_vmm_map_mmio_page(phys, hhdm, out)
#define ioapic_read32(off) host_ioapic_read32(off)
#define ioapic_write32(off, val) host_ioapic_write32(off, val)
#define lapic_is_enabled host_lapic_is_enabled
#define lapic_bsp_apic_id host_lapic_bsp_apic_id

void ioapic_test_reset(void) {
    probed = false;
    ioapic_mmio_base = 0U;
    ioapic_id = 0U;
    ioapic_gsi_base = 0U;
    ioapic_version = 0U;
    ioapic_redirection_entries = 0U;
    for (size_t i = 0U; i < IOAPIC_ISA_IRQ_COUNT; ++i) {
        isa_desc[i].valid = false;
        isa_desc[i].gsi = 0U;
        isa_desc[i].active_low = false;
        isa_desc[i].level_triggered = false;
        isa_irq_routed[i] = false;
    }
}

void ioapic_test_invalidate_irq1_desc(void) {
    isa_desc[1U].valid = false;
}

void ioapic_test_invalidate_irq0_desc(void) {
    isa_desc[0U].valid = false;
}

bool ioapic_test_irq_active_low(uint8_t irq) {
    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return false;
    }
    return isa_desc[irq].active_low;
}

bool ioapic_test_irq_level_triggered(uint8_t irq) {
    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return false;
    }
    return isa_desc[irq].level_triggered;
}

uint32_t ioapic_test_irq_gsi(uint8_t irq) {
    if (irq >= IOAPIC_ISA_IRQ_COUNT) {
        return 0U;
    }
    return isa_desc[irq].gsi;
}

bool ioapic_test_isa_irq_desc_valid(uint8_t irq) {
    return irq < IOAPIC_ISA_IRQ_COUNT && isa_desc[irq].valid;
}

uint32_t ioapic_test_isa_irq_gsi(uint8_t irq) {
    return ioapic_test_irq_gsi(irq);
}

uint8_t ioapic_test_ioapic_id(void) {
    return ioapic_id;
}

uintptr_t ioapic_test_mmio_base(void) {
    return ioapic_mmio_base;
}

uint32_t ioapic_test_gsi_base(void) {
    return ioapic_gsi_base;
}

uint8_t ioapic_test_version(void) {
    return ioapic_version;
}

uint16_t ioapic_test_redirection_entries(void) {
    return ioapic_redirection_entries;
}

#else
#define ioapic_map vmm_map_mmio_page

static uint32_t ioapic_read32(uintptr_t offset) {
    const volatile uint32_t *reg =
        (const volatile uint32_t *)(ioapic_mmio_base + offset);
    return *reg;
}

static void ioapic_write32(uintptr_t offset, uint32_t value) {
    volatile uint32_t *reg =
        (volatile uint32_t *)(ioapic_mmio_base + offset);
    *reg = value;
}
#endif

static uint32_t ioapic_read_reg(uint32_t index) {
    ioapic_write32(0x00U, index);
    return ioapic_read32(0x10U);
}

static void ioapic_write_reg(uint32_t index, uint32_t value) {
    ioapic_write32(0x00U, index);
    ioapic_write32(0x10U, value);
}

static enum ioapic_status ioapic_route_fail(
    uint8_t irq,
    enum ioapic_status status)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[IOAPIC] route failed: irq=");
        serial_write_u64(irq);
        serial_write(" status=");
        serial_write_u64((uint64_t)status);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    return status;
}

static bool ioapic_find_iso(
    const struct acpi_madt_info *madt,
    uint8_t irq,
    const struct acpi_iso **out_iso)
{
    const struct acpi_iso *found = NULL;
    for (size_t i = 0; i < madt->iso_count; ++i) {
        const struct acpi_iso *iso = &madt->isos[i];
        if (iso->bus != 0U || iso->source != irq) {
            continue;
        }
        if (found != NULL) {
            return false;
        }
        found = iso;
    }
    *out_iso = found;
    return true;
}

static enum ioapic_status ioapic_resolve_route(
    const struct acpi_madt_info *madt,
    uint8_t irq,
    struct ioapic_route_desc *out)
{
    const struct acpi_iso *iso = NULL;
    if (!ioapic_find_iso(madt, irq, &iso)) {
        return IOAPIC_BAD_ISO;
    }

    uint32_t gsi = irq;
    uint16_t flags = 0x0000U;
    if (iso != NULL) {
        gsi = iso->gsi;
        flags = iso->flags;
    }

    if (gsi < ioapic_gsi_base ||
        gsi - ioapic_gsi_base >= ioapic_redirection_entries) {
        return IOAPIC_BAD_ISO;
    }

    uint16_t polarity = flags & 0x0003U;
    uint16_t trigger = (flags >> 2U) & 0x0003U;
    if ((flags & 0xFFF0U) != 0U ||
        polarity == 2U ||
        trigger == 2U) {
        return IOAPIC_BAD_ISO;
    }

    out->valid = true;
    out->gsi = gsi;
    out->active_low = (polarity == 3U);
    out->level_triggered = (trigger == 3U);
    return IOAPIC_OK;
}

enum ioapic_status ioapic_init(
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset)
{
    if (probed) {
        return IOAPIC_OK;
    }
    if (madt == NULL) {
        return IOAPIC_NO_ACPI_SNAPSHOT;
    }
    if (madt->ioapic_count == 0U) {
        return IOAPIC_NO_IOAPIC;
    }

    const struct acpi_ioapic *ioapic = &madt->ioapics[0];
    if (ioapic->mmio_address == 0U ||
        (ioapic->mmio_address & 0xFFFU) != 0U) {
        return IOAPIC_BAD_MMIO_ADDRESS;
    }
    if ((uint64_t)ioapic->mmio_address > UINT64_MAX - hhdm_offset) {
        return IOAPIC_HHDM_OVERFLOW;
    }

    volatile void *mapped = NULL;
    enum vmm_status vmm_st = ioapic_map(
        ioapic->mmio_address,
        hhdm_offset,
        &mapped);
    if (vmm_st != VMM_OK) {
        return IOAPIC_MMIO_MAP_FAILED;
    }

    ioapic_mmio_base = (uintptr_t)mapped;
    ioapic_id = ioapic->ioapic_id;
    ioapic_gsi_base = ioapic->gsi_base;

    uint32_t version = ioapic_read_reg(0x01U);
    if ((version & 0xFFU) == 0U) {
        return IOAPIC_BAD_VERSION;
    }

    ioapic_version = (uint8_t)(version & 0xFFU);
    ioapic_redirection_entries =
        (uint16_t)(((version >> 16U) & 0xFFU) + 1U);

    for (uint8_t irq = 0U; irq < IOAPIC_ISA_IRQ_COUNT; ++irq) {
        enum ioapic_status st = ioapic_resolve_route(madt, irq, &isa_desc[irq]);
        if (st != IOAPIC_OK) {
            if (irq <= 1U) {
                return st;
            }
            isa_desc[irq].valid = false;
        }
    }

    probed = true;

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[IOAPIC] probe: id=");
        serial_write_u64(ioapic_id);
        serial_write(" base=");
        serial_write_hex(ioapic_mmio_base);
        serial_write(" gsi_base=");
        serial_write_u64(ioapic_gsi_base);
        serial_write(" version=");
        serial_write_u64(ioapic_version);
        serial_write(" redirections=");
        serial_write_u64(ioapic_redirection_entries);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    return IOAPIC_OK;
}

static bool ioapic_expected_isa_vector(uint8_t irq, uint8_t *out_vector) {
    if (irq == 0U) {
        *out_vector = IOAPIC_IRQ0_VECTOR;
        return true;
    }
    if (irq == 1U) {
        *out_vector = IOAPIC_IRQ1_VECTOR;
        return true;
    }
    if (irq >= 3U && irq < IOAPIC_ISA_IRQ_COUNT) {
        *out_vector = (uint8_t)(0x41U + irq);
        return true;
    }
    return false;
}

static enum ioapic_status ioapic_route_fail_remasked(
    uint8_t irq,
    uint32_t low_index,
    uint32_t low_masked)
{
    ioapic_write_reg(low_index, low_masked);
    return ioapic_route_fail(irq, IOAPIC_ROUTE_FAILED);
}

enum ioapic_status ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector)
{
    if (!probed) {
        return ioapic_route_fail(irq, IOAPIC_NOT_INITIALIZED);
    }
    if (!lapic_is_enabled()) {
        return ioapic_route_fail(irq, IOAPIC_LAPIC_NOT_ENABLED);
    }

    uint8_t expected_vector;
    if (!ioapic_expected_isa_vector(irq, &expected_vector)) {
        return ioapic_route_fail(irq, IOAPIC_UNSUPPORTED_IRQ);
    }
    if (vector != expected_vector) {
        return ioapic_route_fail(irq, IOAPIC_ROUTE_FAILED);
    }
    if (isa_irq_routed[irq]) {
        return IOAPIC_OK;
    }

    const struct ioapic_route_desc *desc = &isa_desc[irq];
    if (!desc->valid ||
        desc->gsi < ioapic_gsi_base ||
        desc->gsi - ioapic_gsi_base >= ioapic_redirection_entries) {
        return ioapic_route_fail(irq, IOAPIC_ROUTE_FAILED);
    }

    const uint32_t index = desc->gsi - ioapic_gsi_base;
    const uint32_t low_index = 0x10U + 2U * index;
    const uint32_t high_index = 0x11U + 2U * index;
    const uint32_t low_masked =
        (uint32_t)expected_vector
        | ((uint32_t)desc->active_low << 13U)
        | ((uint32_t)desc->level_triggered << 15U)
        | (1U << 16U);
    const uint32_t high = (uint32_t)lapic_bsp_apic_id() << 24U;

    ioapic_write_reg(low_index, low_masked);
    ioapic_write_reg(high_index, high);

    uint32_t read_low = ioapic_read_reg(low_index);
    uint32_t read_high = ioapic_read_reg(high_index);
    if ((read_low & ~0x5000U) != (low_masked & ~0x5000U) ||
        read_high != high) {
        return ioapic_route_fail_remasked(irq, low_index, low_masked);
    }

    const uint32_t low_unmasked = low_masked & ~(1U << 16U);
    ioapic_write_reg(low_index, low_unmasked);
    read_low = ioapic_read_reg(low_index);
    read_high = ioapic_read_reg(high_index);
    if ((read_low & ~0x5000U) != (low_unmasked & ~0x5000U) ||
        read_high != high) {
        return ioapic_route_fail_remasked(irq, low_index, low_masked);
    }

    isa_irq_routed[irq] = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        if (irq == 0U) {
            serial_write("[IOAPIC] route: irq=0 vector=0x40 gsi=");
        } else if (irq == 1U) {
            serial_write("[IOAPIC] route: irq=1 vector=0x41 gsi=");
        } else {
            serial_write("[IOAPIC] route: irq=");
            serial_write_u64(irq);
            serial_write(" vector=");
            serial_write_u64(expected_vector);
            serial_write(" gsi=");
        }
        serial_write_u64(desc->gsi);
        serial_write(" dest=");
        serial_write_u64(lapic_bsp_apic_id());
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    return IOAPIC_OK;
}

enum ioapic_status ioapic_route_irq(
    uint8_t irq,
    uint8_t vector)
{
#ifdef HFOC_IRQ0_ROUTE_FAILURE_TEST
    if (irq == 0U) {
        serial_write("[IOAPIC] test: forcing IRQ0 route failure\n");
        return ioapic_route_fail(irq, IOAPIC_ROUTE_FAILED);
    }
#endif
    return ioapic_route_isa_irq(irq, vector);
}

bool ioapic_irq0_mask_save(uint64_t *out_previous_rte) {
    if (out_previous_rte == NULL ||
        !isa_irq_routed[0U] ||
        !isa_desc[0U].valid ||
        isa_desc[0U].gsi < ioapic_gsi_base ||
        isa_desc[0U].gsi - ioapic_gsi_base >= ioapic_redirection_entries) {
        return false;
    }

    const uint32_t index = isa_desc[0U].gsi - ioapic_gsi_base;
    const uint32_t low_index = 0x10U + 2U * index;
    const uint32_t high_index = 0x11U + 2U * index;
    const uint32_t low = ioapic_read_reg(low_index);
    const uint32_t high = ioapic_read_reg(high_index);
    const uint64_t previous = ((uint64_t)high << 32U) | low;
    *out_previous_rte = previous;

    if ((low & 0xFFU) != IOAPIC_IRQ0_VECTOR) {
        return false;
    }
    const uint32_t masked_low = low | (1U << 16U);
    ioapic_write_reg(low_index, masked_low);

    const uint32_t read_low = ioapic_read_reg(low_index);
    const uint32_t read_high = ioapic_read_reg(high_index);
    if ((read_low & ~0x5000U) != (masked_low & ~0x5000U) ||
        read_high != high) {
        (void)ioapic_irq0_restore(previous);
        return false;
    }
    return true;
}

bool ioapic_irq0_restore(uint64_t previous_rte) {
    if (!isa_irq_routed[0U] ||
        !isa_desc[0U].valid ||
        isa_desc[0U].gsi < ioapic_gsi_base ||
        isa_desc[0U].gsi - ioapic_gsi_base >= ioapic_redirection_entries) {
        return false;
    }

    const uint32_t index = isa_desc[0U].gsi - ioapic_gsi_base;
    const uint32_t low_index = 0x10U + 2U * index;
    const uint32_t high_index = 0x11U + 2U * index;
    const uint32_t saved_low = (uint32_t)previous_rte;
    const uint32_t saved_high = (uint32_t)(previous_rte >> 32U);

    ioapic_write_reg(high_index, saved_high);
    ioapic_write_reg(low_index, saved_low);

    const uint32_t read_low = ioapic_read_reg(low_index);
    const uint32_t read_high = ioapic_read_reg(high_index);
    if ((read_low & ~0x5000U) != (saved_low & ~0x5000U) ||
        read_high != saved_high) {
        return false;
    }
    return true;
}

bool ioapic_is_probed(void) {
    return probed;
}

bool ioapic_is_irq1_routed(void) {
    return ioapic_is_isa_irq_routed(1U);
}

bool ioapic_is_irq0_routed(void) {
    return ioapic_is_isa_irq_routed(0U);
}

bool ioapic_is_isa_irq_routed(uint8_t irq) {
    return irq < IOAPIC_ISA_IRQ_COUNT && isa_irq_routed[irq];
}

const char *ioapic_status_string(enum ioapic_status status) {
    switch (status) {
    case IOAPIC_OK:                  return "OK";
    case IOAPIC_NO_ACPI_SNAPSHOT:    return "no ACPI snapshot";
    case IOAPIC_NO_IOAPIC:           return "no IOAPIC";
    case IOAPIC_BAD_MMIO_ADDRESS:    return "bad IOAPIC MMIO address";
    case IOAPIC_HHDM_OVERFLOW:       return "HHDM overflow";
    case IOAPIC_MMIO_MAP_FAILED:     return "MMIO map failed";
    case IOAPIC_BAD_VERSION:         return "bad IOAPIC version";
    case IOAPIC_BAD_ISO:             return "bad ISO";
    case IOAPIC_NOT_INITIALIZED:     return "not initialized";
    case IOAPIC_LAPIC_NOT_ENABLED:   return "LAPIC not enabled";
    case IOAPIC_UNSUPPORTED_IRQ:     return "unsupported IRQ";
    case IOAPIC_ROUTE_FAILED:        return "route failed";
    default:                         return "unknown";
    }
}
