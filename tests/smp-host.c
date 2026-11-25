#include "smp.h"
#include "acpi.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/tss.h"
#include "cpu.h"
#include "lapic.h"
#include "pmm.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SMP_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct limine_mp_info mp_infos[HFOC_MAX_CPUS + 1U];
static struct limine_mp_info *mp_info_ptrs[HFOC_MAX_CPUS + 1U];
static struct limine_mp_response mp_response;
static struct acpi_madt_info madt;

static bool pmm_fail;
static uint64_t pmm_page;
static bool auto_start;
static bool ap_lapic_ok;
static uint32_t mock_bsp_apic_id;
static unsigned prepared_count;
static unsigned ap_entry_count;
static unsigned ap_idle_calls;

uint64_t pmm_alloc_page(void) {
    if (pmm_fail) {
        return 0U;
    }
    const uint64_t page = pmm_page;
    pmm_page += 4096U;
    return page;
}

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (pmm_fail || count == 0U) {
        return 0U;
    }
    const uint64_t page = pmm_page;
    pmm_page += (uint64_t)count * 4096U;
    return page;
}

uint8_t lapic_bsp_apic_id(void) {
    return (uint8_t)mock_bsp_apic_id;
}

enum lapic_status lapic_init_ap(
    const struct acpi_madt_info *madt_arg,
    uint64_t hhdm_offset,
    uint32_t expected_lapic_id)
{
    (void)madt_arg;
    (void)hhdm_offset;
    (void)expected_lapic_id;
    return ap_lapic_ok ? LAPIC_OK : LAPIC_BAD_VERSION;
}

const char *lapic_status_string(enum lapic_status status) {
    (void)status;
    return "host LAPIC status";
}

void gdt_init_for(struct cpu *cpu) {
    (void)cpu;
}

void tss_init_for(struct cpu *cpu) {
    (void)cpu;
}

void gdt_load_for(struct cpu *cpu) {
    (void)cpu;
}

void gdt_load_tr_for(struct cpu *cpu) {
    (void)cpu;
}

void gdt_reload_segments(void) {
}

void interrupts_init(void) {
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

bool smp_test_auto_start(void) {
    return auto_start;
}

void smp_test_note_ap_prepared(uint32_t index) {
    (void)index;
    ++prepared_count;
}

void smp_test_note_ap_entered(uint32_t index) {
    (void)index;
    ++ap_entry_count;
}

void smp_test_note_ap_idle(void) {
    ++ap_idle_calls;
}

static void reset_cpus(void) {
    for (uint32_t index = 0U; index < HFOC_MAX_CPUS; ++index) {
        struct cpu *cpu = cpu_by_index(index);
        memset(cpu, 0, sizeof(*cpu));
    }
    cpu_gs_base_set(cpu_by_index(0U));
    cpu_init_bsp(cpu_by_index(0U), 0U, mock_bsp_apic_id);
}

static void reset_mocks(void) {
    pmm_fail = false;
    pmm_page = 0x100000U;
    auto_start = false;
    ap_lapic_ok = true;
    mock_bsp_apic_id = 0U;
    prepared_count = 0U;
    ap_entry_count = 0U;
    ap_idle_calls = 0U;
    memset(&madt, 0, sizeof(madt));
    madt.lapic_address = 0xFEE00000ULL;
    memset(mp_infos, 0, sizeof(mp_infos));
    memset(mp_info_ptrs, 0, sizeof(mp_info_ptrs));
    memset(&mp_response, 0, sizeof(mp_response));
    mp_response.cpus = mp_info_ptrs;
}

static void prepare_response(uint32_t cpu_count, uint32_t bsp_lapic_id) {
    mp_response.cpu_count = cpu_count;
    mp_response.bsp_lapic_id = bsp_lapic_id;
    for (uint32_t index = 0U; index < cpu_count; ++index) {
        mp_infos[index].processor_id = index;
        mp_infos[index].lapic_id = index;
        mp_info_ptrs[index] = &mp_infos[index];
    }
}

static void test_smp_bsp_only(void) {
    reset_mocks();
    reset_cpus();
    CHECK(smp_init(NULL, NULL, 0U) == SMP_BSP_ONLY,
          "missing MP response returns SMP_BSP_ONLY");
    CHECK(cpu_by_index(0U)->state == CPU_STATE_BSP,
          "BSP state remains published without MP response");
    CHECK(smp_online(), "smp_online remains true for BSP-only boot");
    CHECK(smp_online_ap_count() == 0U,
          "BSP-only boot publishes zero online APs");
}

static void test_smp_bsp_id_mismatch(void) {
    reset_mocks();
    reset_cpus();
    prepare_response(2U, 1U);
    CHECK(smp_init(&mp_response, &madt, 0xffff800000000000ULL) ==
              SMP_BSP_ID_MISMATCH,
          "BSP LAPIC ID mismatch returns SMP_BSP_ID_MISMATCH");
    CHECK(smp_online_ap_count() == 0U,
          "BSP ID mismatch starts no APs");
}

static void test_smp_stack_allocation_failure(void) {
    reset_mocks();
    reset_cpus();
    prepare_response(2U, 0U);
    pmm_fail = true;
    CHECK(smp_init(&mp_response, &madt, 0xffff800000000000ULL) == SMP_OK,
          "AP stack allocation failure returns SMP_OK");
    CHECK(cpu_by_index(1U)->state == CPU_STATE_FAILED,
          "AP stack allocation failure marks CPU failed");
    CHECK(smp_online_ap_count() == 0U,
          "failed AP is not counted online");
    CHECK(mp_infos[1].goto_address == NULL,
          "failed AP never publishes goto_address");
    CHECK(mp_infos[1].reserved == 0U,
          "failed AP never publishes reserved stack");
}

static void test_smp_ap_online(void) {
    reset_mocks();
    reset_cpus();
    prepare_response(2U, 0U);
    auto_start = true;
    CHECK(smp_init(&mp_response, &madt, 0xffff800000000000ULL) == SMP_OK,
          "successful AP startup returns SMP_OK");
    CHECK(cpu_by_index(1U)->state == CPU_STATE_ONLINE,
          "AP reaches CPU_STATE_ONLINE");
    CHECK(smp_online_ap_count() == 1U,
          "online AP count is one after successful startup");
    CHECK(mp_infos[1].goto_address == smp_ap_entry,
          "AP goto_address points at smp_ap_entry");
    CHECK(mp_infos[1].extra_argument == 1U,
          "AP extra_argument stores CPU index");
    CHECK(mp_infos[1].reserved ==
              0xffff800000000000ULL + 0x100000ULL +
              (HFOC_AP_STACK_PAGES * 4096ULL),
          "AP reserved stores HHDM stack top");
    CHECK(prepared_count == 1U,
          "AP goto/reserved preparation happens exactly once");
    CHECK(ap_entry_count == 1U,
          "successful AP enters smp_ap_entry exactly once");
    CHECK(ap_idle_calls == 1U,
          "successful AP reaches smp_ap_idle exactly once");
}

static void test_smp_online_timeout(void) {
    reset_mocks();
    reset_cpus();
    prepare_response(2U, 0U);
    auto_start = false;
    CHECK(smp_init(&mp_response, &madt, 0xffff800000000000ULL) == SMP_OK,
          "online wait timeout still returns SMP_OK");
    CHECK(cpu_by_index(1U)->state == CPU_STATE_FAILED,
          "online wait timeout marks AP failed");
    CHECK(smp_online_ap_count() == 0U,
          "timed-out AP is not counted online");
    CHECK(prepared_count == 1U,
          "timed-out AP still prepares goto/reserved exactly once");
    CHECK(ap_entry_count == 0U,
          "timed-out AP never enters smp_ap_entry");
}

static void test_smp_capacity_overflow(void) {
    reset_mocks();
    reset_cpus();
    prepare_response(HFOC_MAX_CPUS + 1U, 0U);
    CHECK(smp_init(&mp_response, &madt, 0U) == SMP_OUT_OF_CAPACITY,
          "CPU capacity overflow returns SMP_OUT_OF_CAPACITY");
}

static void test_smp_ap_idle(void) {
    reset_mocks();
    reset_cpus();
    smp_ap_idle();
    CHECK(ap_idle_calls == 1U,
          "AP idle path is callable without unmasking LVT entries");
}

int main(void) {
    test_smp_bsp_only();
    test_smp_bsp_id_mismatch();
    test_smp_stack_allocation_failure();
    test_smp_ap_online();
    test_smp_online_timeout();
    test_smp_capacity_overflow();
    test_smp_ap_idle();

    if (failures != 0) {
        fprintf(stderr, "SMP host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("SMP host tests: ALL OK");
    return 0;
}

#endif
