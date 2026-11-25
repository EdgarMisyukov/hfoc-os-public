#include "arch/x86_64/interrupts.h"
#include "cpu.h"
#include "ioapic.h"
#include "isa_irq3_7.h"
#include "isa_irq8_15.h"
#include "lapic.h"
#include "lapic_timer.h"
#include "scheduler.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/timer.h"
#include "keyboard.h"
#include "panic.h"
#include "process.h"
#include "serial.h"
#include <stdint.h>

#ifdef HFOC_USER_IRETQ_QEMU_TEST
__attribute__((noreturn)) void user_iretq_qemu_trap_complete(void);
#endif
#ifdef HFOC_SYSCALL_QEMU_TEST
__attribute__((noreturn)) void syscall_qemu_trap_complete(void);
#endif

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256] __attribute__((aligned(16)));
extern void *isr_stub_table[256];

static bool irq0_ioapic_active;
static uint64_t irq0_stray_pic_count;
/*
 * ITEM-475 (round 14): the IOAPIC IRQ0 bring-up lines used to be printed on
 * every delivery.  With the production tick back on PIT at 100 Hz that is 200
 * serial lines a second, which floods the console mirror and interleaves the
 * console's own records.  The two lines are bring-up evidence, so they are
 * published once, on the first delivery each proves.
 */
static bool irq0_ioapic_irq_published;
static bool irq0_ioapic_eoi_published;
static bool runtime_saved_ioapic;
static uint64_t runtime_previous_rte;
static bool runtime_saved_pic;
static uint8_t runtime_previous_pic_mask;
static bool pic_disable_policy_done;
static bool pic_disabled_published;

#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
static bool oneshot_test_hook_done;

static void lapic_timer_oneshot_test_hook(void) {
    if (oneshot_test_hook_done) {
        return;
    }
    oneshot_test_hook_done = true;
    const uint64_t ticks_per_second = lapic_timer_ticks_per_second();
    const uint64_t count = ticks_per_second / 100U;
    if (count == 0U || count > UINT32_MAX) {
        serial_write(
            "[LAPIC-TIMER] oneshot arm failed; periodic retained\n");
        return;
    }
    if (!lapic_timer_oneshot_arm((uint32_t)count)) {
        serial_write(
            "[LAPIC-TIMER] oneshot arm failed; periodic retained\n");
        return;
    }
    serial_write("[LAPIC-TIMER] oneshot armed: vector=0x43\n");
}
#endif

#ifdef HFOC_X2APIC_TEST
static bool x2apic_test_hook_done;

static void lapic_x2apic_test_hook(void) {
    if (x2apic_test_hook_done) {
        return;
    }
    x2apic_test_hook_done = true;
    const enum lapic_x2apic_status status = lapic_x2apic_transition();
    if (status != LAPIC_X2APIC_OK || !lapic_x2apic_active()) {
        serial_write("[X2APIC] transition failed: ");
        serial_write(lapic_x2apic_status_string(status));
        serial_write("\n");
        if (status == LAPIC_X2APIC_ROLLBACK_FAILED) {
            kernel_panic("x2APIC rollback verification failed");
        }
    }
}
#endif

static const char *const exception_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "BOUND range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved exception",
    "x87 floating-point exception", "Alignment check", "Machine check", "SIMD floating-point exception",
    "Virtualisation exception", "Control protection exception", "Reserved exception", "Reserved exception",
    "Reserved exception", "Reserved exception", "Reserved exception", "Reserved exception",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved exception"
};

/* Set an interrupt gate. ist_index uses the IST field (0=no IST, 1-7=IST1-IST7). */
static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist_index) {
    idt[vector].offset_low = (uint16_t)(handler & 0xffffU);
    idt[vector].selector = 0x08;
    idt[vector].ist = ist_index;
    idt[vector].attributes = 0x8e;
    idt[vector].offset_middle = (uint16_t)((handler >> 16U) & 0xffffU);
    idt[vector].offset_high = (uint32_t)((handler >> 32U) & 0xffffffffU);
    idt[vector].reserved = 0;
}

static int stack_in_range(const void *stack, size_t size, uint64_t rsp) {
    const uint64_t base = (uint64_t)(uintptr_t)stack;
    return (rsp > base && rsp < (base + size)) ? 1 : 0;
}

void interrupts_init(void) {
    for (uint16_t vector = 0; vector < 256U; ++vector) {
        /* NMI (vector 2) uses IST2, Double Fault (vector 8) uses IST1.
         * All others use IST=0. */
        uint8_t ist = (vector == 2U) ? 2U : (vector == 8U) ? 1U : 0U;
        idt_set_gate((uint8_t)vector, (uint64_t)(uintptr_t)isr_stub_table[vector], ist);
    }
    const struct idt_pointer pointer = {
        .limit = (uint16_t)(sizeof(idt) - 1U),
        .base = (uint64_t)(uintptr_t)&idt[0]
    };
    __asm__ volatile ("lidt %0" : : "m"(pointer));
}

void irq0_ioapic_active_publish(bool active) {
    irq0_ioapic_active = active;
}

static bool runtime_mask_active_source(void) {
    runtime_saved_ioapic = false;
    runtime_saved_pic = false;
    if (irq0_ioapic_active) {
        if (!ioapic_irq0_mask_save(&runtime_previous_rte)) {
            return false;
        }
        runtime_saved_ioapic = true;
        return true;
    }
    if (!pic_irq0_mask_save(&runtime_previous_pic_mask)) {
        return false;
    }
    runtime_saved_pic = true;
    return true;
}

static bool runtime_restore_source_masks(void) {
    bool restored = true;
    if (runtime_saved_ioapic) {
        restored = ioapic_irq0_restore(runtime_previous_rte) && restored;
    }
    if (runtime_saved_pic) {
        restored =
            pic_master_mask_restore(runtime_previous_pic_mask) && restored;
    }
    runtime_saved_ioapic = false;
    runtime_saved_pic = false;
    return restored;
}

static void pic_disable_after_apic_runtime(void) {
    if (pic_disable_policy_done ||
        !lapic_timer_runtime_active() ||
        !ioapic_is_irq1_routed()) {
        return;
    }
    pic_disable_policy_done = true;
    pic_disabled_published = pic_disable_transactional();
    if (pic_disabled_published) {
        serial_write("[PIC ] disabled; APIC runtime active.\n");
    } else {
        serial_write("[PIC ] retained; PIC disable failed.\n");
    }
}

static void lapic_timer_runtime_coordinate_irq0(void) {
    if (lapic_timer_runtime_active() ||
        lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED) {
        return;
    }
    if (lapic_timer_status() != LAPIC_TIMER_OK) {
        return;
    }
    if (!lapic_timer_runtime_prepare()) {
        lapic_timer_runtime_abort();
        return;
    }
    if (!runtime_mask_active_source()) {
        lapic_timer_runtime_abort();
        const bool restored = runtime_restore_source_masks();
        if (!restored) {
            serial_write("[LAPIC-TIMER] runtime rollback failed; "
                         "PIT source state not confirmed\n");
        }
        return;
    }
    if (!lapic_timer_runtime_activate()) {
        lapic_timer_runtime_abort();
        const bool restored = runtime_restore_source_masks();
        if (!restored) {
            serial_write("[LAPIC-TIMER] runtime rollback failed; "
                         "PIT source state not confirmed\n");
        }
        return;
    }
    pic_disable_after_apic_runtime();
#ifdef HFOC_X2APIC_TEST
    lapic_x2apic_test_hook();
#endif
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    lapic_timer_oneshot_test_hook();
#endif
}

#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
void irq0_ioapic_active_host_test_reset(void) {
    irq0_ioapic_active = false;
    irq0_stray_pic_count = 0;
    irq0_ioapic_irq_published = false;
    irq0_ioapic_eoi_published = false;
}

bool irq0_ioapic_active_host_test_is_active(void) {
    return irq0_ioapic_active;
}

uint64_t irq0_stray_pic_count_host_test_value(void) {
    return irq0_stray_pic_count;
}
#endif

#ifdef HFOC_LAPIC_TIMER_RUNTIME_HOST_TEST
void lapic_timer_runtime_host_test_reset(void) {
    runtime_saved_ioapic = false;
    runtime_previous_rte = 0U;
    runtime_saved_pic = false;
    runtime_previous_pic_mask = 0U;
    pic_disable_policy_done = false;
    pic_disabled_published = false;
    irq0_ioapic_irq_published = false;
    irq0_ioapic_eoi_published = false;
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    oneshot_test_hook_done = false;
#endif
}

bool pic_disable_host_test_is_published(void) {
    return pic_disabled_published;
}
#endif

void interrupt_dispatch(struct interrupt_frame *frame) {
    if (frame == 0) {
        kernel_panic("Interrupt without stack frame");
    }
    if (frame->vector < 32U) {
        /* IST stack diagnostic for Double Fault and NMI.
         * Read RSP to verify we are on the correct IST stack. */
        if (frame->vector == 8U || frame->vector == 2U) {
            struct cpu *current = cpu_current();
            uint64_t rsp;
            __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
            if (frame->vector == 8U) {
                const int on_ist1 = stack_in_range(
                    current->ist1_stack, sizeof(current->ist1_stack), rsp);
                serial_write(on_ist1
                    ? "double-fault: ist=1 stack=ok\n"
                    : "double-fault: ist=1 stack=bad\n");
            } else {
                const int on_ist2 = stack_in_range(
                    current->ist2_stack, sizeof(current->ist2_stack), rsp);
                serial_write(on_ist2
                    ? "nmi: ist=2 stack=ok\n"
                    : "nmi: ist=2 stack=bad\n");
            }
        }
#ifdef HFOC_USER_IRETQ_QEMU_TEST
        if (frame->vector == 6U && (frame->cs & 3U) == 3U) {
            user_iretq_qemu_trap_complete();
        }
#endif
#ifdef HFOC_SYSCALL_QEMU_TEST
        if (frame->vector == 6U && (frame->cs & 3U) == 3U) {
            syscall_qemu_trap_complete();
        }
#endif
#if !defined(HFOC_LAPIC_TIMER_RUNTIME_HOST_TEST) && \
    !defined(HFOC_DISPATCH_HOST_TEST)
        /*
         * ITEM-625: a user-mode #PF, #GP or #UD is the process's fault, not
         * the kernel's.  Mark it exited with a status derived from the
         * vector and leave the CPU through the scheduler; only a failed
         * scheduler transition falls through to the kernel panic.
         */
        if ((frame->cs & 3U) == 3U &&
            (frame->vector == 6U || frame->vector == 13U ||
             frame->vector == 14U)) {
            struct scheduler_task *task = NULL;
            struct kernel_process *process = NULL;

            if (sched_current(&task) == SCHEDULER_OK && task != NULL &&
                task->process_owner != NULL) {
                process = (struct kernel_process *)task->process_owner;
            }
            if (process != NULL) {
                const uint64_t flags = serial_sequence_begin();

                serial_write("[USER-FAULT] vector=");
                serial_write_u64(frame->vector);
                serial_write(" pid=");
                serial_write_u64(process->pcb.pid);
                serial_write(" rip=");
                serial_write_hex(frame->rip);
                serial_write("\n");
                serial_sequence_end(flags);
                (void)process_exit_kernel(
                    process, 128U + (uint32_t)frame->vector);
                (void)sched_exit();
            }
        }
#endif
        kernel_panic_exception(exception_names[frame->vector], frame);
    }
    struct cpu *current = cpu_current();
    if (current != NULL && current->index != 0U &&
        frame->vector >= 0x20U && frame->vector <= 0x50U) {
        serial_write("[SMP ] unexpected AP vector ");
        serial_write_u64(frame->vector);
        serial_write("; BSP-owned handler not entered\n");
        return;
    }
    if (frame->vector == 32U) {
        if (lapic_timer_runtime_active()) {
            pic_send_eoi(0);
        } else if (irq0_ioapic_active) {
            irq0_stray_pic_count++;
            serial_write("[PIC ] stray IRQ0 after IOAPIC transition; EOI sent.\n");
            pic_send_eoi(0);
        } else {
            timer_handle_irq();
            /*
             * ITEM-475 (round 14): the ordinary production build keeps the
             * PIT as the tick source, so this coordinator is compiled out
             * there and its `tick_source=pit` policy is what the journal
             * names.  The drill/QEMU builds keep the verified transition.
             */
            if (lapic_timer_runtime_transition_allowed()) {
                lapic_timer_runtime_coordinate_irq0();
            }
            pic_send_eoi(0);
            /*
             * ITEM-633: the scheduler is initialized lazily for the owner
             * shell.  The PIT tick only expires its timeout queue; it does
             * not preempt from the IRQ handler (the ordinary loop's
             * sched_yield runs the woken task).  No routing change.
             */
#ifndef HFOC_DISPATCH_HOST_TEST
            (void)sched_timeout_tick();
#endif
        }
        return;
    }
    if (frame->vector == 33U) {
        keyboard_handle_irq();
        pic_send_eoi(1);
        return;
    }
    if (frame->vector == 0x40U) {
        if (!irq0_ioapic_irq_published) {
            irq0_ioapic_irq_published = true;
            serial_write("[IOAPIC] irq0 vector=0x40\n");
        }
        if (!lapic_timer_runtime_active() && irq0_ioapic_active) {
            timer_handle_irq();
            /* ITEM-633: the production IOAPIC IRQ0 path is the tick source
               on this machine; expire scheduler waits here too. */
#ifndef HFOC_DISPATCH_HOST_TEST
            (void)sched_timeout_tick();
#endif
            /* ITEM-475: the same production-PIT policy guard as vector 0x20. */
            if (lapic_timer_runtime_transition_allowed()) {
                lapic_timer_runtime_coordinate_irq0();
            }
        }
        lapic_eoi();
        if (!irq0_ioapic_eoi_published) {
            irq0_ioapic_eoi_published = true;
            serial_write("[IOAPIC] irq0 lapic_eoi\n");
        }
        return;
    }
    if (frame->vector == 0x42U) {
        serial_write("[LAPIC-TIMER] irq vector=0x42\n");
        if (lapic_timer_runtime_active()) {
            timer_handle_irq();
            lapic_eoi();
            serial_write("[LAPIC-TIMER] irq lapic_eoi\n");
            (void)sched_on_tick();
        } else {
            serial_write("[LAPIC-TIMER] irq lapic_eoi\n");
        }
        return;
    }
    if (frame->vector == 0x41U) {
        serial_write("[IOAPIC] irq1 vector=0x41\n");
        keyboard_handle_irq();
        lapic_eoi();
        serial_write("[IOAPIC] irq1 lapic_eoi\n");
        return;
    }
    if (frame->vector == 0x43U) {
        lapic_timer_oneshot_handle_irq();
        lapic_eoi();
        return;
    }
    if (frame->vector >= 0x44U && frame->vector <= 0x48U) {
        isa_irq3_7_handle_irq(
            (uint8_t)(frame->vector - 0x44U + 3U));
        lapic_eoi();
        return;
    }
    if (frame->vector >= 0x49U && frame->vector <= 0x50U) {
        isa_irq8_15_handle_irq(
            (uint8_t)(frame->vector - 0x49U + 8U));
        lapic_eoi();
        return;
    }
    if (frame->vector >= 32U && frame->vector <= 47U) {
        const uint8_t irq = (uint8_t)(frame->vector - 32U);
        bool is_spurious = false;
        if ((irq == 7U || irq == 15U) &&
            pic_is_spurious_irq(irq, &is_spurious) && is_spurious) {
            if (irq == 15U) {
                pic_send_eoi(2U);
            }
            return;
        }
        pic_send_eoi(irq);
        return;
    }

    /* Local APIC spurious interrupt (vector 0xEF).
     * Handler only returns; a spurious interrupt does not require EOI. */
    if (frame->vector == 0xEFU) {
        return;
    }

    serial_write("[INT ] Unexpected vector ");
    serial_write_u64(frame->vector);
    serial_write("\n");
}

void interrupts_make_pf_gate_not_present(void) {
    /* Clear the Present bit (bit 7) of idt[14].attributes.
     * 0x8e -> 0x0e: interrupt gate, DPL=0, not-present. */
    idt[14].attributes = 0x0e;
}
