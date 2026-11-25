#include "panic.h"
#include "appliance_boot_log_store.h"
#include "install_journal_platform.h"
#include "panic_record.h"
#include "arch/x86_64/io.h"
#include "framebuffer.h"
#include "pmm.h"
#include "serial.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ITEM-390: bounded single-line panic detail. */
#define PANIC_DETAIL_MAX 400U

/* The kernel's direct map base, published by the VMM from Limine's response;
 * the page-fault record needs it to turn CR2 into a physical address. */
extern uint64_t vmm_hhdm_offset(void);

_Static_assert(HFOC_PMM_FAULT_LINE_MAX <= PANIC_DETAIL_MAX,
               "the page-fault record must fit the panic detail line");

static bool panic_active;
static char panic_detail_line[PANIC_DETAIL_MAX];

struct panic_context {
    const struct interrupt_frame *frame;
    uint64_t cr2;
    bool has_cr2;
};

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void serial_page_fault(uint64_t error_code) {
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(" page-fault: cause=");
        serial_write((error_code & (1ULL << 0U)) != 0U ? "protection-violation" : "not-present");
        serial_write(" access=");
        serial_write((error_code & (1ULL << 1U)) != 0U ? "write" : "read");
        serial_write(" mode=");
        serial_write((error_code & (1ULL << 2U)) != 0U ? "user" : "supervisor");
        serial_write(" reserved=");
        serial_write((error_code & (1ULL << 3U)) != 0U ? "1" : "0");
        serial_write(" instruction=");
        serial_write((error_code & (1ULL << 4U)) != 0U ? "1" : "0");
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static void serial_frame(const struct panic_context *context) {
    const struct interrupt_frame *frame = context == 0 ? 0 : context->frame;
    if (frame == 0) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(" vector=");
        serial_write_u64(frame->vector);
        serial_write(" error=");
        serial_write_hex(frame->error_code);
        serial_write(" rip=");
        serial_write_hex(frame->rip);
        if (context->has_cr2) {
            serial_write(" cr2=");
            serial_write_hex(context->cr2);
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (frame->vector == 14U) {
        serial_page_fault(frame->error_code);
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(" rax="); serial_write_hex(frame->rax);
        serial_write(" rbx="); serial_write_hex(frame->rbx);
        serial_write(" rcx="); serial_write_hex(frame->rcx);
        serial_write(" rdx="); serial_write_hex(frame->rdx);
        serial_write("\n rsi="); serial_write_hex(frame->rsi);
        serial_sequence_end(serial_flags);
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(" rdi="); serial_write_hex(frame->rdi);
        serial_write(" rbp="); serial_write_hex(frame->rbp);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static void copy_panic_detail(const char *detail) {
    if (detail == NULL) {
        panic_detail_line[0] = '\0';
        return;
    }
    size_t index = 0U;
    while (detail[index] != '\0' && index + 1U < PANIC_DETAIL_MAX) {
        panic_detail_line[index] = detail[index];
        ++index;
    }
    panic_detail_line[index] = '\0';
}

/*
 * ITEM-521 (increment 11): the panic is a record, not only a screen.
 *
 * Requirement 1 names the panic among the records the medium's journal must
 * carry, and the flush below only writes what was staged *before* the machine
 * stopped: on a machine without a serial port the journal ended on whatever
 * ran last and never said that the run had panicked.  The two records are
 * staged here, through the same durable path every other record takes (the
 * boot-log store, whose admitted set the install journal on the medium
 * subscribes to), so the flush that follows carries them - and a refusal on
 * the medium is still named by the journal's own core, never invented here.
 * Best effort by construction: the renderers refuse rather than truncate and
 * nothing in this path may depend on a device.
 */
static void publish_panic_record(const char *message,
                                 const struct panic_context *context) {
    const struct interrupt_frame *frame = context == 0 ? 0 : context->frame;
    char record[PANIC_RECORD_MAX];
    char detail[PANIC_RECORD_DETAIL_LINE_MAX];
    size_t length;

    length = panic_record_render(record, sizeof(record), message,
                                 frame != 0,
                                 frame != 0 ? frame->vector : 0U,
                                 frame != 0 ? frame->error_code : 0U,
                                 frame != 0 ? frame->rip : 0U);
    if (length != 0U) {
        appliance_boot_log_store_observe(record, length);
    }
    length = panic_record_detail(detail, sizeof(detail),
                                 panic_detail_line[0] != '\0'
                                     ? panic_detail_line
                                     : NULL);
    if (length != 0U) {
        appliance_boot_log_store_observe(detail, length);
    }
}

static _Noreturn void panic_finish(const char *message, const struct panic_context *context) {
    const struct interrupt_frame *frame = context == 0 ? 0 : context->frame;
    cpu_cli();
    if (panic_active) {
        for (;;) {
            cpu_hlt();
        }
    }
    panic_active = true;

    serial_write("\n*** HFOC KERNEL PANIC ***\n");
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(message == 0 ? "Unknown panic" : message);
        serial_write("\n");
        if (panic_detail_line[0] != '\0') {
            serial_write("[PANIC] ");
            serial_write(panic_detail_line);
            serial_write("\n");
        }
        serial_sequence_end(serial_flags);
    }
    serial_frame(context);

    if (framebuffer_ready()) {
        framebuffer_clear(0x3f0b12);
        framebuffer_rect(0, 0, framebuffer_width(), 72, 0x991b1b);
        framebuffer_text(24, 18, "HFOC KERNEL PANIC", 0xffffff, 4);
        framebuffer_text(24, 96, message == 0 ? "UNKNOWN PANIC" : message, 0xfecaca, 2);
        if (frame != 0) {
            framebuffer_text(24, 140, "VECTOR:", 0xffffff, 2);
            framebuffer_u64(132, 140, frame->vector, 0xfde68a, 2);
            framebuffer_text(24, 174, "ERROR:", 0xffffff, 2);
            framebuffer_hex64(120, 174, frame->error_code, 0xfde68a, 2);
            framebuffer_text(24, 208, "RIP:", 0xffffff, 2);
            framebuffer_hex64(84, 208, frame->rip, 0xfde68a, 2);
            if (context->has_cr2) {
                framebuffer_text(24, 242, "CR2:", 0xffffff, 2);
                framebuffer_hex64(84, 242, context->cr2, 0xfde68a, 2);
                framebuffer_text(24, 286, "SYSTEM HALTED - RESET QEMU", 0xffffff, 2);
            } else {
                framebuffer_text(24, 252, "SYSTEM HALTED - RESET QEMU", 0xffffff, 2);
            }
        } else {
            framebuffer_text(24, 150, "SYSTEM HALTED - RESET QEMU", 0xffffff, 2);
        }
    }
    if (panic_detail_line[0] != '\0') {
        framebuffer_text(24, 320, panic_detail_line, 0xfecaca, 1);
    }
    /*
     * ITEM-486 (round 24): the panic screen is drawn into the RAM shadow now, so
     * it must be flushed to the device before the machine halts - otherwise the
     * last thing the operator sees is the previous picture.
     */
    framebuffer_sync();

    /*
     * ITEM-410: a panic whose last records are missing is the least useful
     * case there is, so the staged records are written to the appliance's own
     * disk before the machine stops.  The flush is best effort and swallows
     * every failure by construction: nothing in the panic path may depend on
     * the device that may have caused the panic.
     */
    appliance_boot_log_store_panic_flush();
    /*
     * ITEM-521 requirement 2: the install journal's tail goes to the medium by
     * the same rule - best effort, bounded by the placement's own flush, and
     * never able to keep the machine from stopping.  The panic's own records
     * are staged between the two flushes: after the store's, so a record that
     * happens to name one of the log's flush moments can never publish over the
     * panic screen (`store.panic` is already set), and before the journal's, so
     * the tail that reaches the medium names the panic the machine stopped on.
     */
    publish_panic_record(message, context);
    (void)install_journal_platform_panic_flush();

    for (;;) {
        cpu_hlt();
    }
}

_Noreturn void kernel_panic(const char *message) {
    copy_panic_detail(NULL);
    panic_finish(message, 0);
}

_Noreturn void kernel_panic_detail(const char *message, const char *detail) {
    copy_panic_detail(detail);
    panic_finish(message, 0);
}

_Noreturn void kernel_panic_exception(const char *message, const struct interrupt_frame *frame) {
    copy_panic_detail(NULL);
    struct panic_context context = {
        .frame = frame,
        .cr2 = 0,
        .has_cr2 = false
    };
    if (frame != 0 && frame->vector == 14U) {
        context.cr2 = read_cr2();
        context.has_cr2 = true;
        /*
         * ITEM-393: a write to a physical address the allocator never tracked
         * is the shape of a fault that only a real machine's firmware map
         * produces, and on a machine without a serial console the panic screen
         * is the only witness. Name the physical address behind CR2 and what
         * the PMM knows about it, on the same one line the screen already
         * draws; the virtual address stays where it is.
         */
        const uint64_t hhdm_offset = vmm_hhdm_offset();
        if (context.cr2 >= hhdm_offset) {
            (void)pmm_fault_line(
                context.cr2 - hhdm_offset,
                panic_detail_line,
                sizeof(panic_detail_line));
        }
    }
    panic_finish(message, &context);
}
