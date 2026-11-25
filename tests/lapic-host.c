#include "lapic.h"
#include "acpi.h"
#include "cpu.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LAPIC_HOST_TEST
static struct cpu host_cpu;
static struct cpu ap_cpu;

/* Test seam: external linkage, defined in this file. */
enum vmm_status lapic_test_vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual);

/* Mock state for lapic.c host primitives. */
static uint64_t mock_msr;
static uint32_t mock_mmio[1024];
static uint32_t ap_mock_mmio[1024];
static uint32_t mock_x2apic_msr[256];
static bool cpuid_apic_flag = true;
static bool cpuid_x2apic_flag = false;
static bool msr_written;
static uint32_t last_wrmsr_msr;
static uint64_t last_wrmsr_value;
static bool mmio_read_called;
static bool mmio_write_called;
static uint32_t svr_written_value;
static bool svr_was_written;
static bool apic_enable_sticks = true;
static bool x2apic_enable_sticks = true;
static bool x2apic_rollback_sticks = true;
static uint32_t x2apic_corrupt_msr = UINT32_MAX;
static uint32_t x2apic_corrupt_value;

/* LINT0 and APIC-disable mock state for future Tasks 2-3. */
static uint32_t original_lint0;
static bool lint0_config_write_seen;
static uint32_t lint0_configured_value;
static bool lint0_readback_corrupt_delivery;
static bool lint0_readback_keep_mask;
static bool apic_disable_sticks = true;
static bool lint1_read_called;
static bool lint1_write_called;
static char serial_capture[1024];
static size_t serial_capture_length;

/* VMM test seam state. */
static enum vmm_status seam_status = VMM_OK;
static volatile void *seam_out_virtual;

enum vmm_status lapic_test_vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual)
{
    (void)physical_address;
    (void)hhdm_offset;
    if (seam_status == VMM_OK && out_virtual != NULL) {
        *out_virtual = seam_out_virtual;
    }
    return seam_status;
}

/* MSR / MMIO / CPUID mocks. */
uint64_t host_rdmsr(uint32_t msr) {
    if (msr == 0x1BU) {
        return mock_msr;
    }
    if (msr >= 0x800U && msr < 0x900U) {
        const uint32_t index = msr - 0x800U;
        if (x2apic_corrupt_msr == index) {
            return x2apic_corrupt_value;
        }
        return mock_x2apic_msr[index];
    }
    return 0ULL;
}

void host_wrmsr(uint32_t msr, uint64_t value) {
    msr_written = true;
    last_wrmsr_msr = msr;
    last_wrmsr_value = value;
    /* Simulate IA32_APIC_BASE enable failure: when apic_enable_sticks
     * is false and value has bit 11 set, read-back clears bit 11. */
    if (msr == 0x1BU) {
        if ((value & (1ULL << 10U)) && !x2apic_enable_sticks) {
            mock_msr = value & ~(1ULL << 10U);
        } else if ((value & (1ULL << 11U)) && !apic_enable_sticks) {
            mock_msr = value & ~(1ULL << 11U);
        } else if ((value & (1ULL << 11U)) == 0ULL
                   && !apic_disable_sticks) {
            mock_msr = value | (1ULL << 11U);
        } else if ((value & ((1ULL << 11U) | (1ULL << 10U))) == 0ULL
                   && !x2apic_rollback_sticks) {
            mock_msr = value | (1ULL << 11U);
        } else {
            mock_msr = value;
        }
    } else if (msr >= 0x800U && msr < 0x900U) {
        mock_x2apic_msr[msr - 0x800U] = (uint32_t)value;
    }
}

uint32_t host_mmio_read32(uint64_t offset) {
    uint32_t *storage = cpu_current()->index == 0U
        ? mock_mmio : ap_mock_mmio;
    mmio_read_called = true;
    if (offset == 0x350U) {
        if (!lint0_config_write_seen) {
            original_lint0 = storage[offset / 4U];
            return original_lint0;
        }

        uint32_t value = storage[offset / 4U];
        if (lint0_readback_corrupt_delivery) {
            value &= ~(7U << 8U);
            value |= 6U << 8U;
        }
        if (lint0_readback_keep_mask) {
            value |= 1U << 16U;
        }
        return value;
    }
    if (offset == 0x360U) {
        lint1_read_called = true;
    }
    return storage[offset / 4U];
}

void host_mmio_write32(uint64_t offset, uint32_t value) {
    uint32_t *storage = cpu_current()->index == 0U
        ? mock_mmio : ap_mock_mmio;
    mmio_write_called = true;
    storage[offset / 4U] = value;
    if (offset == 0x0F0U) {
        svr_written_value = value;
        svr_was_written = true;
    }
    if (offset == 0x360U) {
        lint1_write_called = true;
    }
    if (offset == 0x350U && value != original_lint0) {
        lint0_config_write_seen = true;
        lint0_configured_value = value;
    }
}

bool host_cpuid_has_apic_flag(void) {
    return cpuid_apic_flag;
}

bool host_cpuid_has_x2apic_flag(void) {
    return cpuid_x2apic_flag;
}

/* Host-link stubs required by lapic.c's COM1 diagnostics.
 * Linked instead of kernel/src/serial.c and kernel/src/vmm.c. */
uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void serial_write(const char *text) {
    while (*text != '\0' && serial_capture_length + 1U < sizeof(serial_capture)) {
        serial_capture[serial_capture_length++] = *text++;
    }
    serial_capture[serial_capture_length] = '\0';
}
void serial_write_u64(uint64_t value) { (void)value; }
void serial_write_hex(uint64_t value) { (void)value; }

static bool serial_contains(const char *text) {
    return strstr(serial_capture, text) != NULL;
}

const char *vmm_status_string(enum vmm_status status) {
    (void)status;
    return "host VMM status";
}

/* Reset all mock state before each independent test case. */
static void reset_mock_state(void) {
    mock_msr = 0;
    memset(mock_mmio, 0, sizeof(mock_mmio));
    memset(ap_mock_mmio, 0, sizeof(ap_mock_mmio));
    memset(mock_x2apic_msr, 0, sizeof(mock_x2apic_msr));
    memset(&ap_cpu, 0, sizeof(ap_cpu));
    ap_cpu.index = 1U;
    cpuid_apic_flag = true;
    cpuid_x2apic_flag = false;
    msr_written = false;
    last_wrmsr_msr = 0;
    last_wrmsr_value = 0;
    mmio_read_called = false;
    mmio_write_called = false;
    svr_written_value = 0;
    svr_was_written = false;
    apic_enable_sticks = true;
    x2apic_enable_sticks = true;
    x2apic_rollback_sticks = true;
    x2apic_corrupt_msr = UINT32_MAX;
    x2apic_corrupt_value = 0;
    original_lint0 = 0U;
    lint0_config_write_seen = false;
    lint0_configured_value = 0U;
    lint0_readback_corrupt_delivery = false;
    lint0_readback_keep_mask = false;
    apic_disable_sticks = true;
    lint1_read_called = false;
    lint1_write_called = false;
    serial_capture_length = 0U;
    serial_capture[0] = '\0';
    seam_status = VMM_OK;
    seam_out_virtual = (volatile void *)0xFEE00000ULL;
}

static void begin_case(void) {
    memset(&host_cpu, 0, sizeof(host_cpu));
    host_cpu.index = 0U;
    cpu_gs_base_set(&host_cpu);
    lapic_test_reset();
    reset_mock_state();
}

static struct acpi_madt_info make_madt(uint64_t lapic_address) {
    struct acpi_madt_info madt;
    memset(&madt, 0, sizeof(madt));
    madt.lapic_address = lapic_address;
    return madt;
}
#endif

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static void prepare_x2apic_host_case(uint8_t apic_id) {
    begin_case();
    struct acpi_madt_info madt = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = (uint32_t)apic_id << 24U;
    mock_mmio[0x0F0U / 4U] = 0x000001EFU;
    mock_mmio[0x350U / 4U] = 0x00012400U;
    mock_mmio[0x320U / 4U] = 0x00020042U;
    mock_mmio[0x3E0U / 4U] = 0x3U;
    mock_mmio[0x380U / 4U] = 625000U;
    cpuid_x2apic_flag = true;
    CHECK(lapic_init(&madt, NULL, 0) == LAPIC_OK,
          "x2APIC host case LAPIC init succeeds");
    mock_x2apic_msr[0x020U >> 4U] = (uint32_t)apic_id << 24U;
    mock_x2apic_msr[0x030U >> 4U] = 0x00050014U;
}

static void test_lapic_ap_init(void) {
    begin_case();
    struct acpi_madt_info madt = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL | (1ULL << 11U);
    mock_mmio[0x030U / 4U] = 0x00050014U; /* version 5, max LVT 5 */
    mock_mmio[0x020U / 4U] = 0x01000000U; /* APIC ID 1 */

    CHECK(lapic_init_ap(&madt, 0xffff800000000000ULL, 1U) == LAPIC_OK,
          "AP LAPIC init succeeds");
    CHECK(host_cpu.lapic.initialized,
          "AP LAPIC init publishes per-CPU state");
    CHECK(host_cpu.lapic.apic_id == 1U,
          "AP LAPIC init stores verified APIC ID");
    CHECK(svr_was_written, "AP LAPIC init programs SVR");
    CHECK(svr_written_value == 0x1EFU,
          "AP SVR uses vector 0xEF and software enable");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 16U)) != 0U,
          "AP timer LVT is masked");
    CHECK((mock_mmio[0x330U / 4U] & (1U << 16U)) != 0U,
          "AP thermal LVT is masked");
    CHECK((mock_mmio[0x340U / 4U] & (1U << 16U)) != 0U,
          "AP performance LVT is masked");
    CHECK((mock_mmio[0x350U / 4U] & (1U << 16U)) != 0U,
          "AP LINT0 LVT is masked");
    CHECK((mock_mmio[0x360U / 4U] & (1U << 16U)) != 0U,
          "AP LINT1 LVT is masked");
    CHECK((mock_mmio[0x370U / 4U] & (1U << 16U)) != 0U,
          "AP error LVT is masked");
    CHECK(mock_mmio[0x2F0U / 4U] == 0U,
          "unimplemented CMCI LVT is not touched");

    begin_case();
    mock_msr = 0xFEE00000ULL | (1ULL << 11U);
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x01000000U;
    CHECK(lapic_init_ap(&madt, 0xffff800000000000ULL, 2U) ==
              LAPIC_APIC_ID_MISMATCH,
          "AP LAPIC init rejects APIC ID mismatch");
    CHECK(!host_cpu.lapic.initialized,
          "APIC ID mismatch does not publish AP LAPIC state");
}

static void test_lapic_ap_eoi(void) {
    begin_case();
    struct acpi_madt_info madt = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL | (1ULL << 11U);
    cpu_gs_base_set(&ap_cpu);
    ap_mock_mmio[0x030U / 4U] = 0x00050014U;
    ap_mock_mmio[0x020U / 4U] = 0x01000000U;

    CHECK(lapic_init_ap(&madt, 0xffff800000000000ULL, 1U) == LAPIC_OK,
          "AP LAPIC init succeeds for per-CPU EOI case");
    CHECK(ap_cpu.lapic.initialized,
          "AP LAPIC state is published for per-CPU EOI case");

    mock_mmio[0x0B0U / 4U] = 0xCAFEBABEU;
    ap_mock_mmio[0x0B0U / 4U] = 0x12345678U;
    mmio_write_called = false;
    lapic_eoi();
    CHECK(ap_mock_mmio[0x0B0U / 4U] == 0U,
          "AP lapic_eoi writes only the AP EOI register");
    CHECK(mock_mmio[0x0B0U / 4U] == 0xCAFEBABEU,
          "AP lapic_eoi does not write the BSP EOI register");
}

int main(void) {
#ifdef HFOC_LAPIC_HOST_TEST
    /* 1. lapic_status_string coverage (all 15 statuses). */
    CHECK(strcmp(lapic_status_string(LAPIC_OK), "OK") == 0,
          "status_string LAPIC_OK");
    CHECK(strcmp(lapic_status_string(LAPIC_NO_CPUID_APIC), "no CPUID APIC") == 0,
          "status_string LAPIC_NO_CPUID_APIC");
    CHECK(strcmp(lapic_status_string(LAPIC_NO_ACPI_SNAPSHOT), "no ACPI snapshot") == 0,
          "status_string LAPIC_NO_ACPI_SNAPSHOT");
    CHECK(strcmp(lapic_status_string(LAPIC_BAD_LAPIC_ADDRESS), "bad LAPIC address") == 0,
          "status_string LAPIC_BAD_LAPIC_ADDRESS");
    CHECK(strcmp(lapic_status_string(LAPIC_X2APIC_DETECTED), "x2APIC detected") == 0,
          "status_string LAPIC_X2APIC_DETECTED");
    CHECK(strcmp(lapic_status_string(LAPIC_MSR_BASE_MISMATCH), "MSR base mismatch") == 0,
          "status_string LAPIC_MSR_BASE_MISMATCH");
    CHECK(strcmp(lapic_status_string(LAPIC_HHDM_OVERFLOW), "HHDM overflow") == 0,
          "status_string LAPIC_HHDM_OVERFLOW");
    CHECK(strcmp(lapic_status_string(LAPIC_RANGE_NOT_MAPPED), "range not mapped") == 0,
          "status_string LAPIC_RANGE_NOT_MAPPED preserved");
    CHECK(strcmp(lapic_status_string(LAPIC_BAD_VERSION), "bad version") == 0,
          "status_string LAPIC_BAD_VERSION");
    CHECK(strcmp(lapic_status_string(LAPIC_ENABLE_FAILED), "enable failed") == 0,
          "status_string LAPIC_ENABLE_FAILED");
    CHECK(strcmp(lapic_status_string(LAPIC_MMIO_MAP_FAILED), "MMIO map failed") == 0,
          "status_string LAPIC_MMIO_MAP_FAILED");
    CHECK(strcmp(lapic_status_string(LAPIC_LINT0_FAILED), "LINT0 failed") == 0,
          "status_string LAPIC_LINT0_FAILED");
    CHECK(strcmp(lapic_status_string(LAPIC_DISABLE_FAILED), "disable failed") == 0,
          "status_string LAPIC_DISABLE_FAILED");
    CHECK(strcmp(lapic_status_string(LAPIC_APIC_ID_MISMATCH),
                 "APIC ID mismatch") == 0,
          "status_string LAPIC_APIC_ID_MISMATCH");
    CHECK(strcmp(lapic_status_string(LAPIC_AP_READBACK_FAILED),
                 "AP readback failed") == 0,
          "status_string LAPIC_AP_READBACK_FAILED");
    CHECK(strcmp(lapic_status_string((enum lapic_status)99), "unknown") == 0,
          "status_string unknown status");

    test_lapic_ap_init();
    test_lapic_ap_eoi();

    /* 1b. Task 1 x2APIC capability detection. */
    begin_case();
    cpuid_x2apic_flag = false;
    CHECK(!lapic_x2apic_capable(),
          "x2APIC capability false when CPUID bit 21 is clear");
    CHECK(!msr_written, "capability detection does not write MSRs");
    CHECK(!mmio_read_called, "capability detection does not read MMIO");
    CHECK(!mmio_write_called, "capability detection does not write MMIO");

    begin_case();
    cpuid_x2apic_flag = true;
    CHECK(lapic_x2apic_capable(),
          "x2APIC capability true when CPUID bit 21 is set");
    CHECK(!msr_written, "capable detection does not write MSRs");
    CHECK(!mmio_read_called, "capable detection does not read MMIO");
    CHECK(!mmio_write_called, "capable detection does not write MMIO");

    begin_case();
    struct acpi_madt_info madt_x2apic_cap = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL | (1ULL << 10U);
    cpuid_x2apic_flag = true;
    CHECK(lapic_init(&madt_x2apic_cap, NULL, 0) == LAPIC_X2APIC_DETECTED,
          "capable CPU retains LAPIC_X2APIC_DETECTED fail-closed path");
    CHECK(!lapic_is_enabled(), "xAPIC is not published for pre-enabled x2APIC");
    CHECK(!msr_written, "pre-enabled x2APIC detection performs no MSR write");

    /* 2. Task 2 LINT0 success: ExtINT delivery, unmasked, success marker. */
    begin_case();
    struct acpi_madt_info madt_lint0_ok = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x00000000U;
    mock_mmio[0x350U / 4U] = 0x00012400U; /* mask + controlled polarity */
    mock_mmio[0x360U / 4U] = 0xDEADBEEFU; /* LINT1 must stay untouched */
    const uint32_t expected_configured_lint0 =
        (0x00012400U & ~((7U << 8U) | (1U << 16U))) | (7U << 8U);
    CHECK(lapic_init(&madt_lint0_ok, NULL, 0) == LAPIC_OK,
          "LINT0 success returns LAPIC_OK");
    CHECK(lint0_config_write_seen, "LINT0 configuration was written");
    CHECK(lint0_configured_value == expected_configured_lint0,
          "LINT0 configured value preserves controlled bits");
    CHECK((lint0_configured_value & (7U << 8U)) == (7U << 8U),
          "LINT0 configured value uses ExtINT delivery");
    CHECK((lint0_configured_value & (1U << 16U)) == 0U,
          "LINT0 configured value clears mask");
    CHECK((mock_mmio[0x350U / 4U] & (7U << 8U)) == (7U << 8U),
          "LINT0 read-back delivery mode is ExtINT");
    CHECK((mock_mmio[0x350U / 4U] & (1U << 16U)) == 0U,
          "LINT0 read-back mask bit is clear");
    CHECK((mock_mmio[0x350U / 4U] & ~((7U << 8U) | (1U << 16U))) ==
              (0x00012400U & ~((7U << 8U) | (1U << 16U))),
          "LINT0 RMW preserves other controlled bits");
    CHECK(svr_was_written, "SVR programmed after LINT0 success");
    CHECK(lapic_is_enabled(), "lapic_is_enabled true after LINT0 success");
    CHECK(host_cpu.lapic.initialized,
          "lapic_init publishes per-CPU LAPIC state");
    CHECK(host_cpu.lapic.apic_id == 0U,
          "lapic_init stores APIC ID in per-CPU state");
    CHECK(serial_contains("[LAPIC] LINT0 configured: delivery=ExtINT mask=0; "
                          "legacy PIC fallback ready"),
          "LINT0 success marker present");
    CHECK(!lint1_read_called, "LINT1 not read on success");
    CHECK(!lint1_write_called, "LINT1 not written on success");

    /* 2.1. Narrow LAPIC timer hardware helpers after successful init. */
    CHECK(lapic_timer_hw_arm(0x42U, 0x3U, 0xFFFFFFFFU),
          "masked LAPIC timer arm succeeds after LAPIC init");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 16U)) != 0U,
          "LVT timer mask bit remains set");
    CHECK((mock_mmio[0x320U / 4U] & 0xFFU) == 0x42U,
          "LVT timer stores non-delivered value 0x42");
    CHECK(mock_mmio[0x3E0U / 4U] == 0x3U,
          "divide configuration is fixed to divide by 16");
    CHECK(mock_mmio[0x380U / 4U] == 0xFFFFFFFFU,
          "initial count is maximum");

    /* 2.1b. Narrow one-shot LVT helpers after successful init. */
    CHECK(lapic_timer_hw_program_masked_oneshot(0x43U, 0x3U, 62500U),
          "masked one-shot programming succeeds");
    CHECK(mock_mmio[0x320U / 4U] == (0x43U | (1U << 16U)),
          "masked one-shot has vector 0x43, periodic bit clear, mask set");
    CHECK(lapic_timer_hw_unmask_oneshot(0x43U),
          "one-shot unmask succeeds");
    CHECK(mock_mmio[0x320U / 4U] == 0x43U,
          "unmasked one-shot has only vector 0x43");

    /* 2.2. Exact periodic runtime helpers after successful init. */
    CHECK(lapic_timer_hw_program_masked_periodic(
              0x42U, 0x3U, 625000U),
          "masked periodic program succeeds after LAPIC init");
    CHECK(mock_mmio[0x320U / 4U] == 0x00030042U,
          "masked periodic LVT uses vector 0x42, periodic bit, mask bit");
    CHECK(mock_mmio[0x3E0U / 4U] == 0x3U,
          "masked periodic divide register is 0x3");
    CHECK(mock_mmio[0x380U / 4U] == 625000U,
          "masked periodic initial count is preserved");

    uint32_t read_lvt = 0U;
    uint32_t read_divide = 0U;
    uint32_t read_initial = 0U;
    CHECK(lapic_timer_hw_readback(
              &read_lvt, &read_divide, &read_initial),
          "runtime readback succeeds after LAPIC init");
    CHECK(read_lvt == 0x00030042U,
          "readback returns exact masked periodic LVT");
    CHECK(read_divide == 0x3U,
          "readback returns exact divide configuration");
    CHECK(read_initial == 625000U,
          "readback returns exact initial count");

    CHECK(lapic_timer_hw_unmask_periodic(0x42U),
          "periodic unmask succeeds after LAPIC init");
    CHECK((mock_mmio[0x320U / 4U] & 0xFFU) == 0x42U,
          "unmasked LVT retains vector 0x42");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 17U)) != 0U,
          "unmasked LVT retains periodic bit");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 16U)) == 0U,
          "unmasked LVT clears mask bit");

    CHECK(lapic_timer_hw_mask_periodic(0x42U),
          "periodic remask succeeds after LAPIC init");
    CHECK((mock_mmio[0x320U / 4U] & 0xFFU) == 0x42U,
          "remasked LVT retains vector 0x42");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 17U)) != 0U,
          "remasked LVT retains periodic bit");
    CHECK((mock_mmio[0x320U / 4U] & (1U << 16U)) != 0U,
          "remasked LVT sets mask bit");

    CHECK(!lapic_timer_hw_readback(NULL, &read_divide, &read_initial),
          "runtime readback rejects NULL LVT output");
    CHECK(!lapic_timer_hw_readback(&read_lvt, NULL, &read_initial),
          "runtime readback rejects NULL divide output");
    CHECK(!lapic_timer_hw_readback(&read_lvt, &read_divide, NULL),
          "runtime readback rejects NULL initial-count output");

    begin_case();
    CHECK(!lapic_timer_hw_arm(0x42U, 0x3U, 0xFFFFFFFFU),
          "timer arm rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_program_masked_periodic(
              0x42U, 0x3U, 625000U),
          "masked periodic program rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_readback(&read_lvt, &read_divide, &read_initial),
          "runtime readback rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_unmask_periodic(0x42U),
          "periodic unmask rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_mask_periodic(0x42U),
          "periodic remask rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_program_masked_oneshot(0x43U, 0x3U, 62500U),
          "masked one-shot rejects unpublished LAPIC");
    CHECK(!lapic_timer_hw_unmask_oneshot(0x43U),
          "one-shot unmask rejects unpublished LAPIC");
    CHECK(!mmio_write_called, "one-shot helpers reject disabled LAPIC without MMIO");

    /* 3. Task 2 delivery-mode mismatch returns LAPIC_LINT0_FAILED. */
    begin_case();
    struct acpi_madt_info madt_lint0_delivery = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x00000000U;
    mock_mmio[0x350U / 4U] = 0x00010000U;
    lint0_readback_corrupt_delivery = true;
    CHECK(lapic_init(&madt_lint0_delivery, NULL, 0) == LAPIC_LINT0_FAILED,
          "LINT0 delivery mismatch returns LAPIC_LINT0_FAILED");
    CHECK(mock_mmio[0x350U / 4U] == 0x00010000U,
          "LINT0 restored after delivery mismatch");
    CHECK((mock_msr & (1ULL << 11U)) == 0ULL,
          "APIC disable verified after delivery mismatch");
    CHECK(!svr_was_written, "SVR not programmed on delivery mismatch");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false on delivery mismatch");
    CHECK(serial_contains("[LAPIC] LINT0 failed: status=11; "
                          "legacy PIC retained"),
          "status 11 marker present");
    mmio_write_called = false;
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op after delivery mismatch");
    CHECK(!lint1_read_called, "LINT1 not read on delivery mismatch");
    CHECK(!lint1_write_called, "LINT1 not written on delivery mismatch");

    /* 4. Task 2 mask-stuck mismatch returns LAPIC_LINT0_FAILED. */
    begin_case();
    struct acpi_madt_info madt_lint0_mask = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x00000000U;
    mock_mmio[0x350U / 4U] = 0x00010000U;
    lint0_readback_keep_mask = true;
    CHECK(lapic_init(&madt_lint0_mask, NULL, 0) == LAPIC_LINT0_FAILED,
          "LINT0 mask-stuck mismatch returns LAPIC_LINT0_FAILED");
    CHECK(mock_mmio[0x350U / 4U] == 0x00010000U,
          "LINT0 restored after mask-stuck mismatch");
    CHECK((mock_msr & (1ULL << 11U)) == 0ULL,
          "APIC disable verified after mask-stuck mismatch");
    CHECK(!svr_was_written, "SVR not programmed on mask-stuck mismatch");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false on mask-stuck mismatch");
    CHECK(serial_contains("[LAPIC] LINT0 failed: status=11; "
                          "legacy PIC retained"),
          "mask-stuck status 11 marker present");
    mmio_write_called = false;
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op after mask-stuck mismatch");
    CHECK(!lint1_read_called, "LINT1 not read on mask-stuck mismatch");
    CHECK(!lint1_write_called, "LINT1 not written on mask-stuck mismatch");

    /* 5. Task 2 disable verification failure returns LAPIC_DISABLE_FAILED. */
    begin_case();
    struct acpi_madt_info madt_lint0_disable = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x00000000U;
    mock_mmio[0x350U / 4U] = 0x00010000U;
    lint0_readback_corrupt_delivery = true;
    apic_disable_sticks = false;
    CHECK(lapic_init(&madt_lint0_disable, NULL, 0) == LAPIC_DISABLE_FAILED,
          "APIC disable verification failure returns LAPIC_DISABLE_FAILED");
    CHECK((mock_msr & (1ULL << 11U)) != 0ULL,
          "APIC enable bit remains set after disable verification failure");
    CHECK(!svr_was_written, "SVR not programmed on disable failure");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false on disable failure");
    CHECK(serial_contains("[LAPIC] LINT0 failed: status=12; Local APIC "
                          "disable verification failed; legacy PIC "
                          "availability unverified"),
          "status 12 marker present");
    CHECK(!serial_contains("status=12; legacy PIC retained"),
          "status 12 does not claim legacy PIC retained");
    mmio_write_called = false;
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op after disable failure");
    CHECK(!lint1_read_called, "LINT1 not read on disable failure");
    CHECK(!lint1_write_called, "LINT1 not written on disable failure");

    /* 5b. Original MSR with bit 11 set still fails closed as status 11. */
    begin_case();
    struct acpi_madt_info madt_lint0_already_enabled =
        make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL | (1ULL << 11U);
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x00000000U;
    mock_mmio[0x350U / 4U] = 0x00010000U;
    lint0_readback_corrupt_delivery = true;
    CHECK(lapic_init(&madt_lint0_already_enabled, NULL, 0)
              == LAPIC_LINT0_FAILED,
          "original MSR with bit 11 returns LAPIC_LINT0_FAILED");
    CHECK((mock_msr & (1ULL << 11U)) == 0ULL,
          "original MSR bit 11 is cleared by fail-closed disable");
    CHECK(!svr_was_written, "SVR not programmed with original bit 11 set");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false with original bit 11");
    mmio_write_called = false;
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op with original bit 11");
    CHECK(!lint1_read_called, "LINT1 not read with original bit 11");
    CHECK(!lint1_write_called, "LINT1 not written with original bit 11");

    /* 6. Successful VMM path, APIC ID 0, SVR, EOI, idempotence. */
    begin_case();
    struct acpi_madt_info madt = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U; /* version 5, max LVT 4 */
    mock_mmio[0x020U / 4U] = 0x00000000U; /* id 0 */
    seam_status = VMM_OK;
    seam_out_virtual = (volatile void *)0xFFFF8000FEE00000ULL;

    CHECK(lapic_init(&madt, NULL, 0xFFFF800000000000ULL) == LAPIC_OK,
          "successful VMM path returns LAPIC_OK");
    CHECK(lapic_is_enabled(), "lapic_is_enabled true after success");
    CHECK(lapic_bsp_apic_id() == 0U,
          "lapic_bsp_apic_id returns saved APIC ID 0");
    CHECK(msr_written, "IA32_APIC_BASE was written");
    CHECK((last_wrmsr_value & (1ULL << 11U)) != 0ULL,
          "MSR write sets APIC enable bit");
    CHECK(svr_was_written, "SVR was programmed");
    CHECK(svr_written_value == 0x1EFU, "SVR value vector=0xEF and bit 8");

    /* Idempotence: second lapic_init returns LAPIC_OK without re-init. */
    mmio_write_called = false;
    CHECK(lapic_init(&madt, NULL, 0) == LAPIC_OK,
          "idempotent lapic_init returns LAPIC_OK");
    CHECK(!mmio_write_called, "idempotent lapic_init does not reprogram SVR");

    /* EOI writes 0 to register 0x0B0 when enabled. */
    mmio_write_called = false;
    lapic_eoi();
    CHECK(mmio_write_called, "lapic_eoi writes MMIO when enabled");
    CHECK(mock_mmio[0x0B0U / 4U] == 0U, "lapic_eoi writes 0 to EOI register");

    /* 2b. BSP APIC ID is saved during successful init without MMIO access. */
    begin_case();
    struct acpi_madt_info madt_id = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    mock_mmio[0x020U / 4U] = 0x05000000U;
    CHECK(lapic_init(&madt_id, NULL, 0) == LAPIC_OK,
          "successful init with APIC ID 5 returns LAPIC_OK");
    CHECK(lapic_bsp_apic_id() == 5U,
          "lapic_bsp_apic_id returns saved APIC ID 5");
    mmio_read_called = false;
    (void)lapic_bsp_apic_id();
    CHECK(!mmio_read_called, "lapic_bsp_apic_id does not perform MMIO");

    begin_case();
    CHECK(lapic_bsp_apic_id() == 0U,
          "lapic_bsp_apic_id returns 0 before successful init");

    /* 3. VMM map failure: no MSR write, no MMIO, no SVR. */
    {
        const enum vmm_status failures_vmm[] = {
            VMM_BAD_ALIGNMENT,
            VMM_PMM_ALLOC_FAILED,
            VMM_LARGE_PAGE_CONFLICT,
            VMM_PTE_CONFLICT,
        };
        for (size_t i = 0; i < sizeof(failures_vmm) / sizeof(failures_vmm[0]); ++i) {
            begin_case();
            struct acpi_madt_info madt_vmm = make_madt(0xFEE00000ULL);
            mock_msr = 0xFEE00000ULL;
            seam_status = failures_vmm[i];
            seam_out_virtual = NULL;

            CHECK(lapic_init(&madt_vmm, NULL, 0xFFFF800000000000ULL)
                      == LAPIC_MMIO_MAP_FAILED,
                  "VMM failure returns LAPIC_MMIO_MAP_FAILED");
            CHECK(!lapic_is_enabled(), "lapic_is_enabled false after VMM failure");
            CHECK(!msr_written, "no MSR write on VMM failure");
            CHECK(!mmio_read_called, "no MMIO read on VMM failure");
            CHECK(!mmio_write_called, "no MMIO write on VMM failure");
            CHECK(!svr_was_written, "SVR not programmed on VMM failure");
            lapic_eoi();
            CHECK(!mmio_write_called, "lapic_eoi no-op after VMM failure");
        }
    }

    /* 4. LAPIC_NO_CPUID_APIC. */
    begin_case();
    struct acpi_madt_info madt_cpuid = make_madt(0xFEE00000ULL);
    cpuid_apic_flag = false;
    CHECK(lapic_init(&madt_cpuid, NULL, 0) == LAPIC_NO_CPUID_APIC,
          "CPUID off returns LAPIC_NO_CPUID_APIC");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false with CPUID off");

    /* 5. LAPIC_NO_ACPI_SNAPSHOT. */
    begin_case();
    CHECK(lapic_init(NULL, NULL, 0) == LAPIC_NO_ACPI_SNAPSHOT,
          "NULL madt returns LAPIC_NO_ACPI_SNAPSHOT");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false with NULL madt");

    /* 6. LAPIC_BAD_LAPIC_ADDRESS. */
    begin_case();
    struct acpi_madt_info madt_zero = make_madt(0ULL);
    CHECK(lapic_init(&madt_zero, NULL, 0) == LAPIC_BAD_LAPIC_ADDRESS,
          "zero LAPIC address returns LAPIC_BAD_LAPIC_ADDRESS");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false with zero address");

    begin_case();
    struct acpi_madt_info madt_unaligned = make_madt(0xFEE00123ULL);
    CHECK(lapic_init(&madt_unaligned, NULL, 0) == LAPIC_BAD_LAPIC_ADDRESS,
          "unaligned LAPIC address returns LAPIC_BAD_LAPIC_ADDRESS");

    /* 7. LAPIC_X2APIC_DETECTED. */
    begin_case();
    struct acpi_madt_info madt_x2apic = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL | (1ULL << 10U);
    CHECK(lapic_init(&madt_x2apic, NULL, 0) == LAPIC_X2APIC_DETECTED,
          "x2APIC flag returns LAPIC_X2APIC_DETECTED");

    /* 8. LAPIC_MSR_BASE_MISMATCH. */
    begin_case();
    struct acpi_madt_info madt_mismatch = make_madt(0xFEE01000ULL);
    mock_msr = 0xFEE00000ULL;
    CHECK(lapic_init(&madt_mismatch, NULL, 0) == LAPIC_MSR_BASE_MISMATCH,
          "MSR/MADT mismatch returns LAPIC_MSR_BASE_MISMATCH");

    /* 9. LAPIC_HHDM_OVERFLOW. */
    begin_case();
    struct acpi_madt_info madt_overflow = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    const uint64_t overflow_hhdm = UINT64_MAX - 0xFEE00000ULL + 1ULL;
    CHECK(lapic_init(&madt_overflow, NULL, overflow_hhdm) == LAPIC_HHDM_OVERFLOW,
          "HHDM overflow returns LAPIC_HHDM_OVERFLOW");
    CHECK(!msr_written, "no MSR write on HHDM overflow");

    /* 10. LAPIC_ENABLE_FAILED. */
    begin_case();
    struct acpi_madt_info madt_enable = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0x00050014U;
    apic_enable_sticks = false;
    CHECK(lapic_init(&madt_enable, NULL, 0) == LAPIC_ENABLE_FAILED,
          "enable read-back failure returns LAPIC_ENABLE_FAILED");
    CHECK(msr_written, "MSR write was attempted");
    CHECK((last_wrmsr_value & (1ULL << 11U)) != 0ULL,
          "MSR write includes APIC enable bit");
    CHECK((mock_msr & (1ULL << 11U)) == 0ULL,
          "read-back clears APIC enable bit");
    CHECK(!mmio_read_called, "no MMIO read on enable failure");
    CHECK(!mmio_write_called, "no MMIO write on enable failure");
    CHECK(!svr_was_written, "SVR not programmed on enable failure");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false on enable failure");
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op after enable failure");

    /* 11. LAPIC_BAD_VERSION with rollback. */
    begin_case();
    struct acpi_madt_info madt_version = make_madt(0xFEE00000ULL);
    mock_msr = 0xFEE00000ULL;
    mock_mmio[0x030U / 4U] = 0U; /* version 0 */
    CHECK(lapic_init(&madt_version, NULL, 0) == LAPIC_BAD_VERSION,
          "zero APIC version returns LAPIC_BAD_VERSION");
    CHECK(mock_msr == 0xFEE00000ULL, "IA32_APIC_BASE rolled back to original");
    CHECK(!lapic_is_enabled(), "lapic_is_enabled false after bad version");

    /* 12. lapic_eoi is no-op when disabled. */
    begin_case();
    lapic_eoi();
    CHECK(!mmio_write_called, "lapic_eoi no-op when disabled");

    /* 13. Task 2 x2APIC transition and rollback. */
    begin_case();
    CHECK(strcmp(lapic_x2apic_status_string(LAPIC_X2APIC_OK), "OK") == 0,
          "x2APIC status_string OK");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_UNSUPPORTED), "unsupported") == 0,
          "x2APIC status_string unsupported");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_NOT_READY), "not ready") == 0,
          "x2APIC status_string not ready");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_ALREADY_ACTIVE), "already active") == 0,
          "x2APIC status_string already active");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_ENABLE_READBACK_FAILED),
              "enable readback failed") == 0,
          "x2APIC status_string enable readback failed");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_ROLLBACK_FAILED), "rollback failed") == 0,
          "x2APIC status_string rollback failed");
    CHECK(strcmp(lapic_x2apic_status_string(
              LAPIC_X2APIC_STATE_READBACK_FAILED),
              "state readback failed") == 0,
          "x2APIC status_string state readback failed");

    begin_case();
    cpuid_x2apic_flag = true;
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_NOT_READY,
          "transition before LAPIC init returns NOT_READY");
    CHECK(!lapic_x2apic_active(),
          "transition before LAPIC init does not publish x2APIC");

    prepare_x2apic_host_case(5U);
    const uint32_t saved_svr = mock_mmio[0x0F0U / 4U];
    const uint32_t saved_lint0 = mock_mmio[0x350U / 4U];
    const uint32_t saved_lvt = mock_mmio[0x320U / 4U];
    const uint32_t saved_divide = mock_mmio[0x3E0U / 4U];
    const uint32_t saved_initial = mock_mmio[0x380U / 4U];
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_OK,
          "x2APIC transition success returns OK");
    CHECK(lapic_x2apic_active(),
          "x2APIC transition success publishes x2APIC");
    CHECK((mock_msr & (1ULL << 10U)) != 0ULL,
          "transition readback retains EXTD");
    CHECK((mock_msr & (1ULL << 11U)) != 0ULL,
          "transition readback retains EN");
    CHECK(mock_x2apic_msr[0x0F0U >> 4U] == saved_svr,
          "transition restores SVR through x2APIC MSR");
    CHECK(mock_x2apic_msr[0x350U >> 4U] == saved_lint0,
          "transition restores LINT0 through x2APIC MSR");
    CHECK(mock_x2apic_msr[0x320U >> 4U] == saved_lvt,
          "transition restores LVT timer through x2APIC MSR");
    CHECK(mock_x2apic_msr[0x3E0U >> 4U] == saved_divide,
          "transition restores divide through x2APIC MSR");
    CHECK(mock_x2apic_msr[0x380U >> 4U] == saved_initial,
          "transition restores initial count through x2APIC MSR");
    CHECK(serial_contains("[X2APIC] enabled: id="),
          "x2APIC enabled diagnostic present");
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_ALREADY_ACTIVE,
          "second transition returns ALREADY_ACTIVE");
    mmio_write_called = false;
    last_wrmsr_msr = 0;
    lapic_eoi();
    CHECK(last_wrmsr_msr == 0x80BU,
          "x2APIC lapic_eoi writes EOI MSR 0x80B");
    CHECK(last_wrmsr_value == 0U,
          "x2APIC lapic_eoi writes zero to EOI MSR");
    CHECK(!mmio_write_called,
          "x2APIC lapic_eoi does not write MMIO");

    prepare_x2apic_host_case(0U);
    cpuid_x2apic_flag = false;
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_UNSUPPORTED,
          "transition without CPUID x2APIC returns UNSUPPORTED");
    CHECK(!lapic_x2apic_active(),
          "unsupported transition does not publish x2APIC");

    prepare_x2apic_host_case(0U);
    const uint32_t failure_svr = mock_mmio[0x0F0U / 4U];
    const uint32_t failure_lint0 = mock_mmio[0x350U / 4U];
    const uint32_t failure_lvt = mock_mmio[0x320U / 4U];
    const uint32_t failure_divide = mock_mmio[0x3E0U / 4U];
    const uint32_t failure_initial = mock_mmio[0x380U / 4U];
    x2apic_enable_sticks = false;
    CHECK(lapic_x2apic_transition()
              == LAPIC_X2APIC_ENABLE_READBACK_FAILED,
          "enable readback mismatch returns ENABLE_READBACK_FAILED");
    CHECK(!lapic_x2apic_active(),
          "enable readback mismatch does not publish x2APIC");
    CHECK((mock_msr & (1ULL << 10U)) == 0ULL,
          "enable mismatch rollback clears EXTD");
    CHECK((mock_msr & (1ULL << 11U)) != 0ULL,
          "enable mismatch rollback re-enables xAPIC");
    CHECK(mock_mmio[0x0F0U / 4U] == failure_svr,
          "enable mismatch rollback restores SVR");
    CHECK(mock_mmio[0x350U / 4U] == failure_lint0,
          "enable mismatch rollback restores LINT0");
    CHECK(mock_mmio[0x320U / 4U] == failure_lvt,
          "enable mismatch rollback restores LVT timer");
    CHECK(mock_mmio[0x3E0U / 4U] == failure_divide,
          "enable mismatch rollback restores divide");
    CHECK(mock_mmio[0x380U / 4U] == failure_initial,
          "enable mismatch rollback restores initial count");
    mmio_write_called = false;
    last_wrmsr_msr = 0;
    lapic_eoi();
    CHECK(mmio_write_called,
          "failed transition leaves lapic_eoi on MMIO");
    CHECK(last_wrmsr_msr == 0U,
          "failed transition does not use EOI MSR");

    prepare_x2apic_host_case(0U);
    const uint32_t restore_lvt = mock_mmio[0x320U / 4U];
    x2apic_corrupt_msr = 0x320U >> 4U;
    x2apic_corrupt_value = restore_lvt ^ 1U;
    CHECK(lapic_x2apic_transition()
              == LAPIC_X2APIC_STATE_READBACK_FAILED,
          "restore readback mismatch returns STATE_READBACK_FAILED");
    CHECK(!lapic_x2apic_active(),
          "restore readback mismatch does not publish x2APIC");
    CHECK(mock_mmio[0x320U / 4U] == restore_lvt,
          "restore readback mismatch rollback restores LVT timer");

    prepare_x2apic_host_case(0U);
    x2apic_enable_sticks = false;
    x2apic_rollback_sticks = false;
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_ROLLBACK_FAILED,
          "rollback readback mismatch returns ROLLBACK_FAILED");
    CHECK(!lapic_x2apic_active(),
          "rollback readback mismatch does not publish x2APIC");

    prepare_x2apic_host_case(0U);
    const uint32_t rollback_svr = mock_mmio[0x0F0U / 4U];
    const uint32_t rollback_lint0 = mock_mmio[0x350U / 4U];
    const uint32_t rollback_lvt = mock_mmio[0x320U / 4U];
    const uint32_t rollback_divide = mock_mmio[0x3E0U / 4U];
    const uint32_t rollback_initial = mock_mmio[0x380U / 4U];
    CHECK(lapic_x2apic_transition() == LAPIC_X2APIC_OK,
          "explicit rollback fixture transitions to x2APIC");
    CHECK(lapic_x2apic_active(),
          "explicit rollback fixture is active before rollback");
    CHECK(lapic_x2apic_rollback() == LAPIC_X2APIC_OK,
          "explicit rollback returns OK");
    CHECK(!lapic_x2apic_active(),
          "explicit rollback publishes xAPIC");
    CHECK((mock_msr & (1ULL << 10U)) == 0ULL,
          "explicit rollback clears EXTD");
    CHECK((mock_msr & (1ULL << 11U)) != 0ULL,
          "explicit rollback keeps EN");
    CHECK(mock_mmio[0x0F0U / 4U] == rollback_svr,
          "explicit rollback restores SVR through MMIO");
    CHECK(mock_mmio[0x350U / 4U] == rollback_lint0,
          "explicit rollback restores LINT0 through MMIO");
    CHECK(mock_mmio[0x320U / 4U] == rollback_lvt,
          "explicit rollback restores LVT timer through MMIO");
    CHECK(mock_mmio[0x3E0U / 4U] == rollback_divide,
          "explicit rollback restores divide through MMIO");
    CHECK(mock_mmio[0x380U / 4U] == rollback_initial,
          "explicit rollback restores initial count through MMIO");
    mmio_write_called = false;
    last_wrmsr_msr = 0;
    lapic_eoi();
    CHECK(mmio_write_called,
          "xAPIC lapic_eoi writes MMIO after explicit rollback");
    CHECK(last_wrmsr_msr == 0U,
          "explicit rollback leaves lapic_eoi on MMIO");

#endif
    if (failures) {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nAll LAPIC host tests passed\n");
    return 0;
}
