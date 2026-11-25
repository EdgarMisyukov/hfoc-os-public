#include "cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/tss.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CPU_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct gdt_pointer last_gdt_pointer;

void gdt_load_table(const struct gdt_pointer *pointer) {
    if (pointer != NULL) {
        last_gdt_pointer = *pointer;
    }
}

void gdt_reload_segments_asm(void) {
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

_Noreturn void kernel_panic(const char *message) {
    (void)message;
    for (;;) {
    }
}

static uint64_t decode_tss_base(const uint64_t slots[2]) {
    const uint64_t low = (slots[0] >> 16U) & 0xffffffULL;
    const uint64_t mid = (slots[0] >> 56U) & 0xffULL;
    return low | (mid << 24U) | (slots[1] << 32U);
}

static uint32_t decode_tss_limit(const uint64_t slots[2]) {
    const uint32_t low = (uint32_t)(slots[0] & 0xffffU);
    const uint32_t high = (uint32_t)((slots[0] >> 48U) & 0xfU);
    return low | (high << 16U);
}

static bool ranges_overlap(
    uintptr_t a_base, size_t a_size,
    uintptr_t b_base, size_t b_size)
{
    return a_base < b_base + b_size && b_base < a_base + a_size;
}

static void test_cpu_init(void) {
    cpu_host_test_reset();
    struct cpu *bsp = cpu_by_index(0U);
    struct cpu *ap = cpu_by_index(1U);

    cpu_init_bsp(bsp, 7U, 5U);
    CHECK(bsp->index == 0U, "BSP CPU index is 0");
    CHECK(bsp->state == CPU_STATE_BSP, "BSP CPU state is BSP");
    CHECK(bsp->processor_id == 7U, "BSP processor ID is stored");
    CHECK(bsp->lapic_id == 5U, "BSP LAPIC ID is stored");
    CHECK(bsp->self == bsp, "BSP self pointer is initialized");

    cpu_init_ap(ap, 1U, 9U, 6U, 0x12345678U);
    CHECK(ap->index == 1U, "AP CPU index is stored");
    CHECK(ap->state == CPU_STATE_STARTING, "AP CPU state is STARTING");
    CHECK(ap->processor_id == 9U, "AP processor ID is stored");
    CHECK(ap->lapic_id == 6U, "AP LAPIC ID is stored");
    CHECK(ap->kernel_stack_top == 0x12345678U,
          "AP kernel stack top is stored");

    CHECK(cpu_by_index(HFOC_MAX_CPUS) == NULL,
          "out-of-range CPU lookup returns NULL");
}

static void test_per_cpu_gdt_tss(void) {
    cpu_host_test_reset();
    struct cpu *bsp = cpu_by_index(0U);
    struct cpu *ap = cpu_by_index(1U);

    cpu_init_bsp(bsp, 1U, 0U);
    cpu_init_ap(ap, 1U, 2U, 1U, 0x200000U);

    gdt_init_for(bsp);
    tss_init_for(bsp);
    gdt_init_for(ap);
    tss_init_for(ap);

    CHECK(bsp->gdt[0] == 0ULL, "per-CPU GDT has NULL descriptor");
    for (unsigned int i = 1U; i < 5U; ++i) {
        CHECK(bsp->gdt[i] == ap->gdt[i], "kernel/user descriptors are identical");
    }

    const uintptr_t bsp_tss_base = (uintptr_t)&bsp->tss;
    const uintptr_t ap_tss_base = (uintptr_t)&ap->tss;
    CHECK(decode_tss_base(&bsp->gdt[5]) == bsp_tss_base,
          "BSP TSS descriptor encodes BSP TSS base");
    CHECK(decode_tss_base(&ap->gdt[5]) == ap_tss_base,
          "AP TSS descriptor encodes AP TSS base");
    CHECK(decode_tss_limit(&bsp->gdt[5]) ==
              (uint32_t)(sizeof(struct tss64) - 1U),
          "BSP TSS descriptor encodes exact TSS limit");

    const uintptr_t bsp_ist1 = (uintptr_t)bsp->ist1_stack;
    const uintptr_t bsp_ist2 = (uintptr_t)bsp->ist2_stack;
    const uintptr_t ap_ist1 = (uintptr_t)ap->ist1_stack;
    const uintptr_t ap_ist2 = (uintptr_t)ap->ist2_stack;
    const size_t ist_size = sizeof(bsp->ist1_stack);

    CHECK(bsp->tss.ist1 == bsp_ist1 + ist_size,
          "BSP IST1 top is embedded in per-CPU TSS");
    CHECK(bsp->tss.ist2 == bsp_ist2 + ist_size,
          "BSP IST2 top is embedded in per-CPU TSS");
    CHECK(ap->tss.ist1 == ap_ist1 + ist_size,
          "AP IST1 top is embedded in per-CPU TSS");
    CHECK(ap->tss.ist2 == ap_ist2 + ist_size,
          "AP IST2 top is embedded in per-CPU TSS");
    CHECK(!ranges_overlap(bsp_ist1, ist_size, bsp_ist2, ist_size),
          "BSP IST1 and IST2 stacks do not overlap");
    CHECK(!ranges_overlap(bsp_ist1, ist_size, ap_ist1, ist_size),
          "BSP and AP IST1 stacks do not overlap");
    CHECK(!ranges_overlap(bsp_ist2, ist_size, ap_ist2, ist_size),
          "BSP and AP IST2 stacks do not overlap");
    CHECK(!ranges_overlap(bsp_ist1, ist_size, ap_ist2, ist_size),
          "BSP IST1 and AP IST2 stacks do not overlap");
}

static void test_current_cpu_and_gs_base(void) {
    cpu_host_test_reset();
    struct cpu *bsp = cpu_by_index(0U);
    struct cpu *ap = cpu_by_index(1U);

    cpu_gs_base_set(bsp);
    CHECK(cpu_current() == bsp, "cpu_current resolves mocked GS base CPU");
    CHECK(bsp->self == bsp, "cpu_gs_base_set publishes cpu->self");

    cpu_gs_base_set(ap);
    CHECK(cpu_current() == ap,
          "cpu_current follows GS base after CPU switch");
    CHECK(ap->self == ap, "AP self pointer is published by cpu_gs_base_set");
}

static void test_tss_rsp0_api(void) {
    cpu_host_test_reset();
    struct cpu *bsp = cpu_by_index(0U);
    cpu_gs_base_set(bsp);

    CHECK(tss_rsp0() == 0U, "initial TSS rsp0 is zero");
    tss_set_rsp0(0x12345678U);
    CHECK(tss_rsp0() == 0x12345678U,
          "tss_set_rsp0 round-trips exact value");
    tss_set_rsp0(0xffff800000400000U);
    CHECK(tss_rsp0() == 0xffff800000400000U,
          "TSS rsp0 accepts exact kernel stack top");
}

int main(void) {
    test_cpu_init();
    test_per_cpu_gdt_tss();
    test_current_cpu_and_gs_base();
    test_tss_rsp0_api();

    if (failures != 0) {
        fprintf(stderr, "CPU host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("CPU host tests: ALL OK");
    return 0;
}

#endif
