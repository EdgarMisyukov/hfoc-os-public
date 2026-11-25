#ifndef HFOC_ACPI_H
#define HFOC_ACPI_H

#include "limine_min.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct acpi_lapic {
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct acpi_ioapic {
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t mmio_address;
    uint32_t gsi_base;
};

struct acpi_iso {
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_lapic_nmi {
    uint8_t acpi_processor_id;
    uint16_t flags;
    uint8_t lint;
};

struct acpi_x2apic {
    uint32_t acpi_processor_uid;
    uint32_t x2apic_id;
    uint32_t flags;
};

struct acpi_x2apic_nmi {
    uint32_t acpi_processor_uid;
    uint16_t flags;
    uint8_t lint;
};

enum acpi_status {
    ACPI_OK = 0,
    ACPI_NO_RSDP_RESPONSE = 1,
    ACPI_BAD_RSDP_SIGNATURE = 2,
    ACPI_RSDP_LEGACY_CHECKSUM = 3,
    ACPI_RSDP_EXT_CHECKSUM = 4,
    ACPI_UNSUPPORTED_RSDP_REVISION = 5,
    ACPI_BAD_RSDP_LENGTH = 6,
    ACPI_BAD_SDT_LENGTH = 7,
    ACPI_BAD_ROOT_SIGNATURE = 8,
    ACPI_NO_VALID_ROOT_TABLE = 9,
    ACPI_HHDM_OVERFLOW = 10,
    ACPI_RANGE_NOT_MAPPED = 11,
    ACPI_SDT_CHECKSUM = 12,
    ACPI_NO_MADT = 13,
    ACPI_DUPLICATE_MADT = 14,
    ACPI_MALFORMED_ENTRY = 15,
    ACPI_CAPACITY_OVERFLOW = 16,
    ACPI_DUPLICATE_LAPIC_OVERRIDE = 17,
};

#define ACPI_MAX_LAPICS    256
#define ACPI_MAX_IOAPICS    16
#define ACPI_MAX_ISOS       32
#define ACPI_MAX_LAPIC_NMIS 32
#define ACPI_MAX_X2APICS    256
#define ACPI_MAX_X2APIC_NMIS 32

struct acpi_madt_info {
    size_t lapic_count;
    size_t ioapic_count;
    size_t iso_count;
    size_t lapic_nmi_count;
    size_t x2apic_count;
    size_t x2apic_nmi_count;

    struct acpi_lapic      lapics[ACPI_MAX_LAPICS];
    struct acpi_ioapic     ioapics[ACPI_MAX_IOAPICS];
    struct acpi_iso        isos[ACPI_MAX_ISOS];
    struct acpi_lapic_nmi  lapic_nmis[ACPI_MAX_LAPIC_NMIS];
    struct acpi_x2apic     x2apics[ACPI_MAX_X2APICS];
    struct acpi_x2apic_nmi x2apic_nmis[ACPI_MAX_X2APIC_NMIS];

    uint64_t lapic_address;
    bool     has_override;
    bool     has_8259;

    enum { ACPI_ROOT_XSDT, ACPI_ROOT_RSDT } root_source;
};

enum acpi_status acpi_parse(
    const void *rsdp_virtual,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset,
    struct acpi_madt_info *out);

enum acpi_status acpi_init(
    const struct limine_rsdp_response *rsdp_response,
    const struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset);

const struct acpi_madt_info *acpi_madt(void);

/*
 * ITEM-560: whether the validated root table carries a DMAR (DMA Remapping)
 * table.  A host that describes one may run its EHCI behind an IOMMU, and a
 * DMA the kernel never translated then fails exactly as a Host System Error;
 * the record states the declaration and nothing here disables the unit.
 */
bool acpi_dmar_present(void);

/*
 * ITEM-411: the FADT's reset register.
 *
 * The reset register is the platform's own statement of how to reboot the
 * machine, and it is the only mechanism the firmware describes rather than the
 * kernel guessing: FADT `RESET_REG` (a 12-byte generic address structure at
 * offset 116), `RESET_VALUE` (offset 128) and the `RESET_REG_SUP` bit of the
 * FADT flags word (offset 112, bit 10).  Nothing else in ACPI says how to
 * reset, so this is what `kernel/src/platform_reset.c` decides from.
 *
 * The snapshot below reports the *declaration*, never a verdict: whether the
 * declared register is one this kernel can write is the reset module's
 * decision, and whether writing it actually resets the machine is the
 * platform's.  `present` is false when the root table carries no FADT this
 * kernel could validate (signature, length, checksum), and `supported` is
 * false when the FADT does not declare a reset register or is too short to
 * describe one - the machine is then reset by a fallback mechanism, and the
 * record says so.
 */
#define ACPI_FADT_RESET_FLAGS_OFFSET 112U
#define ACPI_FADT_RESET_REG_OFFSET 116U
#define ACPI_FADT_RESET_VALUE_OFFSET 128U
/* The flags word is present from this length; the reset register from 129. */
#define ACPI_FADT_RESET_FLAGS_MIN_LENGTH 116U
#define ACPI_FADT_RESET_MIN_LENGTH 129U
/* FADT flags, bit 10: RESET_REG_SUP. */
#define ACPI_FADT_FLAG_RESET_REG_SUP UINT32_C(0x00000400)

enum acpi_address_space {
    ACPI_ADDRESS_SPACE_SYSTEM_MEMORY = 0,
    ACPI_ADDRESS_SPACE_SYSTEM_IO = 1,
    ACPI_ADDRESS_SPACE_PCI_CONFIGURATION = 2,
    ACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER = 3,
    ACPI_ADDRESS_SPACE_SMBUS = 4,
    ACPI_ADDRESS_SPACE_FIXED_HARDWARE = 0x7FU
};

struct acpi_fadt_reset {
    /* A FADT with a valid signature, length and checksum was found. */
    bool present;
    /* And it declares a reset register this kernel can describe. */
    bool supported;
    uint8_t address_space; /* enum acpi_address_space */
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
    uint8_t value;
};

/* NULL when the ACPI root table carries no FADT this kernel could validate. */
const struct acpi_fadt_reset *acpi_fadt_reset(void);

/*
 * ITEM-418: what the FADT says about powering the machine off.
 *
 * A reset register is a convenience; a machine that is asked to *power off*
 * has only one place to say how: the fixed ACPI description table names the
 * PM1 control blocks (`PM1a_CNT_BLK` at offset 64, `PM1b_CNT_BLK` at offset
 * 68), and the `\_S5` object of the DSDT carries the sleep type the platform
 * wants written there.  Both are read here, once, from the same validated
 * root table the MADT and the reset register come from; the DSDT itself is
 * validated and mapped too, because the sleep type lives inside it.
 *
 * `pm1a_declared`/`pm1b_declared` report what the firmware wrote down, never a
 * verdict: whether the platform honours the write is the platform's answer and
 * the power module publishes it.  `dsdt` is NULL when the FADT declares no
 * DSDT or the table does not validate - the platform is then asked for a
 * shutdown this kernel cannot describe, which the record says.
 */
#define ACPI_FADT_DSDT_OFFSET 36U
#define ACPI_FADT_X_DSDT_OFFSET 140U
#define ACPI_FADT_PM1A_CNT_OFFSET 64U
#define ACPI_FADT_PM1B_CNT_OFFSET 68U
/* The X_DSDT field needs this length; the PM1 control blocks need 72. */
#define ACPI_FADT_X_DSDT_MIN_LENGTH 148U
#define ACPI_FADT_PM1_MIN_LENGTH 72U

struct acpi_fadt_power {
    /* A FADT with a valid signature, length and checksum was found. */
    bool present;
    /* And it declares a PM1a control block, which a shutdown needs. */
    bool pm1a_declared;
    bool pm1b_declared;
    uint16_t pm1a_control;
    uint16_t pm1b_control;
    /* The validated, mapped DSDT, or NULL when there is none to read. */
    const void *dsdt;
    uint32_t dsdt_length;
};

/* NULL when the ACPI root table carries no FADT this kernel could validate. */
const struct acpi_fadt_power *acpi_fadt_power(void);

const char *acpi_status_string(enum acpi_status status);

#endif
