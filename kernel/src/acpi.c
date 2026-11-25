#include "acpi.h"
#include "serial.h"
#include <stddef.h>
#include <stdint.h>

__attribute__((unused))
static uint16_t acpi_read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}
/* ITEM-560: whether the validated root table declares a DMA Remapping table. */
static bool published_dmar_present;

bool acpi_dmar_present(void) {
    return published_dmar_present;
}

static uint32_t acpi_read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static uint64_t acpi_read_u64_le(const uint8_t *p) {
    return (uint64_t)p[0]       | ((uint64_t)p[1] << 8U) |
           ((uint64_t)p[2] << 16U) | ((uint64_t)p[3] << 24U) |
           ((uint64_t)p[4] << 32U) | ((uint64_t)p[5] << 40U) |
           ((uint64_t)p[6] << 48U) | ((uint64_t)p[7] << 56U);
}

static bool is_physical_range_mapped(
    uint64_t phys_base, uint64_t length,
    const struct limine_memmap_response *memory_map)
{
    if (length == 0U) return true;
    if (phys_base > UINT64_MAX - length) return false;
    const uint64_t phys_end = phys_base + length;
    uint64_t cursor = phys_base;
    while (cursor < phys_end) {
        bool found = false;
        for (uint64_t i = 0U; i < memory_map->entry_count; ++i) {
            const struct limine_memmap_entry *e = memory_map->entries[i];
            if (e == NULL) continue;
            if (e->type != LIMINE_MEMMAP_ACPI_RECLAIMABLE &&
                e->type != LIMINE_MEMMAP_ACPI_NVS &&
                e->type != LIMINE_MEMMAP_RESERVED_MAPPED)
                continue;
            if (e->base > UINT64_MAX - e->length) continue;
            const uint64_t entry_end = e->base + e->length;
            if (cursor >= e->base && cursor < entry_end) {
                if (entry_end >= phys_end) return true;
                cursor = entry_end;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static void *phys_range_to_virt(uint64_t phys, uint64_t length,
                                uint64_t hhdm_offset) {
    if (length == 0U) return NULL;
    if (phys > UINT64_MAX - length) return NULL;
    if (phys > UINT64_MAX - hhdm_offset) return NULL;
    const uint64_t virt_start = phys + hhdm_offset;
    if (virt_start > UINT64_MAX - length) return NULL;
    return (void *)(uintptr_t)virt_start;
}

#ifdef HFOC_ACPI_HOST_TEST
bool acpi_test_is_physical_range_mapped(
    uint64_t phys_base, uint64_t length,
    const struct limine_memmap_response *memory_map)
{
    return is_physical_range_mapped(phys_base, length, memory_map);
}
bool acpi_test_phys_range_to_virt(
    uint64_t phys, uint64_t length, uint64_t hhdm_offset)
{
    return phys_range_to_virt(phys, length, hhdm_offset) != NULL;
}

#endif

static uint8_t acpi_checksum(const void *data, size_t length) {
    uint8_t sum = 0U;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0U; i < length; ++i) sum += p[i];
    return sum;
}

static enum acpi_status validate_sdt(
    uint64_t phys_base,
    const char expected_sig[4],
    uint64_t hhdm_offset,
    const struct limine_memmap_response *memory_map,
    unsigned int divisor,
    const void **out_virtual,
    uint32_t *out_length)
{
    if (!is_physical_range_mapped(phys_base, 36U, memory_map))
        return ACPI_RANGE_NOT_MAPPED;
    void *virt = phys_range_to_virt(phys_base, 36U, hhdm_offset);
    if (virt == NULL) return ACPI_HHDM_OVERFLOW;
    const uint8_t *hdr = (const uint8_t *)virt;
    if (hdr[0] != (uint8_t)expected_sig[0] ||
        hdr[1] != (uint8_t)expected_sig[1] ||
        hdr[2] != (uint8_t)expected_sig[2] ||
        hdr[3] != (uint8_t)expected_sig[3])
        return ACPI_BAD_ROOT_SIGNATURE;
    uint32_t length = acpi_read_u32_le(hdr + 4);
    if (length < 36U) return ACPI_BAD_SDT_LENGTH;
    if (divisor != 0U && (length - 36U) % divisor != 0U)
        return ACPI_BAD_SDT_LENGTH;
    if (phys_base > UINT64_MAX - length)
        return ACPI_RANGE_NOT_MAPPED;
    if (!is_physical_range_mapped(phys_base, length, memory_map))
        return ACPI_RANGE_NOT_MAPPED;
    virt = phys_range_to_virt(phys_base, length, hhdm_offset);
    if (virt == NULL) return ACPI_HHDM_OVERFLOW;
    if (acpi_checksum(virt, length) != 0U)
        return ACPI_SDT_CHECKSUM;
    *out_virtual = virt;
    *out_length = length;
    return ACPI_OK;
}

static enum acpi_status find_madt(
    const uint8_t *payload, uint32_t entry_count,
    unsigned int entry_width,
    uint64_t hhdm_offset,
    const struct limine_memmap_response *memory_map,
    const void **out_virtual, uint32_t *out_length)
{
    bool found = false;
    for (uint32_t i = 0U; i < entry_count; ++i) {
        const uint8_t *entry = payload + i * entry_width;
        uint64_t phys;
        if (entry_width == 4U)
            phys = acpi_read_u32_le(entry);
        else
            phys = acpi_read_u64_le(entry);
        if (phys == 0U) continue;
        if (!is_physical_range_mapped(phys, 36U, memory_map))
            continue;
        void *hdr_virt = phys_range_to_virt(phys, 36U, hhdm_offset);
        if (hdr_virt == NULL) continue;
        const uint8_t *sig = (const uint8_t *)hdr_virt;
        if (sig[0] != 'A' || sig[1] != 'P' ||
            sig[2] != 'I' || sig[3] != 'C')
            continue;
        const void *virt = NULL;
        uint32_t len = 0U;
        enum acpi_status st = validate_sdt(
            phys, "APIC", hhdm_offset, memory_map, 0U, &virt, &len);
        if (st != ACPI_OK) return st;
        if (found) return ACPI_DUPLICATE_MADT;
        found = true;
        *out_virtual = virt;
        *out_length = len;
    }
    return found ? ACPI_OK : ACPI_NO_MADT;
}

static enum acpi_status parse_madt(
    const void *madt_virtual,
    uint32_t madt_length,
    struct acpi_madt_info *out)
{
    if (madt_length < 44U) return ACPI_BAD_SDT_LENGTH;

    const uint8_t *ptr = (const uint8_t *)madt_virtual;
    const uint8_t *end = ptr + madt_length;

    out->lapic_address = acpi_read_u32_le(ptr + 36U);
    out->has_override = false;
    out->has_8259 = (acpi_read_u32_le(ptr + 40U) & 1U) != 0U;

    ptr += 44U;
    while (ptr + 2U <= end) {
        const uint8_t entry_type = ptr[0];
        const uint8_t entry_len  = ptr[1];
        if (entry_len < 2U) return ACPI_MALFORMED_ENTRY;
        if (ptr + entry_len > end) return ACPI_MALFORMED_ENTRY;

        switch (entry_type) {
        case 0: /* Processor Local APIC */
            if (entry_len < 8U) return ACPI_MALFORMED_ENTRY;
            if (out->lapic_count >= ACPI_MAX_LAPICS)
                return ACPI_CAPACITY_OVERFLOW;
            out->lapics[out->lapic_count].acpi_processor_id = ptr[2];
            out->lapics[out->lapic_count].apic_id = ptr[3];
            out->lapics[out->lapic_count].flags =
                acpi_read_u32_le(ptr + 4U);
            ++out->lapic_count;
            break;
        case 1: /* IOAPIC */
            if (entry_len < 12U) return ACPI_MALFORMED_ENTRY;
            if (out->ioapic_count >= ACPI_MAX_IOAPICS)
                return ACPI_CAPACITY_OVERFLOW;
            out->ioapics[out->ioapic_count].ioapic_id = ptr[2];
            out->ioapics[out->ioapic_count].reserved = ptr[3];
            out->ioapics[out->ioapic_count].mmio_address =
                acpi_read_u32_le(ptr + 4U);
            out->ioapics[out->ioapic_count].gsi_base =
                acpi_read_u32_le(ptr + 8U);
            ++out->ioapic_count;
            break;
        case 2: /* Interrupt Source Override */
            if (entry_len < 10U) return ACPI_MALFORMED_ENTRY;
            if (out->iso_count >= ACPI_MAX_ISOS)
                return ACPI_CAPACITY_OVERFLOW;
            out->isos[out->iso_count].bus    = ptr[2];
            out->isos[out->iso_count].source = ptr[3];
            out->isos[out->iso_count].gsi =
                acpi_read_u32_le(ptr + 4U);
            out->isos[out->iso_count].flags =
                acpi_read_u16_le(ptr + 8U);
            ++out->iso_count;
            break;
        case 4: /* Local APIC NMI */
            if (entry_len < 6U) return ACPI_MALFORMED_ENTRY;
            if (out->lapic_nmi_count >= ACPI_MAX_LAPIC_NMIS)
                return ACPI_CAPACITY_OVERFLOW;
            out->lapic_nmis[out->lapic_nmi_count]
                .acpi_processor_id = ptr[2];
            out->lapic_nmis[out->lapic_nmi_count].flags =
                acpi_read_u16_le(ptr + 3U);
            out->lapic_nmis[out->lapic_nmi_count].lint = ptr[5];
            ++out->lapic_nmi_count;
            break;
        case 5: /* Local APIC Address Override */
            if (entry_len < 12U) return ACPI_MALFORMED_ENTRY;
            if (out->has_override)
                return ACPI_DUPLICATE_LAPIC_OVERRIDE;
            out->lapic_address = acpi_read_u64_le(ptr + 4U);
            out->has_override = true;
            break;
        case 9: /* Local x2APIC */
            if (entry_len < 16U) return ACPI_MALFORMED_ENTRY;
            if (out->x2apic_count >= ACPI_MAX_X2APICS)
                return ACPI_CAPACITY_OVERFLOW;
            out->x2apics[out->x2apic_count].acpi_processor_uid =
                acpi_read_u32_le(ptr + 12U);
            out->x2apics[out->x2apic_count].x2apic_id =
                acpi_read_u32_le(ptr + 4U);
            out->x2apics[out->x2apic_count].flags =
                acpi_read_u32_le(ptr + 8U);
            ++out->x2apic_count;
            break;
        case 10: /* Local x2APIC NMI */
            if (entry_len < 12U) return ACPI_MALFORMED_ENTRY;
            if (out->x2apic_nmi_count >= ACPI_MAX_X2APIC_NMIS)
                return ACPI_CAPACITY_OVERFLOW;
            out->x2apic_nmis[out->x2apic_nmi_count].flags =
                acpi_read_u16_le(ptr + 2U);
            out->x2apic_nmis[out->x2apic_nmi_count].acpi_processor_uid =
                acpi_read_u32_le(ptr + 4U);
            out->x2apic_nmis[out->x2apic_nmi_count].lint = ptr[8];
            ++out->x2apic_nmi_count;
            break;
        default:
            break;
        }
        ptr += entry_len;
    }
    if (ptr != end) return ACPI_MALFORMED_ENTRY;
    return ACPI_OK;
}

#ifdef HFOC_ACPI_HOST_TEST
enum acpi_status acpi_test_parse_madt(
    const void *madt_virtual, uint32_t madt_length,
    struct acpi_madt_info *out)
{
    return parse_madt(madt_virtual, madt_length, out);
}
#endif

/*
 * ITEM-411: the FADT reset register.
 *
 * The FADT is searched for independently of the MADT, because a machine can
 * describe how to reset itself while carrying no MADT this kernel accepts, and
 * a missing MADT must not take the reset register down with it.  What is
 * reported is the platform's declaration and nothing more; a FADT that is too
 * short to carry the flags word declares nothing at all, and one that is too
 * short to carry the register describes no register.
 */
static struct acpi_fadt_reset published_fadt_reset;

static void fadt_reset_clear(void) {
    for (size_t i = 0U; i < sizeof(published_fadt_reset); ++i)
        ((volatile uint8_t *)&published_fadt_reset)[i] = 0U;
}

static void parse_fadt_reset(
    const void *fadt_virtual, uint32_t fadt_length,
    struct acpi_fadt_reset *out)
{
    for (size_t i = 0U; i < sizeof(*out); ++i)
        ((volatile uint8_t *)out)[i] = 0U;
    if (fadt_length < ACPI_FADT_RESET_FLAGS_MIN_LENGTH)
        return;
    const uint8_t *p = (const uint8_t *)fadt_virtual;
    out->present = true;
    if ((acpi_read_u32_le(p + ACPI_FADT_RESET_FLAGS_OFFSET) &
         ACPI_FADT_FLAG_RESET_REG_SUP) == 0U)
        return;
    if (fadt_length < ACPI_FADT_RESET_MIN_LENGTH)
        return;
    out->address_space = p[ACPI_FADT_RESET_REG_OFFSET];
    out->bit_width = p[ACPI_FADT_RESET_REG_OFFSET + 1U];
    out->bit_offset = p[ACPI_FADT_RESET_REG_OFFSET + 2U];
    out->access_size = p[ACPI_FADT_RESET_REG_OFFSET + 3U];
    out->address = acpi_read_u64_le(p + ACPI_FADT_RESET_REG_OFFSET + 4U);
    out->value = p[ACPI_FADT_RESET_VALUE_OFFSET];
    out->supported = true;
}

#ifdef HFOC_ACPI_HOST_TEST
void acpi_test_parse_fadt_reset(
    const void *fadt_virtual, uint32_t fadt_length,
    struct acpi_fadt_reset *out)
{
    if (out == NULL) return;
    parse_fadt_reset(fadt_virtual, fadt_length, out);
}
#endif

/*
 * ITEM-418: the PM1 control blocks and the DSDT pointer.
 *
 * The parse here reads only what the FADT itself declares; the DSDT is
 * validated and mapped by the caller, because that needs the memory map.  A
 * FADT shorter than the PM1 fields describes no control block, and one that
 * declares a block at address zero declares none either - ACPI uses zero for
 * "this platform has no such block", and writing to port zero would be a
 * different machine's business.
 */
static struct acpi_fadt_power published_fadt_power;

static void fadt_power_clear(void) {
    for (size_t i = 0U; i < sizeof(published_fadt_power); ++i)
        ((volatile uint8_t *)&published_fadt_power)[i] = 0U;
}

static void parse_fadt_power(
    const void *fadt_virtual, uint32_t fadt_length,
    struct acpi_fadt_power *out)
{
    for (size_t i = 0U; i < sizeof(*out); ++i)
        ((volatile uint8_t *)out)[i] = 0U;
    if (fadt_virtual == NULL || fadt_length < 36U)
        return;
    const uint8_t *p = (const uint8_t *)fadt_virtual;
    out->present = true;
    out->pm1a_control = (uint16_t)acpi_read_u32_le(
        p + ACPI_FADT_PM1A_CNT_OFFSET);
    if (fadt_length >= ACPI_FADT_PM1_MIN_LENGTH) {
        out->pm1b_control = (uint16_t)acpi_read_u32_le(
            p + ACPI_FADT_PM1B_CNT_OFFSET);
    }
    out->pm1a_declared = out->pm1a_control != 0U;
    out->pm1b_declared = out->pm1b_control != 0U;
}

static uint64_t fadt_power_dsdt_physical(
    const void *fadt_virtual, uint32_t fadt_length)
{
    const uint8_t *p = (const uint8_t *)fadt_virtual;

    if (fadt_length >= ACPI_FADT_X_DSDT_MIN_LENGTH) {
        const uint64_t extended =
            acpi_read_u64_le(p + ACPI_FADT_X_DSDT_OFFSET);
        if (extended != 0U)
            return extended;
    }
    return (uint64_t)acpi_read_u32_le(p + ACPI_FADT_DSDT_OFFSET);
}

#ifdef HFOC_ACPI_HOST_TEST
void acpi_test_parse_fadt_power(
    const void *fadt_virtual, uint32_t fadt_length,
    struct acpi_fadt_power *out)
{
    if (out == NULL) return;
    parse_fadt_power(fadt_virtual, fadt_length, out);
}
#endif

/*
 * The first FADT in the root table whose signature, length and checksum hold.
 * A duplicate is not accepted: a firmware that describes two different power
 * management tables has not declared one reset register, so the machine is
 * reset by a fallback mechanism and the record names it.
 */
static bool find_fadt(
    const uint8_t *payload, uint32_t entry_count,
    unsigned int entry_width,
    uint64_t hhdm_offset,
    const struct limine_memmap_response *memory_map,
    const void **out_virtual, uint32_t *out_length)
{
    bool found = false;
    const void *found_virtual = NULL;
    uint32_t found_length = 0U;

    for (uint32_t i = 0U; i < entry_count; ++i) {
        const uint8_t *entry = payload + i * entry_width;
        uint64_t phys;
        if (entry_width == 4U)
            phys = acpi_read_u32_le(entry);
        else
            phys = acpi_read_u64_le(entry);
        if (phys == 0U) continue;
        if (!is_physical_range_mapped(phys, 36U, memory_map))
            continue;
        void *hdr_virt = phys_range_to_virt(phys, 36U, hhdm_offset);
        if (hdr_virt == NULL) continue;
        const uint8_t *sig = (const uint8_t *)hdr_virt;
        if (sig[0] != 'F' || sig[1] != 'A' ||
            sig[2] != 'C' || sig[3] != 'P')
            continue;
        const void *virt = NULL;
        uint32_t len = 0U;
        if (validate_sdt(phys, "FACP", hhdm_offset, memory_map, 0U,
                         &virt, &len) != ACPI_OK) {
            /* A FACP that does not validate is not a declaration. */
            continue;
        }
        if (found) {
            *out_virtual = NULL;
            *out_length = 0U;
            return false;
        }
        found = true;
        found_virtual = virt;
        found_length = len;
    }
    if (!found) return false;
    *out_virtual = found_virtual;
    *out_length = found_length;
    return true;
}

enum acpi_status acpi_parse(
    const void *rsdp_virtual,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset,
    struct acpi_madt_info *out)
{
    if (out == NULL) return ACPI_NO_RSDP_RESPONSE;
    for (size_t i = 0U; i < sizeof(*out); ++i)
        ((volatile uint8_t *)out)[i] = 0U;
    /* The reset register belongs to this parse, not to the previous one. */
    fadt_reset_clear();
    /* ITEM-418: the power declaration belongs to this parse too. */
    fadt_power_clear();
    if (rsdp_virtual == NULL || memory_map == NULL)
        return ACPI_NO_RSDP_RESPONSE;
    if ((uintptr_t)rsdp_virtual < hhdm_offset)
        return ACPI_HHDM_OVERFLOW;
    const uint64_t rsdp_phys =
        (uint64_t)((uintptr_t)rsdp_virtual - hhdm_offset);
    if (!is_physical_range_mapped(rsdp_phys, 20U, memory_map))
        return ACPI_RANGE_NOT_MAPPED;
    {
        const uint8_t *sig = (const uint8_t *)rsdp_virtual;
        if (sig[0] != 'R' || sig[1] != 'S' || sig[2] != 'D' ||
            sig[3] != ' ' || sig[4] != 'P' || sig[5] != 'T' ||
            sig[6] != 'R' || sig[7] != ' ')
            return ACPI_BAD_RSDP_SIGNATURE;
    }
    if (acpi_checksum(rsdp_virtual, 20U) != 0U)
        return ACPI_RSDP_LEGACY_CHECKSUM;
    const uint8_t revision = ((const uint8_t *)rsdp_virtual)[15];
    if (revision == 1U)
        return ACPI_UNSUPPORTED_RSDP_REVISION;
    const uint8_t *rsdp = (const uint8_t *)rsdp_virtual;
    uint64_t xsdt_phys = 0U, rsdt_phys = 0U;
    if (revision == 0U) {
        rsdt_phys = acpi_read_u32_le(rsdp + 16);
    } else {
        if (!is_physical_range_mapped(rsdp_phys, 36U, memory_map))
            return ACPI_RANGE_NOT_MAPPED;
        const uint32_t ext_length = acpi_read_u32_le(rsdp + 20);
        if (ext_length < 36U) return ACPI_BAD_RSDP_LENGTH;
        if (rsdp_phys > UINT64_MAX - ext_length)
            return ACPI_RANGE_NOT_MAPPED;
        if (!is_physical_range_mapped(rsdp_phys, ext_length, memory_map))
            return ACPI_RANGE_NOT_MAPPED;
        if (acpi_checksum(rsdp_virtual, ext_length) != 0U)
            return ACPI_RSDP_EXT_CHECKSUM;
        xsdt_phys = acpi_read_u64_le(rsdp + 24);
        rsdt_phys = acpi_read_u32_le(rsdp + 16);
    }
    const void *root_virt = NULL;
    uint32_t root_len = 0U;
    enum acpi_status root_status = ACPI_NO_VALID_ROOT_TABLE;
    unsigned int divisor = 0U;
    int root_type = 0;
    if (xsdt_phys != 0U) {
        root_status = validate_sdt(xsdt_phys, "XSDT", hhdm_offset,
                                   memory_map, 8U, &root_virt, &root_len);
        if (root_status == ACPI_OK) { divisor = 8U; root_type = 1; }
    }
    if (root_type == 0 && rsdt_phys != 0U) {
        root_status = validate_sdt(rsdt_phys, "RSDT", hhdm_offset,
                                   memory_map, 4U, &root_virt, &root_len);
        if (root_status == ACPI_OK) { divisor = 4U; root_type = 2; }
    }
    /* Если ни XSDT, ни RSDT не прошли полную проверку — всегда
     * возвращаем ACPI_NO_VALID_ROOT_TABLE, не утекая детали ошибки
     * конкретной таблицы (checksum, signature, length, range). */
    if (root_type == 0) return ACPI_NO_VALID_ROOT_TABLE;
    const uint32_t entry_count = (root_len - 36U) / divisor;
    const uint8_t *payload = (const uint8_t *)root_virt + 36U;
    /*
     * ITEM-411: the reset register is read from the same validated root table,
     * before the MADT is looked for, so a machine whose MADT this kernel will
     * not accept still states how it wants to be reset.
     */
    {
        const void *fadt_virt = NULL;
        uint32_t fadt_len = 0U;
        if (find_fadt(payload, entry_count, (unsigned int)divisor,
                      hhdm_offset, memory_map, &fadt_virt, &fadt_len)) {
            parse_fadt_reset(fadt_virt, fadt_len, &published_fadt_reset);
            /*
             * ITEM-418: and how it wants to be powered off.  The DSDT that
             * carries the sleep type is validated and mapped here, because
             * this is where the memory map is; a DSDT that does not validate
             * leaves `dsdt` NULL and the power module says so instead of
             * writing a sleep type nobody declared.
             */
            parse_fadt_power(fadt_virt, fadt_len, &published_fadt_power);
            if (published_fadt_power.present) {
                const uint64_t dsdt_phys =
                    fadt_power_dsdt_physical(fadt_virt, fadt_len);
                const void *dsdt_virt = NULL;
                uint32_t dsdt_len = 0U;

                if (dsdt_phys != 0U &&
                    validate_sdt(dsdt_phys, "DSDT", hhdm_offset,
                                 memory_map, 0U, &dsdt_virt,
                                 &dsdt_len) == ACPI_OK) {
                    published_fadt_power.dsdt = dsdt_virt;
                    published_fadt_power.dsdt_length = dsdt_len;
                }
            }
        }
    }
    /*
     * ITEM-560: is there a DMA Remapping (VT-d) table on this machine?  The
     * EHCI's Host System Error can be a DMA fault the IOMMU raised because
     * this kernel never programmed a translation domain, and a machine that
     * does not describe DMAR cannot have that cause.  The scan only records
     * the declaration: nothing is disabled or reprogrammed here.
     */
    published_dmar_present = false;
    for (uint32_t index = 0U; index < entry_count; ++index) {
        const uint64_t table_phys =
            divisor == 8U ? acpi_read_u64_le(payload + (size_t)index * 8U)
                          : (uint64_t)acpi_read_u32_le(
                                payload + (size_t)index * 4U);
        const uint8_t *table;
        char signature[4];

        if (table_phys == 0U ||
            table_phys + 4U > UINT64_MAX ||
            !is_physical_range_mapped(table_phys, 4U, memory_map)) {
            continue;
        }
        table = (const uint8_t *)(uintptr_t)(table_phys + hhdm_offset);
        for (uint32_t at = 0U; at < 4U; ++at) {
            signature[at] = (char)table[at];
        }
        if (signature[0] == 'D' && signature[1] == 'M' &&
            signature[2] == 'A' && signature[3] == 'R') {
            published_dmar_present = true;
            break;
        }
    }
    const void *madt_virt = NULL;
    uint32_t madt_len = 0U;
    const enum acpi_status madt_status = find_madt(
        payload, entry_count, (unsigned int)divisor,
        hhdm_offset, memory_map, &madt_virt, &madt_len);
    if (madt_status != ACPI_OK) {
        for (size_t i = 0U; i < sizeof(*out); ++i)
            ((volatile uint8_t *)out)[i] = 0U;
        return madt_status;
    }

    enum acpi_status parse_st =
        parse_madt(madt_virt, madt_len, out);
    if (parse_st != ACPI_OK) {
        for (size_t i = 0U; i < sizeof(*out); ++i)
            ((volatile uint8_t *)out)[i] = 0U;
        return parse_st;
    }

    out->root_source = (root_type == 1) ? ACPI_ROOT_XSDT
                                        : ACPI_ROOT_RSDT;
    return ACPI_OK;
}

const char *acpi_status_string(enum acpi_status status) {
    switch (status) {
    case ACPI_OK: return "OK";
    case ACPI_NO_RSDP_RESPONSE: return "no RSDP response";
    case ACPI_BAD_RSDP_SIGNATURE: return "bad RSDP signature";
    case ACPI_RSDP_LEGACY_CHECKSUM: return "RSDP legacy checksum failed";
    case ACPI_RSDP_EXT_CHECKSUM: return "RSDP extended checksum failed";
    case ACPI_UNSUPPORTED_RSDP_REVISION:
        return "unsupported RSDP revision";
    case ACPI_BAD_RSDP_LENGTH: return "bad RSDP length";
    case ACPI_BAD_SDT_LENGTH: return "bad SDT length";
    case ACPI_BAD_ROOT_SIGNATURE: return "bad root table signature";
    case ACPI_NO_VALID_ROOT_TABLE: return "no valid root table";
    case ACPI_HHDM_OVERFLOW: return "HHDM overflow";
    case ACPI_RANGE_NOT_MAPPED: return "ACPI range not mapped";
    case ACPI_SDT_CHECKSUM: return "SDT checksum failed";
    case ACPI_NO_MADT: return "no MADT found";
    case ACPI_DUPLICATE_MADT: return "duplicate MADT";
    case ACPI_MALFORMED_ENTRY: return "malformed MADT entry";
    case ACPI_CAPACITY_OVERFLOW: return "capacity overflow";
    case ACPI_DUPLICATE_LAPIC_OVERRIDE:
        return "duplicate LAPIC address override";
    }
    return "unknown";
}

static struct acpi_madt_info published_snapshot;
static bool snapshot_ready;

enum acpi_status acpi_init(
    const struct limine_rsdp_response *rsdp_response,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset)
{
    snapshot_ready = false;
    for (size_t i = 0U; i < sizeof(published_snapshot); ++i)
        ((volatile uint8_t *)&published_snapshot)[i] = 0U;

    if (rsdp_response == NULL || rsdp_response->address == NULL) {
        serial_write("[ACPI] unavailable: no RSDP response"
                     "; legacy PIC retained\n");
        return ACPI_NO_RSDP_RESPONSE;
    }

    const enum acpi_status status = acpi_parse(
        rsdp_response->address, memory_map, hhdm_offset,
        &published_snapshot);

    if (status != ACPI_OK) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[ACPI] unavailable: ");
            serial_write(acpi_status_string(status));
            serial_write("; legacy PIC retained\n");
            serial_sequence_end(serial_flags);
        }
        return status;
    }

    snapshot_ready = true;

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[ACPI] Root: ");
        serial_write(published_snapshot.root_source == ACPI_ROOT_XSDT
                     ? "XSDT" : "RSDT");
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[ACPI] MADT: lapic=");
        serial_write_u64(published_snapshot.lapic_count);
        serial_write(" ioapic=");
        serial_write_u64(published_snapshot.ioapic_count);
        serial_write(" iso=");
        serial_write_u64(published_snapshot.iso_count);
        serial_write(" lapic-nmi=");
        serial_write_u64(published_snapshot.lapic_nmi_count);
        serial_write(" address=");
        serial_write_hex(published_snapshot.lapic_address);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    serial_write("[ACPI] Parsed successfully; legacy PIC retained\n");
    return ACPI_OK;
}

const struct acpi_madt_info *acpi_madt(void) {
    return snapshot_ready ? &published_snapshot : NULL;
}

const struct acpi_fadt_reset *acpi_fadt_reset(void) {
    return published_fadt_reset.present ? &published_fadt_reset : NULL;
}

const struct acpi_fadt_power *acpi_fadt_power(void) {
    return published_fadt_power.present ? &published_fadt_power : NULL;
}
