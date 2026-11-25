#include "ioapic.h"

#include "acpi.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test-only accessors defined in kernel/src/ioapic.c under
 * HFOC_IOAPIC_HOST_TEST; they are not part of the kernel ELF. */
bool ioapic_test_irq_active_low(uint8_t irq);
bool ioapic_test_irq_level_triggered(uint8_t irq);
uint32_t ioapic_test_irq_gsi(uint8_t irq);
bool ioapic_test_isa_irq_desc_valid(uint8_t irq);
uint32_t ioapic_test_isa_irq_gsi(uint8_t irq);
uint8_t ioapic_test_ioapic_id(void);
uintptr_t ioapic_test_mmio_base(void);
uint32_t ioapic_test_gsi_base(void);
uint8_t ioapic_test_version(void);
uint16_t ioapic_test_redirection_entries(void);
void ioapic_test_invalidate_irq1_desc(void);
void ioapic_test_invalidate_irq0_desc(void);
enum ioapic_status ioapic_route_isa_irq(uint8_t irq, uint8_t vector);
bool ioapic_is_isa_irq_routed(uint8_t irq);

static uint32_t mock_regs[64];
static uint32_t selected_index;
static bool vmm_map_fails;
static volatile void *mock_map_out;
static uint32_t ioregsel_write_count;
static uint32_t iowin_write_count;
static uint32_t iowin_read_count;
static int failures;
static int test_count;
static bool corrupt_first_low_write;
static bool corrupt_second_low_write;
static uint32_t low_write_count;
static uint32_t low_ignore_bits;
static uint32_t high_corrupt_bits;
static bool corrupt_irq0_low_write;
static bool corrupt_irq0_low_write_once;
static uint32_t corrupt_isa_irq3_stage;

static uint32_t route_low_index(uint8_t irq) {
    return 0x10U + 2U * (uint32_t)irq;
}

static uint32_t route_high_index(uint8_t irq) {
    return 0x11U + 2U * (uint32_t)irq;
}

static bool is_isa_rte_index(uint32_t index) {
    return index >= 0x10U && index <= 0x2FU;
}

static bool host_lapic_enabled = true;
static uint8_t host_lapic_id;

bool host_lapic_is_enabled(void) {
    return host_lapic_enabled;
}

uint8_t host_lapic_bsp_apic_id(void) {
    return host_lapic_id;
}

static void reset_mock_state(void) {
    memset(mock_regs, 0, sizeof(mock_regs));
    mock_regs[0x01U] = 0x00170011U;
    selected_index = 0U;
    vmm_map_fails = false;
    mock_map_out = (volatile void *)0xFEC00000U;
    ioregsel_write_count = 0U;
    iowin_write_count = 0U;
    iowin_read_count = 0U;
    host_lapic_enabled = true;
    host_lapic_id = 0U;
    corrupt_first_low_write = false;
    corrupt_second_low_write = false;
    low_write_count = 0U;
    low_ignore_bits = 0U;
    high_corrupt_bits = 0U;
    corrupt_irq0_low_write = false;
    corrupt_irq0_low_write_once = false;
    corrupt_isa_irq3_stage = 0U;
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void serial_write(const char *text) {
    (void)text;
}

void serial_write_u64(uint64_t value) {
    (void)value;
}

void serial_write_hex(uint64_t value) {
    (void)value;
}

enum vmm_status ioapic_test_vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual)
{
    (void)physical_address;
    (void)hhdm_offset;
    if (vmm_map_fails) {
        return VMM_PMM_ALLOC_FAILED;
    }
    if (out_virtual != NULL) {
        *out_virtual = mock_map_out;
    }
    return VMM_OK;
}

uint32_t host_ioapic_read32(uintptr_t offset) {
    if (offset == 0x10U) {
        ++iowin_read_count;
        return mock_regs[selected_index & 0x3FU];
    }
    return 0U;
}

void host_ioapic_write32(uintptr_t offset, uint32_t value) {
    if (offset == 0x00U) {
        selected_index = value;
        ++ioregsel_write_count;
    } else if (offset == 0x10U) {
        if (is_isa_rte_index(selected_index) &&
            (selected_index & 1U) == 0U) {
            ++low_write_count;
        }
        if (selected_index == route_low_index(0U) ||
            selected_index == route_low_index(1U)) {
            if ((corrupt_first_low_write && low_write_count == 1U) ||
                (corrupt_second_low_write && low_write_count == 2U)) {
                value ^= 0x20U;
            }
            if (corrupt_irq0_low_write &&
                selected_index == route_low_index(0U)) {
                value ^= 0x20U;
            }
            if (corrupt_irq0_low_write_once &&
                selected_index == route_low_index(0U)) {
                value ^= 0x20U;
                corrupt_irq0_low_write_once = false;
            }
            value ^= low_ignore_bits;
        }
        if (selected_index == route_high_index(0U) ||
            selected_index == route_high_index(1U)) {
            value ^= high_corrupt_bits;
        }
        if (corrupt_isa_irq3_stage == 1U &&
            selected_index == route_low_index(3U) &&
            low_write_count == 1U) {
            value ^= 0x20U;
        }
        if (corrupt_isa_irq3_stage == 2U &&
            selected_index == route_high_index(3U)) {
            value ^= 1U;
        }
        if (corrupt_isa_irq3_stage == 3U &&
            selected_index == route_low_index(3U) &&
            low_write_count == 2U) {
            value ^= 0x20U;
        }
        if (corrupt_isa_irq3_stage == 4U &&
            selected_index == route_low_index(3U) &&
            low_write_count == 1U) {
            value &= ~(1U << 16U);
        }
        mock_regs[selected_index & 0x3FU] = value;
        ++iowin_write_count;
    }
}

static bool expect_status(enum ioapic_status actual, enum ioapic_status expected) {
    if (actual != expected) {
        printf("FAIL: expected status %d, got %d\n", expected, actual);
        ++failures;
        return false;
    }
    return true;
}

static void expect_string(const char *actual, const char *expected) {
    if (actual == NULL || strcmp(actual, expected) != 0) {
        printf("FAIL: expected string \"%s\", got \"%s\"\n",
               expected, actual == NULL ? "(null)" : actual);
        ++failures;
    }
}

static void check(bool condition, const char *message) {
    if (!condition) {
        printf("FAIL: %s\n", message);
        ++failures;
    }
}

static void test_status_strings(void) {
    ++test_count;
    static const struct {
        enum ioapic_status status;
        const char *expected;
    } cases[] = {
        {IOAPIC_OK,                 "OK"},
        {IOAPIC_NO_ACPI_SNAPSHOT,   "no ACPI snapshot"},
        {IOAPIC_NO_IOAPIC,          "no IOAPIC"},
        {IOAPIC_BAD_MMIO_ADDRESS,   "bad IOAPIC MMIO address"},
        {IOAPIC_HHDM_OVERFLOW,      "HHDM overflow"},
        {IOAPIC_MMIO_MAP_FAILED,    "MMIO map failed"},
        {IOAPIC_BAD_VERSION,        "bad IOAPIC version"},
        {IOAPIC_BAD_ISO,            "bad ISO"},
        {IOAPIC_NOT_INITIALIZED,    "not initialized"},
        {IOAPIC_LAPIC_NOT_ENABLED,  "LAPIC not enabled"},
        {IOAPIC_UNSUPPORTED_IRQ,    "unsupported IRQ"},
        {IOAPIC_ROUTE_FAILED,       "route failed"},
        {(enum ioapic_status)12,    "unknown"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        expect_string(ioapic_status_string(cases[i].status), cases[i].expected);
    }
}

static struct acpi_madt_info default_madt(void) {
    struct acpi_madt_info madt;
    memset(&madt, 0, sizeof(madt));
    madt.ioapic_count = 1U;
    madt.ioapics[0].ioapic_id = 1U;
    madt.ioapics[0].mmio_address = 0xFEC00000U;
    madt.ioapics[0].gsi_base = 0U;
    return madt;
}

static void test_null_madt(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    if (!expect_status(ioapic_init(NULL, 0U), IOAPIC_NO_ACPI_SNAPSHOT)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false after NULL MADT");
}

static void test_success_default_iso(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0x100000000ULL), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_probed(), "probed must be true after successful probe");
    check(ioregsel_write_count == 1U, "probe must write IOREGSEL once");
    check(iowin_write_count == 0U, "probe must not write IOWIN");
    check(iowin_read_count == 1U, "probe must read IOWIN once");
    check(!ioapic_test_irq_active_low(0U),
          "default IRQ0 must be active-high");
    check(!ioapic_test_irq_level_triggered(0U),
          "default IRQ0 must be edge-triggered");
    check(!ioapic_test_irq_active_low(1U),
          "default IRQ1 must be active-high");
    check(!ioapic_test_irq_level_triggered(1U),
          "default IRQ1 must be edge-triggered");
    check(ioapic_test_irq_gsi(0U) == 0U,
          "default IRQ0 GSI must be 0");
    check(ioapic_test_irq_gsi(1U) == 1U,
          "default IRQ1 GSI must be 1");
    check(ioapic_test_ioapic_id() == 1U,
          "probe must store the IOAPIC id");
    check(ioapic_test_mmio_base() == (uintptr_t)mock_map_out,
          "probe must store the mapped MMIO base");
    check(ioapic_test_gsi_base() == 0U,
          "probe must store the IOAPIC GSI base");
    check(ioapic_test_version() == 0x11U,
          "probe must store the IOAPIC version");
    check(ioapic_test_redirection_entries() == 24U,
          "probe must store redirection_entries = max + 1");
}

static void test_isa_descriptor_snapshot(void) {
    ++test_count;

    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    check(ioapic_test_isa_irq_desc_valid(3U),
          "default IRQ3 descriptor must be valid");
    check(ioapic_test_isa_irq_gsi(3U) == 3U,
          "default IRQ3 descriptor must use GSI 3");
    check(ioapic_test_isa_irq_desc_valid(15U),
          "default IRQ15 descriptor must be valid");
    check(ioapic_test_isa_irq_gsi(15U) == 15U,
          "default IRQ15 descriptor must use GSI 15");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 15U;
    madt.isos[0].gsi = 23U;
    madt.isos[0].flags = 0x000FU;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    check(ioapic_test_isa_irq_desc_valid(15U),
          "explicit IRQ15 descriptor must be valid");
    check(ioapic_test_isa_irq_gsi(15U) == 23U,
          "explicit IRQ15 descriptor must use GSI 23");
    check(ioapic_test_irq_active_low(15U),
          "explicit IRQ15 ISO must be active-low");
    check(ioapic_test_irq_level_triggered(15U),
          "explicit IRQ15 ISO must be level-triggered");
}

static void test_vmm_failure(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    vmm_map_fails = true;
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_MMIO_MAP_FAILED)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false after VMM failure");
    check(ioregsel_write_count == 0U, "VMM failure must not write IOREGSEL");
    check(iowin_write_count == 0U, "VMM failure must not write IOWIN");
    check(iowin_read_count == 0U, "VMM failure must not read IOWIN");
}

static void test_bad_version(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    mock_regs[0x01U] = 0U;
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_VERSION)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false after bad version");
}

static void test_no_ioapic(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.ioapic_count = 0U;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_NO_IOAPIC)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false without IOAPIC");
}

static void test_bad_mmio_address(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.ioapics[0].mmio_address = 0U;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_MMIO_ADDRESS)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false for zero MMIO address");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    madt.ioapics[0].mmio_address = 0xFEC00001U;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_MMIO_ADDRESS)) {
        return;
    }
    check(!ioapic_is_probed(),
          "probed must be false for unaligned MMIO address");
}

static void test_hhdm_overflow(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, UINT64_MAX),
                       IOAPIC_HHDM_OVERFLOW)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false after HHDM overflow");
    check(ioregsel_write_count == 0U,
          "HHDM overflow must not write IOREGSEL");
    check(iowin_write_count == 0U, "HHDM overflow must not write IOWIN");
    check(iowin_read_count == 0U, "HHDM overflow must not read IOWIN");
}

static void test_duplicate_iso(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 2U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 0U;
    madt.isos[0].gsi = 0U;
    madt.isos[1].bus = 0U;
    madt.isos[1].source = 0U;
    madt.isos[1].gsi = 1U;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_ISO)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false for duplicate ISO");
}

static void test_iso_out_of_range(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 0U;
    madt.isos[0].gsi = 24U;
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_ISO)) {
        return;
    }
    check(!ioapic_is_probed(), "probed must be false for GSI out of range");
}

static void test_explicit_iso(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 0U;
    madt.isos[0].gsi = 1U;
    madt.isos[0].flags = 0x000FU;

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    check(ioapic_test_irq_gsi(0U) == 1U,
          "explicit ISO must use its GSI");
    check(ioapic_test_irq_active_low(0U),
          "explicit ISO flags 0x0F must be active-low");
    check(ioapic_test_irq_level_triggered(0U),
          "explicit ISO flags 0x0F must be level-triggered");
}

static void test_iso_flags_case(
    uint16_t flags,
    bool expect_ok,
    bool expect_active_low,
    bool expect_level_triggered)
{
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 0U;
    madt.isos[0].gsi = 0U;
    madt.isos[0].flags = flags;

    enum ioapic_status st = ioapic_init(&madt, 0U);
    if (expect_ok) {
        if (!expect_status(st, IOAPIC_OK)) {
            printf("  flags=0x%04X\n", flags);
            return;
        }
        check(ioapic_test_irq_gsi(0U) == 0U,
              "IRQ0 GSI must use ISO GSI");
        check(ioapic_test_irq_active_low(0U) == expect_active_low,
              "unexpected active_low flag mapping");
        check(ioapic_test_irq_level_triggered(0U) == expect_level_triggered,
              "unexpected level_triggered flag mapping");
    } else {
        if (!expect_status(st, IOAPIC_BAD_ISO)) {
            printf("  flags=0x%04X\n", flags);
        }
    }
}

static void test_iso_flags(void) {
    ++test_count;
    static const struct {
        uint16_t flags;
        bool ok;
        bool active_low;
        bool level_triggered;
    } cases[] = {
        {0x0000U, true,  false, false},
        {0x0001U, true,  false, false},
        {0x0002U, false, false, false},
        {0x0003U, true,  true,  false},
        {0x0004U, true,  false, false},
        {0x0005U, true,  false, false},
        {0x0006U, false, false, false},
        {0x0007U, true,  true,  false},
        {0x0008U, false, false, false},
        {0x0009U, false, false, false},
        {0x000AU, false, false, false},
        {0x000BU, false, false, false},
        {0x000CU, true,  false, true},
        {0x000DU, true,  false, true},
        {0x000EU, false, false, false},
        {0x000FU, true,  true,  true},
        {0x0010U, false, false, false},
        {0x0100U, false, false, false},
        {0xFFF0U, false, false, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_iso_flags_case(
            cases[i].flags,
            cases[i].ok,
            cases[i].active_low,
            cases[i].level_triggered);
    }
}

static void test_iso_reserved_high_bits(void) {
    ++test_count;
    static const uint16_t flags[] = {
        0x0010U, 0x0020U, 0x0040U, 0x0080U,
        0x0100U, 0x0200U, 0x0400U, 0x0800U,
        0x1000U, 0x2000U, 0x4000U, 0x8000U,
        0xFFF0U, 0xFFFFU,
    };

    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        madt.iso_count = 1U;
        madt.isos[0].bus = 0U;
        madt.isos[0].source = 0U;
        madt.isos[0].gsi = 0U;
        madt.isos[0].flags = flags[i];
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_BAD_ISO)) {
            printf("  flags=0x%04X\n", flags[i]);
        }
    }
}

static void test_idempotent_init(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    uint32_t writes = ioregsel_write_count;
    uint32_t writes_iowin = iowin_write_count;
    uint32_t reads_iowin = iowin_read_count;

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_probed(), "probed must remain true on second init");
    check(ioregsel_write_count == writes,
          "second init must not write IOREGSEL again");
    check(iowin_write_count == writes_iowin,
          "second init must not write IOWIN again");
    check(iowin_read_count == reads_iowin,
          "second init must not read IOWIN again");
}

static void test_irq1_scaffold(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    check(!ioapic_is_irq1_routed(), "irq1_routed false after reset");
    check(iowin_write_count == 0U, "scaffold writes no IOWIN");
}

static void test_irq0_scaffold(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    ioapic_test_invalidate_irq0_desc();
    check(!ioapic_is_irq0_routed(), "irq0_routed false after reset");
    check(iowin_write_count == 0U, "IRQ0 scaffold writes no IOWIN");
    check(route_low_index(0U) == 0x10U,
          "IRQ0 RTE low index must be 0x10");
    check(route_high_index(0U) == 0x11U,
          "IRQ0 RTE high index must be 0x11");
    check(route_low_index(1U) == 0x12U,
          "IRQ1 RTE low index must remain 0x12");
    check(route_high_index(1U) == 0x13U,
          "IRQ1 RTE high index must remain 0x13");

    host_ioapic_write32(0x00U, route_low_index(0U));
    host_ioapic_write32(0x10U, 0x12345678U);
    check(mock_regs[0x10U] == 0x12345678U,
          "IRQ0 low write must update mock_regs[0x10]");

    host_ioapic_write32(0x00U, route_high_index(0U));
    host_ioapic_write32(0x10U, 0x87654321U);
    check(mock_regs[0x11U] == 0x87654321U,
          "IRQ0 high write must update mock_regs[0x11]");
    check(iowin_write_count == 2U,
          "IRQ0 scaffold writes two IOWIN values");
}

static void test_irq0_route_success(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;

    const uint32_t ioregsel_writes = ioregsel_write_count;
    const uint32_t iowin_writes = iowin_write_count;
    const uint32_t iowin_reads = iowin_read_count;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq0_routed(),
          "irq0_routed true after successful IRQ0 route");
    check(ioregsel_write_count == ioregsel_writes + 7U,
          "IRQ0 route writes IOREGSEL for masked/high/readback/unmasked/readback");
    check(iowin_write_count == iowin_writes + 3U,
          "IRQ0 route writes masked low, high, and unmasked low");
    check(iowin_read_count == iowin_reads + 4U,
          "IRQ0 route reads low/high before and after unmask");
    check(mock_regs[0x10U] == 0x40U,
          "IRQ0 RTE low must be unmasked vector 0x40");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 RTE high must contain BSP APIC ID 5");

    const uint32_t writes_after_route = iowin_write_count;
    const uint32_t reads_after_route = iowin_read_count;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(iowin_write_count == writes_after_route,
          "already routed IRQ0 must not write IOWIN");
    check(iowin_read_count == reads_after_route,
          "already routed IRQ0 must not read IOWIN");
}

static void test_irq0_mask_save_restore_success(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK) ||
        !expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }

    const uint32_t writes_before = iowin_write_count;
    const uint32_t reads_before = iowin_read_count;
    uint64_t previous = 0U;
    check(ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save succeeds on verified route");
    check(previous == (((uint64_t)0x05000000U << 32U) | 0x40U),
          "IRQ0 mask save returns exact previous 64-bit RTE");
    check((mock_regs[0x10U] & (1U << 16U)) != 0U,
          "IRQ0 mask save sets RTE mask bit");
    check((mock_regs[0x10U] & 0xFFU) == IOAPIC_IRQ0_VECTOR,
          "IRQ0 mask save preserves vector 0x40");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 mask save preserves RTE high word");
    check(mock_regs[0x12U] == IOAPIC_IRQ1_VECTOR,
          "IRQ0 mask save does not touch IRQ1 low word");
    check(mock_regs[0x13U] == 0x05000000U,
          "IRQ0 mask save does not touch IRQ1 high word");
    check(iowin_write_count == writes_before + 1U,
          "IRQ0 mask save writes one masked RTE low");
    check(iowin_read_count == reads_before + 4U,
          "IRQ0 mask save reads low/high before and after masked write");

    const uint32_t writes_after_mask = iowin_write_count;
    const uint32_t reads_after_mask = iowin_read_count;
    check(ioapic_irq0_restore(previous),
          "IRQ0 restore succeeds with saved exact RTE");
    check(mock_regs[0x10U] == IOAPIC_IRQ0_VECTOR,
          "IRQ0 restore writes exact previous low RTE");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 restore writes exact previous high RTE");
    check(mock_regs[0x12U] == IOAPIC_IRQ1_VECTOR,
          "IRQ0 restore does not touch IRQ1 low word");
    check(mock_regs[0x13U] == 0x05000000U,
          "IRQ0 restore does not touch IRQ1 high word");
    check(iowin_write_count == writes_after_mask + 2U,
          "IRQ0 restore writes high then low");
    check(iowin_read_count == reads_after_mask + 2U,
          "IRQ0 restore reads low/high after restore write");
}

static void test_irq0_mask_save_preconditions(void) {
    ++test_count;

    ioapic_test_reset();
    reset_mock_state();
    uint64_t previous = 0U;
    check(!ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save rejects uninitialized IOAPIC");
    check(iowin_write_count == 0U,
          "uninitialized mask save writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    check(!ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save rejects unrouted IRQ0");

    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(!ioapic_irq0_mask_save(NULL),
          "IRQ0 mask save rejects NULL output");
    ioapic_test_invalidate_irq0_desc();
    check(!ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save rejects invalid saved descriptor");
}

static void test_irq0_mask_save_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }

    corrupt_irq0_low_write = true;
    uint64_t previous = 0U;
    check(!ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save fails on low readback mismatch");
}

static void test_irq0_mask_save_readback_mismatch_restores(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }

    const uint32_t writes_before = iowin_write_count;
    const uint32_t reads_before = iowin_read_count;
    corrupt_irq0_low_write_once = true;
    uint64_t previous = 0U;
    check(!ioapic_irq0_mask_save(&previous),
          "IRQ0 mask save fails on one-shot low readback mismatch");
    check(previous == (((uint64_t)0x05000000U << 32U) | 0x40U),
          "IRQ0 mask save returns exact previous RTE before rollback");
    check(mock_regs[0x10U] == IOAPIC_IRQ0_VECTOR,
          "IRQ0 mask save restores exact previous low RTE");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 mask save restores exact previous high RTE");
    check(iowin_write_count == writes_before + 3U,
          "IRQ0 mask save rollback writes masked low then restore high/low");
    check(iowin_read_count == reads_before + 6U,
          "IRQ0 mask save rollback reads low/high before/after mask and restore");
}

static void test_irq0_restore_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    uint64_t previous = 0U;
    if (!ioapic_irq0_mask_save(&previous)) {
        return;
    }

    corrupt_irq0_low_write = true;
    check(!ioapic_irq0_restore(previous),
          "IRQ0 restore fails on low readback mismatch");

    corrupt_irq0_low_write = false;
    high_corrupt_bits = 1U;
    check(!ioapic_irq0_restore(previous),
          "IRQ0 restore fails on high readback mismatch");
}

static void test_irq0_route_rte_flags(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 0U;
    madt.isos[0].gsi = 1U;
    madt.isos[0].flags = 0x000FU;

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    const uint32_t expected_low =
        IOAPIC_IRQ0_VECTOR | (1U << 13U) | (1U << 15U);
    check(mock_regs[0x12U] == expected_low,
          "IRQ0 RTE low maps active-low and level-triggered ISO flags");
    check(mock_regs[0x13U] == 0x05000000U,
          "IRQ0 RTE high retains BSP APIC ID for explicit ISO");
}

static void test_irq0_route_low_ignores_bits_12_and_14(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    low_ignore_bits = (1U << 12U) | (1U << 14U);
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq0_routed(),
          "IRQ0 RTE bits 12/14 mismatch does not fail route");
    check((mock_regs[0x10U] & ~0x5000U) == (0x40U & ~0x5000U),
          "IRQ0 RTE low compare ignores only bits 12 and 14");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 RTE high compare remains exact");
}

static void test_irq0_route_high_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    high_corrupt_bits = 1U;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq0_routed(),
          "IRQ0 high readback mismatch leaves irq0_routed false");
    check(low_write_count == 2U,
          "IRQ0 high readback mismatch performs best-effort remask");
    check((mock_regs[0x10U] & (1U << 16U)) != 0U,
          "IRQ0 high readback mismatch leaves RTE masked");
}

static void test_irq0_route_precondition_failures(void) {
    ++test_count;

    ioapic_test_reset();
    reset_mock_state();
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_NOT_INITIALIZED)) {
        return;
    }
    check(ioregsel_write_count == 0U,
          "IRQ0 route before probe writes no IOREGSEL");
    check(iowin_write_count == 0U,
          "IRQ0 route before probe writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_enabled = false;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_LAPIC_NOT_ENABLED)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "IRQ0 disabled LAPIC route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "IRQ0 disabled LAPIC route writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    if (!expect_status(ioapic_route_irq(2U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_UNSUPPORTED_IRQ)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "IRQ0 unsupported IRQ route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "IRQ0 unsupported IRQ route writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ1_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "IRQ0 invalid vector route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "IRQ0 invalid vector route writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    ioapic_test_invalidate_irq0_desc();
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "IRQ0 invalid saved state route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "IRQ0 invalid saved state route writes no IOWIN");
}

static void test_irq0_masked_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    corrupt_first_low_write = true;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq0_routed(),
          "IRQ0 masked readback mismatch leaves irq0_routed false");
    check(low_write_count == 2U,
          "IRQ0 masked readback mismatch performs best-effort remask");
    check((mock_regs[0x10U] & (1U << 16U)) != 0U,
          "IRQ0 masked readback mismatch leaves RTE masked");
}

static void test_irq0_final_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    corrupt_second_low_write = true;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq0_routed(),
          "IRQ0 final readback mismatch leaves irq0_routed false");
    check(low_write_count == 3U,
          "IRQ0 final readback mismatch performs best-effort remask");
    check((mock_regs[0x10U] & (1U << 16U)) != 0U,
          "IRQ0 final readback mismatch remasks bit 16");
    check((mock_regs[0x10U] & ~0x5000U) ==
              (0x00010040U & ~0x5000U),
          "IRQ0 best-effort remask restores masked RTE low");
}

/* Task 5: IRQ0 must never publish IRQ1 route state. */
static void test_irq0_irq1_independent_routes(void) {
    ++test_count;

    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq1_routed(),
          "IRQ1 route must publish irq1_routed");
    check(!ioapic_is_irq0_routed(),
          "IRQ1 route must not publish irq0_routed");
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq0_routed(),
          "IRQ1 route must not block later IRQ0 route");
    check(mock_regs[0x12U] == 0x41U,
          "IRQ1 RTE low remains vector 0x41");
    check(mock_regs[0x13U] == 0x05000000U,
          "IRQ1 RTE high remains BSP APIC ID 5");
    check(mock_regs[0x10U] == 0x40U,
          "IRQ0 RTE low is vector 0x40 after IRQ1 route");
    check(mock_regs[0x11U] == 0x05000000U,
          "IRQ0 RTE high is BSP APIC ID 5 after IRQ1 route");

    const uint32_t writes_after_both = iowin_write_count;
    const uint32_t reads_after_both = iowin_read_count;
    if (!expect_status(ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR), IOAPIC_OK) ||
        !expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(iowin_write_count == writes_after_both,
          "already routed IRQ0/IRQ1 must not write IOWIN");
    check(iowin_read_count == reads_after_both,
          "already routed IRQ0/IRQ1 must not read IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    const enum ioapic_status st =
        ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR);
    if (!expect_status(st, IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq0_routed(),
          "IRQ0 route must publish irq0_routed");
    const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
    check(!irq1_ready, "IRQ0 route must not publish irq1_routed");
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq1_routed(),
          "IRQ0 route must not block later IRQ1 route");
}

static void test_isa_route_irq3_success(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;

    if (!expect_status(ioapic_route_isa_irq(3U, 0x44U), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_isa_irq_routed(3U),
          "IRQ3 route must publish only after final readback");
    check(mock_regs[0x16U] == 0x44U,
          "IRQ3 RTE low must be unmasked vector 0x44");
    check(mock_regs[0x17U] == 0x05000000U,
          "IRQ3 RTE high must contain BSP APIC ID");
    check(!ioapic_is_irq0_routed() && !ioapic_is_irq1_routed(),
          "IRQ3 route must not publish IRQ0 or IRQ1 state");

    const uint32_t writes_after_success = iowin_write_count;
    expect_status(ioapic_route_isa_irq(2U, 0x43U), IOAPIC_UNSUPPORTED_IRQ);
    expect_status(ioapic_route_isa_irq(16U, 0x51U), IOAPIC_UNSUPPORTED_IRQ);
    expect_status(ioapic_route_isa_irq(3U, 0x45U), IOAPIC_ROUTE_FAILED);
    check(iowin_write_count == writes_after_success,
          "rejected common routes must not write IOWIN");
}

static void test_irq3_route_failure_paths(void) {
    ++test_count;

    {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
            return;
        }
        corrupt_isa_irq3_stage = 1U;
        expect_status(ioapic_route_isa_irq(3U, 0x44U), IOAPIC_ROUTE_FAILED);
        check(!ioapic_is_isa_irq_routed(3U),
              "IRQ3 masked-write corruption must not publish route state");
        check((mock_regs[0x16U] & (1U << 16U)) != 0U,
              "IRQ3 masked-write corruption must leave RTE masked");
    }

    {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
            return;
        }
        corrupt_isa_irq3_stage = 2U;
        expect_status(ioapic_route_isa_irq(3U, 0x44U), IOAPIC_ROUTE_FAILED);
        check(!ioapic_is_isa_irq_routed(3U),
              "IRQ3 high-write corruption must not publish route state");
        check((mock_regs[0x16U] & (1U << 16U)) != 0U,
              "IRQ3 high-write corruption must leave RTE masked");
    }

    {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
            return;
        }
        corrupt_isa_irq3_stage = 3U;
        expect_status(ioapic_route_isa_irq(3U, 0x44U), IOAPIC_ROUTE_FAILED);
        check(!ioapic_is_isa_irq_routed(3U),
              "IRQ3 unmasked-write corruption must not publish route state");
        check((mock_regs[0x16U] & (1U << 16U)) != 0U,
              "IRQ3 unmasked-write corruption must remask RTE");
        check((mock_regs[0x16U] & ~0x5000U) ==
                  (0x00010044U & ~0x5000U),
              "IRQ3 best-effort remask restores masked RTE low");
    }
}

static void test_irq3_masked_low_readback_remask(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }

    corrupt_isa_irq3_stage = 4U;
    expect_status(ioapic_route_isa_irq(3U, 0x44U), IOAPIC_ROUTE_FAILED);
    check(!ioapic_is_isa_irq_routed(3U),
          "masked-low unmask corruption must not publish IRQ3 route state");
    check((mock_regs[0x16U] & (1U << 16U)) != 0U,
          "masked-low unmask corruption must leave RTE masked after failure");
    check((mock_regs[0x16U] & ~0x5000U) ==
              (0x00010044U & ~0x5000U),
          "masked-low failure must best-effort remask computed RTE low");
}

/* Task 5: route status and routed accessor jointly drive irq1_ready. */
static void test_route_success(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;

    const uint32_t ioregsel_writes = ioregsel_write_count;
    const uint32_t iowin_writes = iowin_write_count;
    const uint32_t iowin_reads = iowin_read_count;
    const enum ioapic_status st =
        ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);
    if (!expect_status(st, IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq1_routed(),
          "successful IRQ1 route sets irq1_routed for policy");
    const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
    check(irq1_ready,
          "successful IRQ1 route satisfies irq1_ready initializer");
    check(ioregsel_write_count == ioregsel_writes + 7U,
          "route writes IOREGSEL for masked/high/readback/unmasked/readback");
    check(iowin_write_count == iowin_writes + 3U,
          "route writes masked low, high, and unmasked low");
    check(iowin_read_count == iowin_reads + 4U,
          "route reads low/high before and after unmask");
    check(mock_regs[0x12U] == 0x41U,
          "RTE low must be unmasked vector 0x41");
    check(mock_regs[0x13U] == 0x05000000U,
          "RTE high must contain BSP APIC ID 5");

    const uint32_t writes_after_route = iowin_write_count;
    const uint32_t reads_after_route = iowin_read_count;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(iowin_write_count == writes_after_route,
          "already routed IRQ1 must not write IOWIN");
    check(iowin_read_count == reads_after_route,
          "already routed IRQ1 must not read IOWIN");
}

static void test_route_rte_flags(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    madt.iso_count = 1U;
    madt.isos[0].bus = 0U;
    madt.isos[0].source = 1U;
    madt.isos[0].gsi = 1U;
    madt.isos[0].flags = 0x000FU;

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    const uint32_t expected_low =
        IOAPIC_IRQ1_VECTOR | (1U << 13U) | (1U << 15U);
    check(mock_regs[0x12U] == expected_low,
          "RTE low maps active-low and level-triggered ISO flags");
    check(mock_regs[0x13U] == 0x05000000U,
          "RTE high retains BSP APIC ID for explicit ISO");
}

static void test_route_precondition_failures(void) {
    ++test_count;

    ioapic_test_reset();
    reset_mock_state();
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR),
                       IOAPIC_NOT_INITIALIZED)) {
        return;
    }
    check(ioregsel_write_count == 0U,
          "route before probe writes no IOREGSEL");
    check(iowin_write_count == 0U,
          "route before probe writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_enabled = false;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR),
                       IOAPIC_LAPIC_NOT_ENABLED)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "disabled LAPIC route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "disabled LAPIC route writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    if (!expect_status(ioapic_route_irq(2U, IOAPIC_IRQ0_VECTOR),
                       IOAPIC_UNSUPPORTED_IRQ)) {
        return;
    }
    check(ioregsel_write_count == 1U,
          "unsupported IRQ route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "unsupported IRQ route writes no IOWIN");

    ioapic_test_reset();
    reset_mock_state();
    madt = default_madt();
    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    const enum ioapic_status st =
        ioapic_route_irq(1U, IOAPIC_IRQ0_VECTOR);
    if (!expect_status(st, IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq1_routed(),
          "invalid vector route leaves irq1_routed false for fallback policy");
    const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
    check(!irq1_ready,
          "invalid vector route leaves irq1_ready false for fallback policy");
    check(ioregsel_write_count == 1U,
          "invalid vector route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "invalid vector route writes no IOWIN");
}

static void test_route_vmm_failure_no_writes(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    vmm_map_fails = true;
    struct acpi_madt_info madt = default_madt();

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_MMIO_MAP_FAILED)) {
        return;
    }
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR),
                       IOAPIC_NOT_INITIALIZED)) {
        return;
    }
    check(ioregsel_write_count == 0U,
          "route after VMM failure writes no IOREGSEL");
    check(iowin_write_count == 0U,
          "route after VMM failure writes no IOWIN");
}

static void test_route_invalid_saved_state_no_writes(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    ioapic_test_invalidate_irq1_desc();
    const enum ioapic_status st =
        ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);
    if (!expect_status(st, IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq1_routed(),
          "invalid saved IRQ1 descriptor leaves irq1_routed false for fallback policy");
    const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
    check(!irq1_ready,
          "invalid saved IRQ1 descriptor leaves irq1_ready false for fallback policy");
    check(ioregsel_write_count == 1U,
          "invalid saved state route writes no extra IOREGSEL");
    check(iowin_write_count == 0U,
          "invalid saved state route writes no IOWIN");
}

static void test_route_low_ignores_bits_12_and_14(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    host_lapic_id = 5U;
    low_ignore_bits = (1U << 12U) | (1U << 14U);
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR), IOAPIC_OK)) {
        return;
    }
    check(ioapic_is_irq1_routed(),
          "RTE bits 12/14 mismatch does not fail route");
    check((mock_regs[0x12U] & ~0x5000U) == (0x41U & ~0x5000U),
          "RTE low compare ignores only bits 12 and 14");
    check(mock_regs[0x13U] == 0x05000000U,
          "RTE high compare remains exact");
}

static void test_route_high_readback_mismatch(void) {
    ++test_count;
    ioapic_test_reset();
    reset_mock_state();
    struct acpi_madt_info madt = default_madt();

    if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
        return;
    }
    high_corrupt_bits = 1U;
    if (!expect_status(ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR),
                       IOAPIC_ROUTE_FAILED)) {
        return;
    }
    check(!ioapic_is_irq1_routed(),
          "high readback mismatch leaves irq1_routed false");
    check(low_write_count == 2U,
          "high readback mismatch performs best-effort remask");
    check((mock_regs[0x12U] & (1U << 16U)) != 0U,
          "high readback mismatch leaves RTE masked");
}

static void test_route_readback_mismatch_paths(void) {
    ++test_count;

    {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
            return;
        }
        corrupt_first_low_write = true;
        const enum ioapic_status st =
            ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);
        if (!expect_status(st, IOAPIC_ROUTE_FAILED)) {
            return;
        }
        const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
        check(!ioapic_is_irq1_routed(),
              "masked readback mismatch does not publish IRQ1 route state");
        check(!irq1_ready,
              "masked readback mismatch conjunction is false");
        check((mock_regs[0x12U] & (1U << 16U)) != 0U,
              "masked readback mismatch leaves RTE masked");
    }

    {
        ioapic_test_reset();
        reset_mock_state();
        struct acpi_madt_info madt = default_madt();
        if (!expect_status(ioapic_init(&madt, 0U), IOAPIC_OK)) {
            return;
        }
        corrupt_second_low_write = true;
        const enum ioapic_status st =
            ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);
        if (!expect_status(st, IOAPIC_ROUTE_FAILED)) {
            return;
        }
        const bool irq1_ready = st == IOAPIC_OK && ioapic_is_irq1_routed();
        check(!ioapic_is_irq1_routed(),
              "final readback mismatch does not publish IRQ1 route state");
        check(!irq1_ready,
              "final readback mismatch conjunction is false");
        check((mock_regs[0x12U] & (1U << 16U)) != 0U,
              "final readback mismatch performs best-effort remask");
        check((mock_regs[0x12U] & ~0x5000U) ==
                  (0x00010041U & ~0x5000U),
              "best-effort remask restores masked RTE low");
    }
}

int main(void) {
    test_status_strings();
    test_null_madt();
    test_success_default_iso();
    test_isa_descriptor_snapshot();
    test_vmm_failure();
    test_bad_version();
    test_no_ioapic();
    test_bad_mmio_address();
    test_hhdm_overflow();
    test_explicit_iso();
    test_duplicate_iso();
    test_iso_out_of_range();
    test_iso_flags();
    test_iso_reserved_high_bits();
    test_idempotent_init();
    test_irq1_scaffold();
    test_irq0_scaffold();
    test_irq0_route_success();
    test_irq0_mask_save_restore_success();
    test_irq0_mask_save_preconditions();
    test_irq0_mask_save_readback_mismatch();
    test_irq0_mask_save_readback_mismatch_restores();
    test_irq0_restore_readback_mismatch();
    test_irq0_route_rte_flags();
    test_irq0_route_low_ignores_bits_12_and_14();
    test_irq0_route_high_readback_mismatch();
    test_irq0_route_precondition_failures();
    test_irq0_masked_readback_mismatch();
    test_irq0_final_readback_mismatch();
    test_irq0_irq1_independent_routes();
    test_isa_route_irq3_success();
    test_irq3_route_failure_paths();
    test_irq3_masked_low_readback_remask();
    test_route_success();
    test_route_rte_flags();
    test_route_precondition_failures();
    test_route_vmm_failure_no_writes();
    test_route_invalid_saved_state_no_writes();
    test_route_low_ignores_bits_12_and_14();
    test_route_high_readback_mismatch();
    test_route_readback_mismatch_paths();

    if (failures != 0) {
        printf("IOAPIC host tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("IOAPIC host scaffold: OK\n");
    printf("IOAPIC host route core: OK\n");
    printf("IOAPIC host tests: ALL OK (%d tests)\n", test_count);
    return 0;
}
