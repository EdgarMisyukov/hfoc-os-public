#ifndef HFOC_CPU_H
#define HFOC_CPU_H

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/tss.h"
#include "lapic.h"
#include <stdbool.h>
#include <stdint.h>

#define HFOC_MAX_CPUS     64U
#define IST_STACK_SIZE    16384U
#define HFOC_AP_STACK_PAGES 8U

enum cpu_state {
    CPU_STATE_UNKNOWN = 0,
    CPU_STATE_BSP = 1,
    CPU_STATE_STARTING = 2,
    CPU_STATE_ONLINE = 3,
    CPU_STATE_FAILED = 4,
};

struct cpu {
    uint32_t index;
    uint32_t processor_id;
    uint32_t lapic_id;
    enum cpu_state state;
    struct cpu *self;
    uint64_t gdt[7];
    struct gdt_pointer gdt_descriptor;
    struct tss64 tss;
    uint8_t ist1_stack[IST_STACK_SIZE];
    uint8_t ist2_stack[IST_STACK_SIZE];
    uintptr_t kernel_stack_top;
    struct lapic_cpu_state lapic;
};

void cpu_init_bsp(struct cpu *cpu, uint32_t processor_id,
                  uint32_t lapic_id);
void cpu_init_ap(struct cpu *cpu, uint32_t index,
                 uint32_t processor_id, uint32_t lapic_id,
                 uintptr_t kernel_stack_top);
struct cpu *cpu_current(void);
struct cpu *cpu_by_index(uint32_t index);
void cpu_gs_base_set(struct cpu *cpu);

#ifdef HFOC_CPU_HOST_TEST
void cpu_host_test_reset(void);
void cpu_host_test_set_current(struct cpu *cpu);
#endif

#endif
