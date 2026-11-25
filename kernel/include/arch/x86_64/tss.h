#ifndef HFOC_ARCH_X86_64_TSS_H
#define HFOC_ARCH_X86_64_TSS_H

#include <stdint.h>
#include <stddef.h>

#define IST1_STACK_SIZE 16384U
#define IST2_STACK_SIZE 16384U

struct cpu;

/* AMD64 / Intel 64-bit Task State Segment.
 * IST fields are used by the CPU only when the corresponding IDT entry has
 * a non-zero IST index.  The I/O-map base is set beyond the TSS limit so the
 * CPU treats the I/O permission bitmap as absent. */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

void tss_init(void);
void tss_init_for(struct cpu *cpu);
void tss_set_rsp0(uint64_t rsp0);
uint64_t tss_rsp0(void);

/* Returns the address of the static IST1 stack area (low end). */
const void *tss_ist1_stack(void);

/* Size of the IST1 stack in bytes. */
size_t tss_ist1_stack_size(void);

/* Returns the address of the static IST2 stack area (low end). */
const void *tss_ist2_stack(void);

/* Size of the IST2 stack in bytes. */
size_t tss_ist2_stack_size(void);

#endif
