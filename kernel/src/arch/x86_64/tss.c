#include "arch/x86_64/tss.h"
#include "cpu.h"
#include "serial.h"
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(struct tss64) == 104U, "tss64 struct size must be 104 bytes");
_Static_assert(offsetof(struct tss64, io_map_base) == 102U, "io_map_base must be at offset 102");

void tss_init_for(struct cpu *cpu) {
    const uint64_t stack_top =
        (uint64_t)(uintptr_t)(cpu->ist1_stack + sizeof(cpu->ist1_stack));
    const uint64_t ist2_top =
        (uint64_t)(uintptr_t)(cpu->ist2_stack + sizeof(cpu->ist2_stack));

    cpu->tss.reserved0 = 0U;
    cpu->tss.rsp0 = 0U;
    cpu->tss.rsp1 = 0U;
    cpu->tss.rsp2 = 0U;
    cpu->tss.reserved1 = 0U;
    cpu->tss.ist1 = stack_top;
    cpu->tss.ist2 = ist2_top;
    cpu->tss.ist3 = 0U;
    cpu->tss.ist4 = 0U;
    cpu->tss.ist5 = 0U;
    cpu->tss.ist6 = 0U;
    cpu->tss.ist7 = 0U;
    cpu->tss.reserved2 = 0U;
    cpu->tss.reserved3 = 0U;
    /* I/O map base >= TSS limit disables the I/O permission bitmap.
     * sizeof(tss64) == 104, so io_map_base = 104. */
    cpu->tss.io_map_base = (uint16_t)sizeof(struct tss64);

    serial_write("[TSS ] IST1 stack: ");
    serial_write_hex((uint64_t)(uintptr_t)cpu->ist1_stack);
    serial_write(" .. ");
    serial_write_hex(stack_top);
    serial_write("\n");

    serial_write("[TSS ] IST2 stack: ");
    serial_write_hex((uint64_t)(uintptr_t)cpu->ist2_stack);
    serial_write(" .. ");
    serial_write_hex(ist2_top);
    serial_write("\n");
}

void tss_init(void) {
    tss_init_for(cpu_current());
}

void tss_set_rsp0(uint64_t rsp0) {
    cpu_current()->tss.rsp0 = rsp0;
}

uint64_t tss_rsp0(void) {
    return cpu_current()->tss.rsp0;
}

const void *tss_ist1_stack(void) {
    return cpu_current()->ist1_stack;
}

size_t tss_ist1_stack_size(void) {
    return sizeof(cpu_current()->ist1_stack);
}

const void *tss_ist2_stack(void) {
    return cpu_current()->ist2_stack;
}

size_t tss_ist2_stack_size(void) {
    return sizeof(cpu_current()->ist2_stack);
}
