#include "smp.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/tss.h"
#include "cpu.h"
#include "lapic.h"
#include "pmm.h"
#include "serial.h"
#include <stddef.h>
#include <stdint.h>

#define SMP_PAGE_SIZE 4096U

#ifdef HFOC_SMP_HOST_TEST
#define SMP_ONLINE_WAIT_ITERATIONS 8U
#else
#define SMP_ONLINE_WAIT_ITERATIONS 100000000U
#endif

static const struct acpi_madt_info *smp_madt;
static uint64_t smp_hhdm_offset;

static bool allocate_ap_stack(uint64_t hhdm_offset, uintptr_t *out_top) {
    const uint64_t first_page =
        pmm_alloc_contiguous_pages(HFOC_AP_STACK_PAGES);
    if (first_page == 0U) {
        return false;
    }

    const uint64_t end =
        first_page + ((uint64_t)HFOC_AP_STACK_PAGES * SMP_PAGE_SIZE);
    if (hhdm_offset > UINT64_MAX - end) {
        return false;
    }
    *out_top = (uintptr_t)(hhdm_offset + end);
    return true;
}

static void publish_ap(struct limine_mp_info *info, struct cpu *cpu) {
    info->reserved = (uint64_t)cpu->kernel_stack_top;
    info->extra_argument = (uint64_t)cpu->index;
#ifdef HFOC_SMP_HOST_TEST
    smp_test_note_ap_prepared(cpu->index);
#endif
    __atomic_store_n(&info->goto_address, smp_ap_entry, __ATOMIC_RELEASE);
}

#ifndef HFOC_SMP_HOST_TEST
static void smp_ap_failed_idle(void) {
    for (;;) {
        __asm__ volatile ("pause");
    }
}
#endif

void smp_ap_idle(void) {
#ifdef HFOC_SMP_HOST_TEST
    smp_test_note_ap_idle();
#else
    for (;;) {
        __asm__ volatile ("pause");
    }
#endif
}

static void wait_for_ap(struct limine_mp_info *info, struct cpu *cpu) {
    (void)info;
#ifdef HFOC_SMP_HOST_TEST
    if (smp_test_auto_start()) {
        smp_ap_entry(info);
    }
#endif

    for (uint32_t spin = 0U; spin < SMP_ONLINE_WAIT_ITERATIONS; ++spin) {
        const enum cpu_state state =
            __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE);
        if (state == CPU_STATE_ONLINE || state == CPU_STATE_FAILED) {
            return;
        }
#ifdef HFOC_SMP_HOST_TEST
        for (volatile uint32_t delay = 0U; delay < 100U; ++delay) {
        }
#else
        __asm__ volatile ("pause");
#endif
    }

    __atomic_store_n(&cpu->state, CPU_STATE_FAILED, __ATOMIC_RELEASE);
    serial_write("[SMP ] AP bring-up failed: online timeout\n");
}

static void mark_ap_failed(struct cpu *cpu, const char *reason) {
    __atomic_store_n(&cpu->state, CPU_STATE_FAILED, __ATOMIC_RELEASE);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SMP ] AP bring-up failed: ");
        serial_write(reason);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

enum smp_status smp_init(
    const struct limine_mp_response *mp_response,
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset)
{
    if (mp_response == NULL || mp_response->cpu_count == 0U) {
        serial_write("[SMP ] BSP-only: no MP response\n");
        return SMP_BSP_ONLY;
    }
    if (mp_response->cpus == NULL) {
        serial_write("[SMP ] invalid MP response\n");
        return SMP_INVALID_RESPONSE;
    }
    if (mp_response->cpu_count > HFOC_MAX_CPUS) {
        serial_write("[SMP ] CPU capacity overflow\n");
        return SMP_OUT_OF_CAPACITY;
    }
    if (madt == NULL) {
        serial_write("[SMP ] BSP-only: no ACPI topology\n");
        return SMP_BSP_ONLY;
    }

    const uint32_t bsp_lapic_id = lapic_bsp_apic_id();
    if (mp_response->bsp_lapic_id != bsp_lapic_id) {
        serial_write("[SMP ] BSP LAPIC ID mismatch; AP startup disabled\n");
        return SMP_BSP_ID_MISMATCH;
    }

    smp_madt = madt;
    smp_hhdm_offset = hhdm_offset;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SMP ] BSP lapic=");
        serial_write_u64(bsp_lapic_id);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    uint32_t next_index = 1U;
    for (uint64_t entry = 0U; entry < mp_response->cpu_count; ++entry) {
        struct limine_mp_info *info = mp_response->cpus[entry];
        if (info == NULL) {
            serial_write("[SMP ] invalid MP CPU entry\n");
            return SMP_INVALID_RESPONSE;
        }
        if (info->lapic_id == bsp_lapic_id) {
            continue;
        }

        struct cpu *cpu = cpu_by_index(next_index);
        uintptr_t stack_top = 0U;
        if (!allocate_ap_stack(hhdm_offset, &stack_top)) {
            cpu_init_ap(cpu, next_index, info->processor_id,
                        info->lapic_id, 0U);
            mark_ap_failed(cpu, "stack allocation");
            ++next_index;
            continue;
        }

        cpu_init_ap(cpu, next_index, info->processor_id,
                    info->lapic_id, stack_top);
        tss_init_for(cpu);
        publish_ap(info, cpu);
        wait_for_ap(info, cpu);

        const enum cpu_state state =
            __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE);
        if (state == CPU_STATE_ONLINE) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[SMP ] AP ");
                serial_write_u64(cpu->index);
                serial_write(" online: lapic=");
                serial_write_u64(cpu->lapic_id);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
        } else if (state == CPU_STATE_FAILED) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[SMP ] AP ");
                serial_write_u64(cpu->index);
                serial_write(" failed\n");
                serial_sequence_end(serial_flags);
            }
        }
        ++next_index;
    }

    return SMP_OK;
}

void smp_ap_entry(struct limine_mp_info *info) {
    if (info == NULL) {
#ifdef HFOC_SMP_HOST_TEST
        return;
#else
        smp_ap_failed_idle();
#endif
    }

    const uint32_t index = (uint32_t)info->extra_argument;
#ifdef HFOC_SMP_HOST_TEST
    smp_test_note_ap_entered(index);
#endif

    struct cpu *cpu = cpu_by_index(index);
    if (cpu == NULL || cpu->self != cpu ||
        cpu->state != CPU_STATE_STARTING ||
        cpu->lapic_id != info->lapic_id) {
        if (cpu != NULL) {
            __atomic_store_n(&cpu->state, CPU_STATE_FAILED,
                             __ATOMIC_RELEASE);
        }
#ifdef HFOC_SMP_HOST_TEST
        return;
#else
        smp_ap_failed_idle();
#endif
    }

    gdt_init_for(cpu);
    gdt_load_tr_for(cpu);
    interrupts_init();
    gdt_reload_segments();
    cpu_gs_base_set(cpu);

    const enum lapic_status lapic_status =
        lapic_init_ap(smp_madt, smp_hhdm_offset, cpu->lapic_id);
    if (lapic_status != LAPIC_OK) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SMP ] AP bring-up failed: LAPIC ");
            serial_write(lapic_status_string(lapic_status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        __atomic_store_n(&cpu->state, CPU_STATE_FAILED, __ATOMIC_RELEASE);
#ifdef HFOC_SMP_HOST_TEST
        return;
#else
        smp_ap_failed_idle();
#endif
    }

    __atomic_store_n(&cpu->state, CPU_STATE_ONLINE, __ATOMIC_RELEASE);
    smp_ap_idle();
}

bool smp_online(void) {
    struct cpu *bsp = cpu_by_index(0U);
    return bsp != NULL &&
           (bsp->state == CPU_STATE_BSP || bsp->state == CPU_STATE_ONLINE);
}

uint32_t smp_online_ap_count(void) {
    uint32_t count = 0U;
    for (uint32_t index = 1U; index < HFOC_MAX_CPUS; ++index) {
        struct cpu *cpu = cpu_by_index(index);
        if (cpu != NULL &&
            __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) ==
                CPU_STATE_ONLINE) {
            ++count;
        }
    }
    return count;
}

const char *smp_status_string(enum smp_status status) {
    switch (status) {
    case SMP_OK:                 return "OK";
    case SMP_BSP_ONLY:           return "BSP only";
    case SMP_INVALID_RESPONSE:   return "invalid response";
    case SMP_BSP_ID_MISMATCH:    return "BSP ID mismatch";
    case SMP_OUT_OF_CAPACITY:    return "out of capacity";
    default:                     return "unknown";
    }
}
