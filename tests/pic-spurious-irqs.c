#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/tss.h"
#include "ioapic.h"
#include "lapic_timer.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST
#include "ioapic.h"
#endif

#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
void irq0_ioapic_active_publish(bool active);
void irq0_ioapic_active_host_test_reset(void);
bool irq0_ioapic_active_host_test_is_active(void);
uint64_t irq0_stray_pic_count_host_test_value(void);
#endif

#define PIC1_COMMAND 0x20U
#define PIC1_DATA    0x21U
#define PIC2_COMMAND 0xa0U
#define PIC2_DATA    0xa1U
#define PIC_READ_ISR 0x0bU
#define PIC_EOI      0x20U

enum io_kind {
    IO_READ,
    IO_WRITE
};

struct io_event {
    enum io_kind kind;
    uint16_t port;
    uint8_t value;
};

static struct io_event events[16];
static unsigned int event_count;
static uint8_t master_mask;
static uint8_t slave_mask;
static uint8_t master_isr;
static uint8_t slave_isr;

void *isr_stub_table[256];

static void record_event(enum io_kind kind, uint16_t port, uint8_t value) {
    if (event_count >= sizeof(events) / sizeof(events[0])) {
        fputs("too many PIC I/O events\n", stderr);
        exit(2);
    }
    events[event_count++] = (struct io_event){ kind, port, value };
}

void outb(uint16_t port, uint8_t value) {
    switch (port) {
        case PIC1_DATA:
            master_mask = value;
            break;
        case PIC2_DATA:
            slave_mask = value;
            break;
        default:
            break;
    }
    record_event(IO_WRITE, port, value);
}

uint8_t inb(uint16_t port) {
    uint8_t value;

    switch (port) {
        case PIC1_COMMAND:
            value = master_isr;
            break;
        case PIC1_DATA:
            value = master_mask;
            break;
        case PIC2_COMMAND:
            value = slave_isr;
            break;
        case PIC2_DATA:
            value = slave_mask;
            break;
        default:
            fprintf(stderr, "unexpected PIC read port: 0x%02x\n", port);
            exit(2);
    }

    record_event(IO_READ, port, value);
    return value;
}

void io_wait(void) {
}

#ifdef HFOC_DISPATCH_HOST_TEST
static unsigned keyboard_calls;
static unsigned lapic_eoi_calls;
static unsigned timer_calls;

void timer_handle_irq(void) {
    ++timer_calls;
}

enum scheduler_status sched_on_tick(void) {
    return SCHEDULER_OK;
}

void keyboard_handle_irq(void) {
    ++keyboard_calls;
}

void lapic_eoi(void) {
    ++lapic_eoi_calls;
}

void lapic_timer_oneshot_handle_irq(void) {
}

static unsigned interrupt_dispatch_calls;

void __real_interrupt_dispatch(struct interrupt_frame *frame);

void __wrap_interrupt_dispatch(struct interrupt_frame *frame) {
    interrupt_dispatch_calls++;
    __real_interrupt_dispatch(frame);
}

bool ioapic_is_irq0_routed(void) {
    fputs("dispatcher re-read IOAPIC IRQ0 state\n", stderr);
    exit(2);
}

bool ioapic_is_irq1_routed(void) {
    return false;
}
#else
void timer_handle_irq(void) {
}

void keyboard_handle_irq(void) {
}
#endif

void isa_irq3_7_handle_irq(uint8_t irq) {
    (void)irq;
}

void isa_irq8_15_handle_irq(uint8_t irq) {
    (void)irq;
}

_Noreturn void kernel_panic(const char *message) {
    fprintf(stderr, "unexpected panic: %s\n", message);
    exit(2);
}

_Noreturn void kernel_panic_exception(const char *message,
                                      const struct interrupt_frame *frame) {
    (void)frame;
    fprintf(stderr, "unexpected exception panic: %s\n", message);
    exit(2);
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

/* Stubs for IST/TSS symbols referenced by interrupt_dispatch. */
static uint8_t dummy_ist1_stack[16];

static uint8_t dummy_ist2_stack[16];

const void *tss_ist1_stack(void) {
    return dummy_ist1_stack;
}

size_t tss_ist1_stack_size(void) {
    return sizeof(dummy_ist1_stack);
}

const void *tss_ist2_stack(void) {
    return dummy_ist2_stack;
}

size_t tss_ist2_stack_size(void) {
    return sizeof(dummy_ist2_stack);
}

enum lapic_timer_status lapic_timer_status(void) {
    return LAPIC_TIMER_IDLE;
}

enum lapic_timer_runtime_status lapic_timer_runtime_status(void) {
    return LAPIC_TIMER_RUNTIME_IDLE;
}

bool lapic_timer_runtime_active(void) {
    return false;
}

/* ITEM-475: this host proof drives the PIC ownership path, where the tick
   source stays PIT, so the transition policy denies the LAPIC coordinator. */
bool lapic_timer_runtime_transition_allowed(void) {
    return false;
}

bool lapic_timer_runtime_prepare(void) {
    return false;
}

bool lapic_timer_runtime_activate(void) {
    return false;
}

void lapic_timer_runtime_abort(void) {
}

bool ioapic_irq0_mask_save(uint64_t *out_previous_rte) {
    if (out_previous_rte != NULL) {
        *out_previous_rte = 0U;
    }
    return false;
}

bool ioapic_irq0_restore(uint64_t previous_rte) {
    (void)previous_rte;
    return false;
}

static void reset_io(uint8_t initial_master_mask, uint8_t initial_slave_mask,
                     uint8_t initial_master_isr, uint8_t initial_slave_isr) {
    event_count = 0U;
    master_mask = initial_master_mask;
    slave_mask = initial_slave_mask;
    master_isr = initial_master_isr;
    slave_isr = initial_slave_isr;
}

static int expect_events(const char *name, const struct io_event *expected,
                         unsigned int expected_count) {
    if (event_count != expected_count) {
        fprintf(stderr, "%s: expected %u I/O events, got %u\n",
                name, expected_count, event_count);
        return 1;
    }
    for (unsigned int index = 0U; index < expected_count; ++index) {
        if (events[index].kind != expected[index].kind ||
            events[index].port != expected[index].port ||
            events[index].value != expected[index].value) {
            fprintf(stderr,
                    "%s event %u: expected %c port=0x%02x value=0x%02x, "
                    "got %c port=0x%02x value=0x%02x\n",
                    name, index,
                    expected[index].kind == IO_READ ? 'R' : 'W',
                    expected[index].port, expected[index].value,
                    events[index].kind == IO_READ ? 'R' : 'W',
                    events[index].port, events[index].value);
            return 1;
        }
    }
    return 0;
}

typedef bool (*pic_irq_operation)(uint8_t irq);

static int run_operation_case(const char *name, pic_irq_operation operation,
                              uint8_t irq, uint8_t initial_master_mask,
                              uint8_t initial_slave_mask, bool expected_status,
                              const struct io_event *expected,
                              unsigned int expected_count) {
    reset_io(initial_master_mask, initial_slave_mask, 0U, 0U);

    const bool status = operation(irq);
    if (status != expected_status) {
        fprintf(stderr, "%s: expected status %s, got %s\n", name,
                expected_status ? "true" : "false", status ? "true" : "false");
        return 1;
    }
    return expect_events(name, expected, expected_count);
}

static int run_dispatch_case(const char *name, uint64_t vector,
                             uint8_t initial_master_isr,
                             uint8_t initial_slave_isr,
                             const struct io_event *expected,
                             unsigned int expected_count) {
    struct interrupt_frame frame = { .vector = vector };
    reset_io(0xffU, 0xffU, initial_master_isr, initial_slave_isr);

    interrupt_dispatch(&frame);

    return expect_events(name, expected, expected_count);
}

static int test_unmask(void) {
    static const struct io_event irq0[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };
    static const struct io_event irq1[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0xfdU }
    };
    static const struct io_event irq7[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0x7fU }
    };
    static const struct io_event irq8[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0xfbU },
        { IO_READ, PIC2_DATA, 0xffU },
        { IO_WRITE, PIC2_DATA, 0xfeU }
    };
    static const struct io_event irq15[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0xfbU },
        { IO_READ, PIC2_DATA, 0xffU },
        { IO_WRITE, PIC2_DATA, 0x7fU }
    };

    int status = 0;
    status |= run_operation_case("unmask IRQ0", pic_unmask, 0U, 0xffU, 0xffU,
                                 true, irq0, sizeof(irq0) / sizeof(irq0[0]));
    status |= run_operation_case("unmask IRQ1", pic_unmask, 1U, 0xffU, 0xffU,
                                 true, irq1, sizeof(irq1) / sizeof(irq1[0]));
    status |= run_operation_case("unmask IRQ2", pic_unmask, 2U, 0xffU, 0xffU,
                                 false, NULL, 0U);
    status |= run_operation_case("unmask IRQ7", pic_unmask, 7U, 0xffU, 0xffU,
                                 true, irq7, sizeof(irq7) / sizeof(irq7[0]));
    status |= run_operation_case("unmask IRQ8", pic_unmask, 8U, 0xffU, 0xffU,
                                 true, irq8, sizeof(irq8) / sizeof(irq8[0]));
    status |= run_operation_case("unmask IRQ15", pic_unmask, 15U, 0xffU, 0xffU,
                                 true, irq15, sizeof(irq15) / sizeof(irq15[0]));
    status |= run_operation_case("unmask invalid IRQ16", pic_unmask, 16U,
                                 0xffU, 0xffU, false, NULL, 0U);
    status |= run_operation_case("unmask invalid IRQ255", pic_unmask, 255U,
                                 0xffU, 0xffU, false, NULL, 0U);
    return status;
}

static int test_mask(void) {
    static const struct io_event irq0[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event irq1[] = {
        { IO_READ, PIC1_DATA, 0xfdU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event irq2[] = {
        { IO_READ, PIC1_DATA, 0xfbU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event irq7[] = {
        { IO_READ, PIC1_DATA, 0x7fU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event irq8[] = {
        { IO_READ, PIC2_DATA, 0xfeU },
        { IO_WRITE, PIC2_DATA, 0xffU }
    };
    static const struct io_event irq15[] = {
        { IO_READ, PIC2_DATA, 0x7fU },
        { IO_WRITE, PIC2_DATA, 0xffU }
    };

    int status = 0;
    status |= run_operation_case("mask IRQ0", pic_mask, 0U, 0xfeU, 0xffU,
                                 true, irq0, sizeof(irq0) / sizeof(irq0[0]));
    status |= run_operation_case("mask IRQ1", pic_mask, 1U, 0xfdU, 0xffU,
                                 true, irq1, sizeof(irq1) / sizeof(irq1[0]));
    status |= run_operation_case("mask IRQ2", pic_mask, 2U, 0xfbU, 0xffU,
                                 true, irq2, sizeof(irq2) / sizeof(irq2[0]));
    status |= run_operation_case("mask IRQ7", pic_mask, 7U, 0x7fU, 0xffU,
                                 true, irq7, sizeof(irq7) / sizeof(irq7[0]));
    status |= run_operation_case("mask IRQ8", pic_mask, 8U, 0xfbU, 0xfeU,
                                 true, irq8, sizeof(irq8) / sizeof(irq8[0]));
    status |= run_operation_case("mask IRQ15", pic_mask, 15U, 0xfbU, 0x7fU,
                                 true, irq15, sizeof(irq15) / sizeof(irq15[0]));
    status |= run_operation_case("mask invalid IRQ16", pic_mask, 16U,
                                 0xffU, 0xffU, false, NULL, 0U);
    status |= run_operation_case("mask invalid IRQ255", pic_mask, 255U,
                                 0xffU, 0xffU, false, NULL, 0U);
    return status;
}

static int test_eoi(void) {
    static const struct io_event master[] = {
        { IO_WRITE, PIC1_COMMAND, PIC_EOI }
    };
    static const struct io_event slave_then_master[] = {
        { IO_WRITE, PIC2_COMMAND, PIC_EOI },
        { IO_WRITE, PIC1_COMMAND, PIC_EOI }
    };

    int status = 0;
    status |= run_operation_case("EOI IRQ0", pic_send_eoi, 0U, 0xffU, 0xffU,
                                 true, master, sizeof(master) / sizeof(master[0]));
    status |= run_operation_case("EOI IRQ7", pic_send_eoi, 7U, 0xffU, 0xffU,
                                 true, master, sizeof(master) / sizeof(master[0]));
    status |= run_operation_case("EOI IRQ8", pic_send_eoi, 8U, 0xffU, 0xffU,
                                 true, slave_then_master,
                                 sizeof(slave_then_master) / sizeof(slave_then_master[0]));
    status |= run_operation_case("EOI IRQ15", pic_send_eoi, 15U, 0xffU, 0xffU,
                                 true, slave_then_master,
                                 sizeof(slave_then_master) / sizeof(slave_then_master[0]));
    status |= run_operation_case("EOI invalid IRQ16", pic_send_eoi, 16U,
                                 0xffU, 0xffU, false, NULL, 0U);
    status |= run_operation_case("EOI invalid IRQ255", pic_send_eoi, 255U,
                                 0xffU, 0xffU, false, NULL, 0U);
    return status;
}

static int test_spurious_dispatch(void) {
    static const struct io_event real_irq7[] = {
        { IO_WRITE, PIC1_COMMAND, PIC_READ_ISR },
        { IO_READ, PIC1_COMMAND, 0x80U },
        { IO_WRITE, PIC1_COMMAND, PIC_EOI }
    };
    static const struct io_event spurious_irq7[] = {
        { IO_WRITE, PIC1_COMMAND, PIC_READ_ISR },
        { IO_READ, PIC1_COMMAND, 0x00U }
    };
    static const struct io_event real_irq15[] = {
        { IO_WRITE, PIC2_COMMAND, PIC_READ_ISR },
        { IO_READ, PIC2_COMMAND, 0x80U },
        { IO_WRITE, PIC2_COMMAND, PIC_EOI },
        { IO_WRITE, PIC1_COMMAND, PIC_EOI }
    };
    static const struct io_event spurious_irq15[] = {
        { IO_WRITE, PIC2_COMMAND, PIC_READ_ISR },
        { IO_READ, PIC2_COMMAND, 0x00U },
        { IO_WRITE, PIC1_COMMAND, PIC_EOI }
    };

    int status = 0;
    status |= run_dispatch_case("real IRQ7", 39U, 0x80U, 0x00U,
                                real_irq7, sizeof(real_irq7) / sizeof(real_irq7[0]));
    status |= run_dispatch_case("spurious IRQ7", 39U, 0x00U, 0x80U,
                                spurious_irq7,
                                sizeof(spurious_irq7) / sizeof(spurious_irq7[0]));
    status |= run_dispatch_case("real IRQ15", 47U, 0x00U, 0x80U,
                                real_irq15, sizeof(real_irq15) / sizeof(real_irq15[0]));
    status |= run_dispatch_case("spurious IRQ15", 47U, 0x80U, 0x00U,
                                spurious_irq15,
                                sizeof(spurious_irq15) / sizeof(spurious_irq15[0]));
    return status;
}

static int test_spurious_unsupported(void) {
    static const uint8_t unsupported_irqs[] = { 0U, 8U, 16U, 255U };
    int status = 0;

    for (unsigned int index = 0U;
         index < sizeof(unsupported_irqs) / sizeof(unsupported_irqs[0]); ++index) {
        const uint8_t irq = unsupported_irqs[index];
        bool is_spurious = true;
        reset_io(0xffU, 0xffU, 0x80U, 0x80U);

        if (pic_is_spurious_irq(irq, &is_spurious)) {
            fprintf(stderr, "spurious unsupported IRQ%u: expected false status\n", irq);
            status = 1;
        }
        if (!is_spurious) {
            fprintf(stderr, "spurious unsupported IRQ%u: modified result\n", irq);
            status = 1;
        }
        status |= expect_events("spurious unsupported IRQ", NULL, 0U);
    }

    reset_io(0xffU, 0xffU, 0x80U, 0x80U);
    if (pic_is_spurious_irq(7U, NULL)) {
        fputs("spurious NULL result: expected false status\n", stderr);
        status = 1;
    }
    status |= expect_events("spurious NULL result", NULL, 0U);
    return status;
}

#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
static int test_irq0_ownership_host_test(void) {
    int status = 0;

    if (irq0_ioapic_active_host_test_is_active()) {
        fputs("IRQ0 ownership initial false: state was active\n", stderr);
        status = 1;
    }
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fputs("IRQ0 ownership initial false: stray counter was non-zero\n",
              stderr);
        status = 1;
    }

    irq0_ioapic_active_publish(true);
    if (!irq0_ioapic_active_host_test_is_active()) {
        fputs("IRQ0 ownership publish true: state was not active\n", stderr);
        status = 1;
    }

    irq0_ioapic_active_publish(false);
    if (irq0_ioapic_active_host_test_is_active()) {
        fputs("IRQ0 ownership publish false: state was active\n", stderr);
        status = 1;
    }

    irq0_ioapic_active_host_test_reset();
    if (irq0_ioapic_active_host_test_is_active()) {
        fputs("IRQ0 ownership host reset: state was active\n", stderr);
        status = 1;
    }
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fputs("IRQ0 ownership host reset: stray counter was non-zero\n",
              stderr);
        status = 1;
    }

    return status;
}
#endif

#ifdef HFOC_DISPATCH_HOST_TEST
static int run_irq1_eoi_case(
    const char *name,
    uint64_t vector,
    bool expect_pic_eoi,
    bool expect_lapic_eoi)
{
    struct interrupt_frame frame = { .vector = vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&frame);

    int status = 0;
    if (keyboard_calls != 1U) {
        fprintf(stderr, "%s: keyboard handler calls expected 1, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: timer handler calls expected 0, got %u\n",
                name, timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != (expect_lapic_eoi ? 1U : 0U)) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    const bool pic_eoi_seen =
        event_count == 1U &&
        events[0].kind == IO_WRITE &&
        events[0].port == PIC1_COMMAND &&
        events[0].value == PIC_EOI;
    const bool any_pic_io = event_count != 0U;
    if (expect_pic_eoi ? !pic_eoi_seen : any_pic_io) {
        fprintf(stderr, "%s: PIC EOI mismatch\n", name);
        status = 1;
    }
    return status;
}

static int run_irq0_eoi_case(
    const char *name,
    uint64_t vector,
    bool expect_pic_eoi,
    bool expect_lapic_eoi,
    bool expect_timer)
{
    struct interrupt_frame frame = { .vector = vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&frame);

    int status = 0;
    if (timer_calls != (expect_timer ? 1U : 0U)) {
        fprintf(stderr, "%s: timer handler calls mismatch\n", name);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: keyboard handler calls expected 0, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != (expect_lapic_eoi ? 1U : 0U)) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    const bool pic_eoi_seen =
        event_count == 1U &&
        events[0].kind == IO_WRITE &&
        events[0].port == PIC1_COMMAND &&
        events[0].value == PIC_EOI;
    const bool any_pic_io = event_count != 0U;
    if (expect_pic_eoi ? !pic_eoi_seen : any_pic_io) {
        fprintf(stderr, "%s: PIC EOI mismatch\n", name);
        status = 1;
    }
    return status;
}

static int run_irq0_sequence_case(
    const char *name,
    uint64_t first_vector,
    uint64_t second_vector)
{
    struct interrupt_frame first = { .vector = first_vector };
    struct interrupt_frame second = { .vector = second_vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != 1U) {
        fprintf(stderr, "%s: expected one timer call, got %u\n",
                name, timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 1U) {
        fprintf(stderr, "%s: expected one lapic_eoi call, got %u\n",
                name, lapic_eoi_calls);
        status = 1;
    }
    const bool pic_eoi_seen =
        event_count == 1U &&
        events[0].kind == IO_WRITE &&
        events[0].port == PIC1_COMMAND &&
        events[0].value == PIC_EOI;
    if (!pic_eoi_seen) {
        fprintf(stderr, "%s: expected exactly one PIC EOI\n", name);
        status = 1;
    }
    return status;
}

static int expect_only_pic_eois(const char *name,
                                unsigned int expected_count) {
    if (event_count != expected_count) {
        fprintf(stderr, "%s: expected %u PIC EOI events, got %u\n",
                name, expected_count, event_count);
        return 1;
    }
    for (unsigned int index = 0U; index < expected_count; ++index) {
        if (events[index].kind != IO_WRITE ||
            events[index].port != PIC1_COMMAND ||
            events[index].value != PIC_EOI) {
            fprintf(stderr, "%s: event %u is not a master PIC EOI\n",
                    name, index);
            return 1;
        }
    }
    return 0;
}

static int run_irq1_repeat_case(
    const char *name,
    uint64_t vector,
    bool expect_pic_eoi,
    bool expect_lapic_eoi)
{
    struct interrupt_frame first = { .vector = vector };
    struct interrupt_frame second = { .vector = vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (keyboard_calls != 2U) {
        fprintf(stderr, "%s: keyboard handler calls expected 2, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: timer handler calls expected 0, got %u\n",
                name, timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != (expect_lapic_eoi ? 2U : 0U)) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expect_pic_eoi ? 2U : 0U);
    return status;
}

static int run_irq0_repeat_case(
    const char *name,
    uint64_t vector,
    bool expect_pic_eoi,
    bool expect_lapic_eoi,
    bool expect_timer)
{
    struct interrupt_frame first = { .vector = vector };
    struct interrupt_frame second = { .vector = vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != (expect_timer ? 2U : 0U)) {
        fprintf(stderr, "%s: timer handler calls mismatch\n", name);
        status = 1;
    }
    if (lapic_eoi_calls != (expect_lapic_eoi ? 2U : 0U)) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: keyboard handler calls expected 0, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expect_pic_eoi ? 2U : 0U);
    return status;
}

static int run_irq1_sequence_case(
    const char *name,
    uint64_t first_vector,
    uint64_t second_vector)
{
    struct interrupt_frame first = { .vector = first_vector };
    struct interrupt_frame second = { .vector = second_vector };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (keyboard_calls != 2U) {
        fprintf(stderr, "%s: keyboard handler calls expected 2, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: timer handler calls expected 0, got %u\n",
                name, timer_calls);
        status = 1;
    }
    const unsigned int expected_lapic =
        (first_vector == 0x41U ? 1U : 0U) +
        (second_vector == 0x41U ? 1U : 0U);
    if (lapic_eoi_calls != expected_lapic) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    const unsigned int expected_pic =
        (first_vector == 0x21U ? 1U : 0U) +
        (second_vector == 0x21U ? 1U : 0U);
    status |= expect_only_pic_eois(name, expected_pic);
    return status;
}

static int run_spurious_eoi_case(void) {
    struct interrupt_frame frame = { .vector = 0xEFU };
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&frame);

    int status = 0;
    if (keyboard_calls != 0U) {
        fprintf(stderr, "spurious 0xEF: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "spurious 0xEF: timer calls expected 0, got %u\n",
                timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "spurious 0xEF: lapic_eoi calls expected 0, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("spurious 0xEF", 0U);
    return status;
}

#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
static int run_irq0_ownership_single_case(
    const char *name,
    uint64_t vector,
    bool active,
    bool expect_timer,
    bool expect_pic_eoi,
    bool expect_lapic_eoi)
{
    struct interrupt_frame frame = { .vector = vector };
    const uint64_t expected_stray =
        active && vector == 0x20U ? 1U : 0U;

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(active);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&frame);

    int status = 0;
    if (timer_calls != (expect_timer ? 1U : 0U)) {
        fprintf(stderr, "%s: timer handler calls mismatch\n", name);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: keyboard handler calls expected 0, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != (expect_lapic_eoi ? 1U : 0U)) {
        fprintf(stderr, "%s: lapic_eoi calls mismatch\n", name);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expect_pic_eoi ? 1U : 0U);
    if (irq0_stray_pic_count_host_test_value() != expected_stray) {
        fprintf(stderr, "%s: stray counter mismatch, expected %llu, got %llu\n",
                name, (unsigned long long)expected_stray,
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_irq0_stray_repeat_case(void) {
    struct interrupt_frame first = { .vector = 0x20U };
    struct interrupt_frame second = { .vector = 0x20U };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(true);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != 0U) {
        fprintf(stderr, "IRQ0 stray repeat counter: timer calls expected 0, got %u\n",
                timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "IRQ0 stray repeat counter: lapic_eoi calls expected 0, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "IRQ0 stray repeat counter: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("IRQ0 stray repeat counter", 2U);
    if (irq0_stray_pic_count_host_test_value() != 2U) {
        fprintf(stderr, "IRQ0 stray repeat counter: expected 2, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_irq0_active_to_fallback_case(void) {
    struct interrupt_frame first = { .vector = 0x20U };
    struct interrupt_frame second = { .vector = 0x20U };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(true);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    irq0_ioapic_active_publish(false);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != 1U) {
        fprintf(stderr, "IRQ0 active to fallback transition: timer calls expected 1, got %u\n",
                timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "IRQ0 active to fallback transition: lapic_eoi calls expected 0, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "IRQ0 active to fallback transition: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("IRQ0 active to fallback transition", 2U);
    if (irq0_stray_pic_count_host_test_value() != 1U) {
        fprintf(stderr, "IRQ0 active to fallback transition: stray counter expected 1, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_irq0_no_ioapic_reread_case(void) {
    static const uint64_t vectors[] = { 0x20U, 0x40U, 0x21U, 0x41U, 0xEFU };
    int status = 0;

    /* IRQ0 dispatcher no IOAPIC reread: the mock below exits if queried. */
    for (unsigned int mode = 0U; mode < 2U; ++mode) {
        irq0_ioapic_active_host_test_reset();
        irq0_ioapic_active_publish(mode == 1U);
        for (unsigned int index = 0U;
             index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
            struct interrupt_frame frame = { .vector = vectors[index] };
            reset_io(0xffU, 0xffU, 0U, 0U);
            keyboard_calls = 0U;
            lapic_eoi_calls = 0U;
            timer_calls = 0U;
            interrupt_dispatch(&frame);
        }
    }

    irq0_ioapic_active_host_test_reset();
    return status;
}

static int test_irq0_ownership_dispatch_paths(void) {
    int status = 0;
    status |= run_irq0_ownership_single_case(
        "IRQ0 fallback 0x20 timer owner", 0x20U, false, true, true, false);
    status |= run_irq0_ownership_single_case(
        "IRQ0 fallback 0x40 shadow", 0x40U, false, false, false, true);
    status |= run_irq0_ownership_single_case(
        "IRQ0 active 0x40 timer owner", 0x40U, true, true, false, true);
    status |= run_irq0_ownership_single_case(
        "IRQ0 active stray 0x20", 0x20U, true, false, true, false);
    status |= run_irq0_stray_repeat_case();
    status |= run_irq0_active_to_fallback_case();
    status |= run_irq0_no_ioapic_reread_case();
    irq0_ioapic_active_host_test_reset();
    return status;
}
#endif

#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
static int run_exact_negative_case(
    const char *name,
    uint64_t vector,
    bool active,
    unsigned int expected_timer,
    unsigned int expected_keyboard,
    unsigned int expected_lapic_eoi,
    unsigned int expected_pic1_eoi,
    uint64_t expected_stray)
{
    struct interrupt_frame frame = { .vector = vector };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(active);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&frame);

    int status = 0;
    if (timer_calls != expected_timer) {
        fprintf(stderr, "%s: timer calls expected %u, got %u\n",
                name, expected_timer, timer_calls);
        status = 1;
    }
    if (keyboard_calls != expected_keyboard) {
        fprintf(stderr, "%s: keyboard calls expected %u, got %u\n",
                name, expected_keyboard, keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != expected_lapic_eoi) {
        fprintf(stderr, "%s: lapic_eoi calls expected %u, got %u\n",
                name, expected_lapic_eoi, lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expected_pic1_eoi);
    if (irq0_stray_pic_count_host_test_value() != expected_stray) {
        fprintf(stderr, "%s: stray counter expected %llu, got %llu\n",
                name, (unsigned long long)expected_stray,
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_full_sequence_case(
    const char *name,
    bool active,
    uint64_t first_vector,
    uint64_t second_vector,
    unsigned int expected_timer,
    unsigned int expected_pic_eoi,
    unsigned int expected_lapic_eoi,
    uint64_t expected_stray)
{
    struct interrupt_frame first = { .vector = first_vector };
    struct interrupt_frame second = { .vector = second_vector };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(active);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != expected_timer) {
        fprintf(stderr, "%s: timer calls expected %u, got %u\n",
                name, expected_timer, timer_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: keyboard calls expected 0, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != expected_lapic_eoi) {
        fprintf(stderr, "%s: lapic_eoi calls expected %u, got %u\n",
                name, expected_lapic_eoi, lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expected_pic_eoi);
    if (irq0_stray_pic_count_host_test_value() != expected_stray) {
        fprintf(stderr, "%s: stray counter expected %llu, got %llu\n",
                name, (unsigned long long)expected_stray,
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_full_repeat_case(
    const char *name,
    bool active,
    uint64_t vector,
    unsigned int expected_timer,
    unsigned int expected_pic_eoi,
    unsigned int expected_lapic_eoi,
    uint64_t expected_stray)
{
    struct interrupt_frame first = { .vector = vector };
    struct interrupt_frame second = { .vector = vector };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(active);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&first);
    interrupt_dispatch(&second);

    int status = 0;
    if (timer_calls != expected_timer) {
        fprintf(stderr, "%s: timer calls expected %u, got %u\n",
                name, expected_timer, timer_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: keyboard calls expected 0, got %u\n",
                name, keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != expected_lapic_eoi) {
        fprintf(stderr, "%s: lapic_eoi calls expected %u, got %u\n",
                name, expected_lapic_eoi, lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois(name, expected_pic_eoi);
    if (irq0_stray_pic_count_host_test_value() != expected_stray) {
        fprintf(stderr, "%s: stray counter expected %llu, got %llu\n",
                name, (unsigned long long)expected_stray,
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_irq0_false_to_true_case(void) {
    struct interrupt_frame shadow = { .vector = 0x40U };
    struct interrupt_frame active = { .vector = 0x40U };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(false);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&shadow);

    int status = 0;
    if (timer_calls != 0U) {
        fprintf(stderr, "IRQ0 false to true 0x40 shadow: timer calls expected 0, got %u\n",
                timer_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "IRQ0 false to true 0x40 shadow: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 1U) {
        fprintf(stderr, "IRQ0 false to true 0x40 shadow: lapic_eoi calls expected 1, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("IRQ0 false to true 0x40 shadow", 0U);
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fprintf(stderr, "IRQ0 false to true 0x40 shadow: stray counter expected 0, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }

    irq0_ioapic_active_publish(true);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&active);

    if (timer_calls != 1U) {
        fprintf(stderr, "IRQ0 false to true 0x40 owner: timer calls expected 1, got %u\n",
                timer_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "IRQ0 false to true 0x40 owner: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 1U) {
        fprintf(stderr, "IRQ0 false to true 0x40 owner: lapic_eoi calls expected 1, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("IRQ0 false to true 0x40 owner", 0U);
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fprintf(stderr, "IRQ0 false to true 0x40 owner: stray counter expected 0, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int run_irq0_reset_seam_case(void) {
    struct interrupt_frame stray = { .vector = 0x20U };
    struct interrupt_frame fallback = { .vector = 0x20U };

    irq0_ioapic_active_host_test_reset();
    irq0_ioapic_active_publish(true);
    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&stray);

    int status = 0;
    if (irq0_stray_pic_count_host_test_value() != 1U) {
        fprintf(stderr, "IRQ0 reset seam no leakage: counter before reset expected 1, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }

    irq0_ioapic_active_host_test_reset();
    if (irq0_ioapic_active_host_test_is_active()) {
        fputs("IRQ0 reset seam no leakage: state remained active\n", stderr);
        status = 1;
    }
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fputs("IRQ0 reset seam no leakage: counter not reset\n", stderr);
        status = 1;
    }

    reset_io(0xffU, 0xffU, 0U, 0U);
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    timer_calls = 0U;
    interrupt_dispatch(&fallback);
    if (timer_calls != 1U) {
        fprintf(stderr, "IRQ0 reset seam no leakage: fallback timer calls expected 1, got %u\n",
                timer_calls);
        status = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "IRQ0 reset seam no leakage: lapic_eoi calls expected 0, got %u\n",
                lapic_eoi_calls);
        status = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "IRQ0 reset seam no leakage: keyboard calls expected 0, got %u\n",
                keyboard_calls);
        status = 1;
    }
    status |= expect_only_pic_eois("IRQ0 reset seam no leakage", 1U);
    if (irq0_stray_pic_count_host_test_value() != 0U) {
        fprintf(stderr, "IRQ0 reset seam no leakage: counter leaked into fallback, got %llu\n",
                (unsigned long long)irq0_stray_pic_count_host_test_value());
        status = 1;
    }
    return status;
}

static int test_irq0_full_dispatch_matrix(void) {
    int status = 0;
    status |= run_exact_negative_case(
        "IRQ0 negative fallback 0x20", 0x20U, false, 1U, 0U, 0U, 1U, 0U);
    status |= run_exact_negative_case(
        "IRQ0 negative active stray 0x20", 0x20U, true, 0U, 0U, 0U, 1U, 1U);
    status |= run_exact_negative_case(
        "IRQ0 negative fallback 0x40", 0x40U, false, 0U, 0U, 1U, 0U, 0U);
    status |= run_exact_negative_case(
        "IRQ0 negative active 0x40", 0x40U, true, 1U, 0U, 1U, 0U, 0U);
    status |= run_exact_negative_case(
        "IRQ1 negative 0x21 fallback", 0x21U, false, 0U, 1U, 0U, 1U, 0U);
    status |= run_exact_negative_case(
        "IRQ1 negative 0x21 active", 0x21U, true, 0U, 1U, 0U, 1U, 0U);
    status |= run_exact_negative_case(
        "IRQ1 negative 0x41 fallback", 0x41U, false, 0U, 1U, 1U, 0U, 0U);
    status |= run_exact_negative_case(
        "IRQ1 negative 0x41 active", 0x41U, true, 0U, 1U, 1U, 0U, 0U);
    status |= run_exact_negative_case(
        "IRQ spurious negative fallback", 0xEFU, false, 0U, 0U, 0U, 0U, 0U);
    status |= run_exact_negative_case(
        "IRQ spurious negative active", 0xEFU, true, 0U, 0U, 0U, 0U, 0U);

    status |= run_full_repeat_case(
        "IRQ0 fallback repeat 0x20", false, 0x20U, 2U, 2U, 0U, 0U);
    status |= run_full_repeat_case(
        "IRQ0 active repeat 0x40", true, 0x40U, 2U, 0U, 2U, 0U);
    status |= run_full_repeat_case(
        "IRQ0 active repeat stray 0x20", true, 0x20U, 0U, 2U, 0U, 2U);

    status |= run_full_sequence_case(
        "IRQ0 success sequence 0x20 then 0x40", true, 0x20U, 0x40U,
        1U, 1U, 1U, 1U);
    status |= run_full_sequence_case(
        "IRQ0 success sequence 0x40 then 0x20", true, 0x40U, 0x20U,
        1U, 1U, 1U, 1U);
    status |= run_full_sequence_case(
        "IRQ0 fallback sequence 0x20 then 0x40", false, 0x20U, 0x40U,
        1U, 1U, 1U, 0U);
    status |= run_full_sequence_case(
        "IRQ0 fallback sequence 0x40 then 0x20", false, 0x40U, 0x20U,
        1U, 1U, 1U, 0U);

    status |= run_irq0_false_to_true_case();
    status |= run_irq0_reset_seam_case();
    irq0_ioapic_active_host_test_reset();
    return status;
}
#endif

#ifdef HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST
bool pic_irq1_boot_policy_host_test_apply(
    enum ioapic_status route_status,
    bool routed);
bool pic_irq1_boot_policy_host_test_reset(void);
bool pic_irq0_boot_policy_host_test_apply(
    enum ioapic_status route_status,
    bool routed);
bool pic_irq01_boot_policy_host_test_apply(
    enum ioapic_status irq1_status,
    bool irq1_routed,
    enum ioapic_status irq0_status,
    bool irq0_routed);

static int run_irq1_boot_policy_case(
    const char *name,
    enum ioapic_status route_status,
    bool routed,
    uint8_t initial_master_mask,
    uint8_t expected_master_mask,
    const struct io_event *expected,
    unsigned int expected_count)
{
    pic_irq1_boot_policy_host_test_reset();
    reset_io(initial_master_mask, 0xffU, 0U, 0U);
    timer_calls = 0U;

    const bool status =
        pic_irq1_boot_policy_host_test_apply(route_status, routed);
    int result = 0;
    if (!status) {
        fprintf(stderr, "%s: expected true policy status\n", name);
        result = 1;
    }
    const unsigned int events_after_first = event_count;
    const bool repeat_status =
        pic_irq1_boot_policy_host_test_apply(route_status, routed);
    if (!repeat_status) {
        fprintf(stderr, "%s: expected true repeated policy status\n", name);
        result = 1;
    }
    if (event_count != events_after_first) {
        fprintf(stderr,
                "%s: repeated policy added %u I/O events, expected zero\n",
                name, event_count - events_after_first);
        result = 1;
    }
    if ((master_mask & 0x01U) != 0U) {
        fprintf(stderr, "%s: master bit 0 became masked (0x%02x)\n",
                name, master_mask);
        result = 1;
    }
    if (master_mask != expected_master_mask) {
        fprintf(stderr, "%s: expected master mask 0x%02x, got 0x%02x\n",
                name, expected_master_mask, master_mask);
        result = 1;
    }
    if (slave_mask != 0xffU) {
        fprintf(stderr, "%s: expected slave mask 0xff, got 0x%02x\n",
                name, slave_mask);
        result = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: expected zero timer calls, got %u\n",
                name, timer_calls);
        result = 1;
    }
    result |= expect_events(name, expected, expected_count);
    return result;
}

static int test_irq1_boot_policy(void) {
    static const struct io_event success_transition[] = {
        { IO_READ, PIC1_DATA, 0xfcU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };
    static const struct io_event success_idempotence[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };
    static const struct io_event fallback_transition[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfcU }
    };
    static const struct io_event fallback_idempotence[] = {
        { IO_READ, PIC1_DATA, 0xfcU },
        { IO_WRITE, PIC1_DATA, 0xfcU }
    };

    int status = 0;
    status |= run_irq1_boot_policy_case(
        "IRQ1 success transition", IOAPIC_OK, true, 0xfcU, 0xfeU,
        success_transition,
        sizeof(success_transition) / sizeof(success_transition[0]));
    status |= run_irq1_boot_policy_case(
        "IRQ1 success idempotence", IOAPIC_OK, true, 0xfeU, 0xfeU,
        success_idempotence,
        sizeof(success_idempotence) / sizeof(success_idempotence[0]));
    status |= run_irq1_boot_policy_case(
        "IRQ1 fallback transition", IOAPIC_ROUTE_FAILED, false,
        0xfeU, 0xfcU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq1_boot_policy_case(
        "IRQ1 status OK but not routed", IOAPIC_OK, false,
        0xfeU, 0xfcU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq1_boot_policy_case(
        "IRQ1 route failure but routed true", IOAPIC_ROUTE_FAILED, true,
        0xfeU, 0xfcU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq1_boot_policy_case(
        "IRQ1 fallback idempotence", IOAPIC_OK, false, 0xfcU, 0xfcU,
        fallback_idempotence,
        sizeof(fallback_idempotence) / sizeof(fallback_idempotence[0]));
    return status;
}

static int run_irq0_boot_policy_case(
    const char *name,
    enum ioapic_status route_status,
    bool routed,
    uint8_t initial_master_mask,
    uint8_t expected_master_mask,
    const struct io_event *expected,
    unsigned int expected_count)
{
    reset_io(initial_master_mask, 0xffU, 0U, 0U);
    timer_calls = 0U;
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    interrupt_dispatch_calls = 0U;

    const bool status =
        pic_irq0_boot_policy_host_test_apply(route_status, routed);
    int result = 0;
    if (!status) {
        fprintf(stderr, "%s: expected true policy status\n", name);
        result = 1;
    }
    if (master_mask != expected_master_mask) {
        fprintf(stderr, "%s: expected master mask 0x%02x, got 0x%02x\n",
                name, expected_master_mask, master_mask);
        result = 1;
    }
    if (slave_mask != 0xffU) {
        fprintf(stderr, "%s: expected slave mask 0xff, got 0x%02x\n",
                name, slave_mask);
        result = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: policy timer calls expected 0, got %u\n",
                name, timer_calls);
        result = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: policy keyboard calls expected 0, got %u\n",
                name, keyboard_calls);
        result = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "%s: policy lapic_eoi calls expected 0, got %u\n",
                name, lapic_eoi_calls);
        result = 1;
    }
    if (interrupt_dispatch_calls != 0U) {
        fprintf(stderr, "%s: interrupt_dispatch calls expected 0, got %u\n",
                name, interrupt_dispatch_calls);
        result = 1;
    }
    result |= expect_events(name, expected, expected_count);
    return result;
}

static int test_irq0_boot_policy(void) {
    static const struct io_event success_transition[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event fallback_transition[] = {
        { IO_READ, PIC1_DATA, 0xffU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };
    static const struct io_event fallback_retained[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };

    int status = 0;
    status |= run_irq0_boot_policy_case(
        "IRQ0 boot policy success", IOAPIC_OK, true, 0xfeU, 0xffU,
        success_transition,
        sizeof(success_transition) / sizeof(success_transition[0]));
    status |= run_irq0_boot_policy_case(
        "IRQ0 boot policy status OK but not routed", IOAPIC_OK, false,
        0xffU, 0xfeU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq0_boot_policy_case(
        "IRQ0 boot policy route failure", IOAPIC_ROUTE_FAILED, false,
        0xffU, 0xfeU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq0_boot_policy_case(
        "IRQ0 boot policy route failure but routed true",
        IOAPIC_ROUTE_FAILED, true, 0xffU, 0xfeU, fallback_transition,
        sizeof(fallback_transition) / sizeof(fallback_transition[0]));
    status |= run_irq0_boot_policy_case(
        "IRQ0 boot policy fallback retained", IOAPIC_OK, false,
        0xfeU, 0xfeU, fallback_retained,
        sizeof(fallback_retained) / sizeof(fallback_retained[0]));
    return status;
}

static int run_irq01_boot_policy_case(
    const char *name,
    enum ioapic_status irq1_status,
    bool irq1_routed,
    enum ioapic_status irq0_status,
    bool irq0_routed,
    bool expected_irq0_ready,
    uint8_t initial_master_mask,
    uint8_t expected_master_mask,
    const struct io_event *expected,
    unsigned int expected_count)
{
    reset_io(initial_master_mask, 0xffU, 0U, 0U);
    timer_calls = 0U;
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    interrupt_dispatch_calls = 0U;

    const bool irq0_ready =
        pic_irq01_boot_policy_host_test_apply(
            irq1_status, irq1_routed, irq0_status, irq0_routed);
    int result = 0;
    if (irq0_ready != expected_irq0_ready) {
        fprintf(stderr, "%s: expected irq0_ready %s, got %s\n",
                name, expected_irq0_ready ? "true" : "false",
                irq0_ready ? "true" : "false");
        result = 1;
    }
    if (master_mask != expected_master_mask) {
        fprintf(stderr, "%s: expected master mask 0x%02x, got 0x%02x\n",
                name, expected_master_mask, master_mask);
        result = 1;
    }
    if (slave_mask != 0xffU) {
        fprintf(stderr, "%s: expected slave mask 0xff, got 0x%02x\n",
                name, slave_mask);
        result = 1;
    }
    if (timer_calls != 0U) {
        fprintf(stderr, "%s: policy timer calls expected 0, got %u\n",
                name, timer_calls);
        result = 1;
    }
    if (keyboard_calls != 0U) {
        fprintf(stderr, "%s: policy keyboard calls expected 0, got %u\n",
                name, keyboard_calls);
        result = 1;
    }
    if (lapic_eoi_calls != 0U) {
        fprintf(stderr, "%s: policy lapic_eoi calls expected 0, got %u\n",
                name, lapic_eoi_calls);
        result = 1;
    }
    if (interrupt_dispatch_calls != 0U) {
        fprintf(stderr, "%s: interrupt_dispatch calls expected 0, got %u\n",
                name, interrupt_dispatch_calls);
        result = 1;
    }
    result |= expect_events(name, expected, expected_count);
    return result;
}

static int test_irq01_boot_policy(void) {
    static const struct io_event ok_ok_true_true[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfeU },
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xffU }
    };
    static const struct io_event ok_ok_true_false[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfeU },
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfeU }
    };
    static const struct io_event fail_ok_false_false[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfcU },
        { IO_READ, PIC1_DATA, 0xfcU },
        { IO_WRITE, PIC1_DATA, 0xfcU }
    };
    static const struct io_event fail_ok_false_true[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfcU },
        { IO_READ, PIC1_DATA, 0xfcU },
        { IO_WRITE, PIC1_DATA, 0xfdU }
    };
    static const struct io_event fail_fail_false_false[] = {
        { IO_READ, PIC1_DATA, 0xfeU },
        { IO_WRITE, PIC1_DATA, 0xfcU },
        { IO_READ, PIC1_DATA, 0xfcU },
        { IO_WRITE, PIC1_DATA, 0xfcU }
    };

    int status = 0;
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy OK OK true true",
        IOAPIC_OK, true, IOAPIC_OK, true, true,
        0xfeU, 0xffU, ok_ok_true_true,
        sizeof(ok_ok_true_true) / sizeof(ok_ok_true_true[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy OK OK true false",
        IOAPIC_OK, true, IOAPIC_OK, false, false,
        0xfeU, 0xfeU, ok_ok_true_false,
        sizeof(ok_ok_true_false) / sizeof(ok_ok_true_false[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy OK fail true false",
        IOAPIC_OK, true, IOAPIC_ROUTE_FAILED, false, false,
        0xfeU, 0xfeU, ok_ok_true_false,
        sizeof(ok_ok_true_false) / sizeof(ok_ok_true_false[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy fail OK false false",
        IOAPIC_ROUTE_FAILED, false, IOAPIC_OK, false, false,
        0xfeU, 0xfcU, fail_ok_false_false,
        sizeof(fail_ok_false_false) / sizeof(fail_ok_false_false[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy fail OK false true",
        IOAPIC_ROUTE_FAILED, false, IOAPIC_OK, true, true,
        0xfeU, 0xfdU, fail_ok_false_true,
        sizeof(fail_ok_false_true) / sizeof(fail_ok_false_true[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy OK OK false false",
        IOAPIC_OK, false, IOAPIC_OK, false, false,
        0xfeU, 0xfcU, fail_ok_false_false,
        sizeof(fail_ok_false_false) / sizeof(fail_ok_false_false[0]));
    status |= run_irq01_boot_policy_case(
        "IRQ01 boot policy fail fail false false",
        IOAPIC_ROUTE_FAILED, false, IOAPIC_ROUTE_FAILED, false, false,
        0xfeU, 0xfcU, fail_fail_false_false,
        sizeof(fail_fail_false_false) / sizeof(fail_fail_false_false[0]));
    return status;
}
#endif

static int test_irq1_eoi_paths(void) {
    int status = 0;
    status |= run_irq1_eoi_case("IRQ1 PIC 0x21", 0x21U, true, false);
    status |= run_irq1_eoi_case("IRQ1 IOAPIC 0x41", 0x41U, false, true);
    status |= run_irq1_sequence_case("IRQ1 sequence 0x21 then 0x41",
                                     0x21U, 0x41U);
    status |= run_irq1_sequence_case("IRQ1 sequence 0x41 then 0x21",
                                     0x41U, 0x21U);
    status |= run_irq1_repeat_case("IRQ1 repeat 0x21", 0x21U, true, false);
    status |= run_irq1_repeat_case("IRQ1 repeat 0x41", 0x41U, false, true);
    return status;
}

static int test_irq0_eoi_paths(void) {
    int status = 0;
    status |= run_irq0_eoi_case("IRQ0 PIC 0x20", 0x20U,
                                true, false, true);
    status |= run_irq0_eoi_case("IRQ0 IOAPIC 0x40", 0x40U,
                                false, true, false);
    status |= run_irq0_sequence_case("IRQ0 sequence 0x20 then 0x40",
                                     0x20U, 0x40U);
    status |= run_irq0_sequence_case("IRQ0 sequence 0x40 then 0x20",
                                     0x40U, 0x20U);
    status |= run_irq0_repeat_case("IRQ0 repeat 0x20", 0x20U,
                                   true, false, true);
    status |= run_irq0_repeat_case("IRQ0 repeat 0x40", 0x40U,
                                   false, true, false);
    return status;
}

static int test_spurious_eoi_paths(void) {
    return run_spurious_eoi_case();
}
#endif

int main(void) {
    int status = 0;
    status |= test_unmask();
    status |= test_mask();
    status |= test_eoi();
    status |= test_spurious_dispatch();
    status |= test_spurious_unsupported();
#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
    status |= test_irq0_ownership_host_test();
#endif
#ifdef HFOC_DISPATCH_HOST_TEST
    status |= test_irq1_eoi_paths();
    status |= test_irq0_eoi_paths();
    status |= test_spurious_eoi_paths();
#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST
    status |= test_irq0_ownership_dispatch_paths();
    status |= test_irq0_full_dispatch_matrix();
#endif
#endif
#ifdef HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST
    status |= test_irq1_boot_policy();
    status |= test_irq0_boot_policy();
    status |= test_irq01_boot_policy();
#endif
    if (status != 0) {
        return status;
    }

    puts("PIC spurious IRQ dispatch: OK");
    puts("PIC regression suite: OK");
    return 0;
}
