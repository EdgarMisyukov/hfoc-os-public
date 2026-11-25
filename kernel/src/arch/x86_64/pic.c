#include "arch/x86_64/pic.h"
#include "arch/x86_64/io.h"

#include <stddef.h>

#ifdef HFOC_PIC_HOST_TEST
uint8_t host_pic_inb(uint16_t port);
void host_pic_outb(uint16_t port, uint8_t value);
#define inb(port) host_pic_inb(port)
#define outb(port, value) host_pic_outb(port, value)

static bool pic_disable_rollback_verified;

bool pic_disable_host_test_rollback_verified(void) {
    return pic_disable_rollback_verified;
}
#endif

#ifdef HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST
#include "ioapic.h"

static bool pic_irq1_policy_state_valid;
static bool pic_irq1_policy_masked;

bool pic_irq1_boot_policy_host_test_reset(void) {
    pic_irq1_policy_state_valid = false;
    pic_irq1_policy_masked = false;
    return true;
}

bool pic_irq1_boot_policy_host_test_apply(
    enum ioapic_status route_status,
    bool routed)
{
    const bool desired_masked = route_status == IOAPIC_OK && routed;
    if (pic_irq1_policy_state_valid &&
        pic_irq1_policy_masked == desired_masked) {
        return true;
    }

    const bool applied =
        desired_masked ? pic_mask(1U) : pic_unmask(1U);
    if (applied) {
        pic_irq1_policy_masked = desired_masked;
        pic_irq1_policy_state_valid = true;
    }
    return applied;
}

bool pic_irq0_boot_policy_host_test_apply(
    enum ioapic_status route_status,
    bool routed)
{
    const bool ready = route_status == IOAPIC_OK && routed;
    return ready ? pic_mask(0U) : pic_unmask(0U);
}

bool pic_irq01_boot_policy_host_test_apply(
    enum ioapic_status irq1_status,
    bool irq1_routed,
    enum ioapic_status irq0_status,
    bool irq0_routed)
{
    const bool irq1_ready =
        irq1_status == IOAPIC_OK && irq1_routed;
    const bool irq0_ready =
        irq0_status == IOAPIC_OK && irq0_routed;
    if (irq1_ready) {
        pic_mask(1U);
    } else {
        pic_unmask(1U);
    }
    if (irq0_ready) {
        pic_mask(0U);
    } else {
        pic_unmask(0U);
    }
    return irq0_ready;
}
#endif

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA    0xa1
#define PIC_EOI      0x20
#define PIC_READ_ISR 0x0b
#define PIC_IRQ_COUNT 16U

static uint8_t pic_read_isr(uint16_t command_port) {
    outb(command_port, PIC_READ_ISR);
    return inb(command_port);
}

static bool pic_irq_data_port(uint8_t irq, uint16_t *port, uint8_t *line) {
    if (irq >= PIC_IRQ_COUNT) {
        return false;
    }
    if (irq < 8U) {
        *port = PIC1_DATA;
        *line = irq;
    } else {
        *port = PIC2_DATA;
        *line = (uint8_t)(irq - 8U);
    }
    return true;
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
}

void pic_init(void) {
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Start from a deterministic state. Only the explicitly requested IRQ
       lines are enabled later by pic_unmask(). */
    pic_mask_all();
}

bool pic_mask(uint8_t irq) {
    uint16_t port;
    uint8_t line;

    if (!pic_irq_data_port(irq, &port, &line)) {
        return false;
    }
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask | (1U << line)));
    return true;
}

bool pic_unmask(uint8_t irq) {
    if (irq == 2U) {
        return false;
    }

    uint16_t port;
    uint8_t line;

    if (!pic_irq_data_port(irq, &port, &line)) {
        return false;
    }
    if (irq >= 8U) {
        uint8_t master = inb(PIC1_DATA);
        outb(PIC1_DATA, (uint8_t)(master & ~(1U << 2U)));
    }
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask & ~(1U << line)));
    return true;
}

bool pic_send_eoi(uint8_t irq) {
    if (irq >= PIC_IRQ_COUNT) {
        return false;
    }
    if (irq >= 8U) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
    return true;
}

bool pic_is_spurious_irq(uint8_t irq, bool *is_spurious) {
    if (is_spurious == 0 || (irq != 7U && irq != 15U)) {
        return false;
    }
    const uint16_t command_port = irq == 7U ? PIC1_COMMAND : PIC2_COMMAND;
    *is_spurious = (pic_read_isr(command_port) & (1U << 7U)) == 0U;
    return true;
}

bool pic_irq0_mask_save(uint8_t *out_previous_master_mask) {
    if (out_previous_master_mask == NULL) {
        return false;
    }
    const uint8_t previous = inb(PIC1_DATA);
    *out_previous_master_mask = previous;
    outb(PIC1_DATA, (uint8_t)(previous | 1U));
    if ((inb(PIC1_DATA) & 1U) == 0U) {
        (void)pic_master_mask_restore(previous);
        return false;
    }
    return true;
}

bool pic_master_mask_restore(uint8_t previous_master_mask) {
    outb(PIC1_DATA, previous_master_mask);
    return inb(PIC1_DATA) == previous_master_mask;
}

bool pic_disable_transactional(void) {
    const uint8_t previous_master = inb(PIC1_DATA);
    const uint8_t previous_slave = inb(PIC2_DATA);

    outb(PIC2_DATA, 0xff);
    outb(PIC1_DATA, 0xff);

    if (inb(PIC1_DATA) != 0xffU || inb(PIC2_DATA) != 0xffU) {
        outb(PIC1_DATA, previous_master);
        outb(PIC2_DATA, previous_slave);
        const bool rollback_verified =
            inb(PIC1_DATA) == previous_master &&
            inb(PIC2_DATA) == previous_slave;
#ifdef HFOC_PIC_HOST_TEST
        pic_disable_rollback_verified = rollback_verified;
#else
        (void)rollback_verified;
#endif
        return false;
    }
#ifdef HFOC_PIC_HOST_TEST
    pic_disable_rollback_verified = true;
#endif
    return true;
}

uint8_t pic_master_mask(void) {
    return inb(PIC1_DATA);
}

uint8_t pic_slave_mask(void) {
    return inb(PIC2_DATA);
}
