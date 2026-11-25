#include "cpu.h"
#include <stddef.h>
#include <stdint.h>
#ifdef HFOC_CPU_HOST_TEST
#include <string.h>
#endif

static struct cpu cpus[HFOC_MAX_CPUS];

#ifdef HFOC_CPU_HOST_TEST
static struct cpu host_cpu;
static struct cpu *host_current_cpu = &host_cpu;

void cpu_host_test_reset(void) {
    memset(&host_cpu, 0, sizeof(host_cpu));
    host_current_cpu = &host_cpu;
}

void cpu_host_test_set_current(struct cpu *cpu) {
    host_current_cpu = (cpu != NULL) ? cpu : &host_cpu;
}

struct cpu *cpu_current(void) {
    return host_current_cpu;
}
#else
static void write_gs_base(uintptr_t base) {
    const uint32_t low = (uint32_t)(base & 0xFFFFFFFFULL);
    const uint32_t high = (uint32_t)(base >> 32U);
    __asm__ volatile (
        "wrmsr\n"
        :
        : "c"(0xC0000101U), "a"(low), "d"(high)
        : "memory"
    );
}

struct cpu *cpu_current(void) {
    struct cpu *cpu;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(cpu) : : "memory");
    return cpu;
}
#endif

void cpu_init_bsp(struct cpu *cpu, uint32_t processor_id,
                  uint32_t lapic_id)
{
    if (cpu == NULL) return;
    cpu->index = 0U;
    cpu->processor_id = processor_id;
    cpu->lapic_id = lapic_id;
    cpu->state = CPU_STATE_BSP;
    cpu->self = cpu;
    cpu->kernel_stack_top = 0U;
}

void cpu_init_ap(struct cpu *cpu, uint32_t index,
                 uint32_t processor_id, uint32_t lapic_id,
                 uintptr_t kernel_stack_top)
{
    if (cpu == NULL) return;
    cpu->index = index;
    cpu->processor_id = processor_id;
    cpu->lapic_id = lapic_id;
    cpu->state = CPU_STATE_STARTING;
    cpu->self = cpu;
    cpu->kernel_stack_top = kernel_stack_top;
}

struct cpu *cpu_by_index(uint32_t index) {
    if (index >= HFOC_MAX_CPUS) return NULL;
    return &cpus[index];
}

void cpu_gs_base_set(struct cpu *cpu) {
    if (cpu == NULL) return;
    cpu->self = cpu;
#ifdef HFOC_CPU_HOST_TEST
    host_current_cpu = cpu;
#else
    write_gs_base((uintptr_t)&cpu->self);
#endif
}
