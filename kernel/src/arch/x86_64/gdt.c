#include "arch/x86_64/gdt.h"
#include "arch/x86_64/tss.h"
#include "cpu.h"
#include "panic.h"
#include "serial.h"
#include <stdint.h>

/*
 * Keep each CPU GDT writable. x86 may set the Accessed bit while loading a
 * segment selector. A const GDT is placed in .rodata and therefore causes a
 * supervisor write-protection page fault during mov %ax, %ds.
 *
 * The Accessed bit is also pre-set in every usable descriptor so the CPU does
 * not need to update the table during the initial segment reload.
 *
 * GDT layout:
 *   [0] NULL          (selector 0x00)
 *   [1] kernel code   (selector 0x08)
 *   [2] kernel data   (selector 0x10)
 *   [3] user data     (selector 0x18)
 *   [4] user code     (selector 0x20)
 *   [5..6] 64-bit TSS (selector 0x28, two descriptor slots)
 */

extern void gdt_load_table(const struct gdt_pointer *pointer);
extern void gdt_reload_segments_asm(void);

/* Encode a 64-bit TSS descriptor into two consecutive GDT slots.
 * start points to the first of the two slots. */
static void gdt_encode_tss(uint64_t *start, uint64_t tss_base, uint32_t limit) {
    const uint64_t base = tss_base;
    const uint64_t low_limit = limit & 0xffffULL;
    const uint64_t type = 0x9ULL;  /* available 64-bit TSS */
    const uint64_t access = type | (1ULL << 7U);  /* P=1, DPL=0, S=0, Type=9 */
    const uint64_t hi_limit = (limit >> 16ULL) & 0xfULL;
    const uint64_t hi_base = (base >> 24ULL) & 0xffULL;

    start[0] = low_limit
               | ((base & 0xffffffULL) << 16ULL)
               | (access << 40ULL)
               | (hi_limit << 48ULL)
               | (hi_base << 56ULL);
    start[1] = base >> 32ULL;
}

static void gdt_init_common(uint64_t gdt[7]) {
    gdt[0] = 0x0000000000000000ULL;
    gdt[1] = 0x00af9b000000ffffULL;
    gdt[2] = 0x00cf93000000ffffULL;
    gdt[3] = 0x00cff3000000ffffULL;
    gdt[4] = 0x00affb000000ffffULL;
    gdt[5] = 0x0000000000000000ULL;
    gdt[6] = 0x0000000000000000ULL;
}

void gdt_init_for(struct cpu *cpu) {
    gdt_init_common(cpu->gdt);
    /* Encode the TSS descriptor into slots 5 and 6. */
    gdt_encode_tss(&cpu->gdt[5], (uint64_t)(uintptr_t)&cpu->tss,
                   (uint32_t)(sizeof(struct tss64) - 1U));
    cpu->gdt_descriptor.limit = (uint16_t)(sizeof(cpu->gdt) - 1U);
    cpu->gdt_descriptor.base = (uint64_t)(uintptr_t)&cpu->gdt[0];
    gdt_load_for(cpu);
}

void gdt_load_for(struct cpu *cpu) {
    gdt_load_table(&cpu->gdt_descriptor);
}

void gdt_init(void) {
    gdt_init_for(cpu_current());
}

void gdt_reload_segments(void) {
    gdt_reload_segments_asm();
}

void gdt_load_tr_for(struct cpu *cpu) {
    (void)cpu;
    __asm__ volatile (
        "ltr %%ax\n"
        :
        : "a"(GDT_TSS_SELECTOR)
        : "memory"
    );

    const uint16_t actual = gdt_read_tr();
    serial_write("[TSS ] TR selector: ");
    serial_write_hex(actual);
    serial_write("\n");
    if (actual != GDT_TSS_SELECTOR) {
        kernel_panic("TR selector mismatch after ltr");
    }
}

void gdt_load_tr(void) {
    gdt_load_tr_for(cpu_current());
}

uint16_t gdt_read_tr(void) {
    uint16_t selector;
    __asm__ volatile ("str %0" : "=r"(selector));
    return selector;
}
