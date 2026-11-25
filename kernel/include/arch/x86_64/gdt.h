#ifndef HFOC_ARCH_X86_64_GDT_H
#define HFOC_ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT_TSS_SELECTOR 0x28U

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct cpu;

void gdt_init(void);
void gdt_init_for(struct cpu *cpu);
void gdt_load_for(struct cpu *cpu);
void gdt_load_tr_for(struct cpu *cpu);
void gdt_reload_segments(void);
void gdt_load_tr(void);
uint16_t gdt_read_tr(void);

#endif
