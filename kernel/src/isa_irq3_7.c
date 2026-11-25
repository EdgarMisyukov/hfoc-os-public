#include "isa_irq3_7.h"

#include "arch/x86_64/pic.h"
#include "ioapic.h"
#include "lapic.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>

#ifdef HFOC_ISA_IRQ3_7_QEMU_TEST
struct isa_irq3_7_uart_port {
    uint8_t irq;
    uint16_t iobase;
};

static const char uart_irq3_marker[] =
    "[UART] irq=3 vector=0x44 char=";

static const struct isa_irq3_7_uart_port uart_ports[] = {
    {3U, 0x2f8U},
    {4U, 0x3f8U},
    {5U, 0x3e8U},
    {6U, 0x2e8U},
    {7U, 0x278U}
};

static inline void uart_outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t uart_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint16_t uart_iobase_for_irq(uint8_t irq) {
    for (size_t index = 0U;
         index < sizeof(uart_ports) / sizeof(uart_ports[0]); ++index) {
        if (uart_ports[index].irq == irq) {
            return uart_ports[index].iobase;
        }
    }
    return 0U;
}

static void uart_enable_receive_interrupt(uint16_t iobase) {
    uart_outb((uint16_t)(iobase + 1U), 0x00U);
    uart_outb((uint16_t)(iobase + 3U), 0x80U);
    uart_outb(iobase, 0x03U);
    uart_outb((uint16_t)(iobase + 1U), 0x00U);
    uart_outb((uint16_t)(iobase + 3U), 0x03U);
    uart_outb((uint16_t)(iobase + 2U), 0xc7U);
    uart_outb((uint16_t)(iobase + 4U), 0x0bU);
    uart_outb((uint16_t)(iobase + 1U), 0x01U);
}

static void isa_irq3_7_qemu_test_init(void) {
    for (size_t index = 0U;
         index < sizeof(uart_ports) / sizeof(uart_ports[0]); ++index) {
        uart_enable_receive_interrupt(uart_ports[index].iobase);
    }
    serial_write("[ISA ] IRQ3-7 UART receive interrupts enabled.\n");
}

static void serial_write_hex_u8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_putc('0');
    serial_putc('x');
    serial_putc(digits[(value >> 4U) & 0x0FU]);
    serial_putc(digits[value & 0x0FU]);
}
#endif

#ifdef HFOC_ISA_IRQ3_7_HOST_TEST
enum ioapic_status host_ioapic_route_isa_irq(uint8_t irq, uint8_t vector);
bool host_ioapic_is_isa_irq_routed(uint8_t irq);
bool host_pic_mask(uint8_t irq);
bool host_pic_unmask(uint8_t irq);
#define ioapic_route_isa_irq host_ioapic_route_isa_irq
#define ioapic_is_isa_irq_routed host_ioapic_is_isa_irq_routed
#define pic_mask host_pic_mask
#define pic_unmask host_pic_unmask
#endif

static const uint8_t irq3_7[] = {
    3U, 4U, 5U, 6U, 7U
};

static bool is_irq3_7(uint8_t irq) {
    return irq >= ISA_IRQ3_7_FIRST && irq <= ISA_IRQ3_7_LAST;
}

static uint8_t expected_vector(uint8_t irq) {
    return (uint8_t)(0x41U + irq);
}

static void write_masked_marker(uint8_t irq) {
    switch (irq) {
    case 3U:
        serial_write("[ISA ] IRQ3 masked; IOAPIC vector=0x44 active.\n");
        break;
    case 4U:
        serial_write("[ISA ] IRQ4 masked; IOAPIC vector=0x45 active.\n");
        break;
    case 5U:
        serial_write("[ISA ] IRQ5 masked; IOAPIC vector=0x46 active.\n");
        break;
    case 6U:
        serial_write("[ISA ] IRQ6 masked; IOAPIC vector=0x47 active.\n");
        break;
    case 7U:
        serial_write("[ISA ] IRQ7 masked; IOAPIC vector=0x48 active.\n");
        break;
    default:
        break;
    }
}

static void write_fallback_marker(uint8_t irq) {
    switch (irq) {
    case 3U:
        serial_write("[ISA ] IRQ3 unmasked; legacy PIC fallback retained.\n");
        break;
    case 4U:
        serial_write("[ISA ] IRQ4 unmasked; legacy PIC fallback retained.\n");
        break;
    case 5U:
        serial_write("[ISA ] IRQ5 unmasked; legacy PIC fallback retained.\n");
        break;
    case 6U:
        serial_write("[ISA ] IRQ6 unmasked; legacy PIC fallback retained.\n");
        break;
    case 7U:
        serial_write("[ISA ] IRQ7 unmasked; legacy PIC fallback retained.\n");
        break;
    default:
        break;
    }
}

void isa_irq3_7_route_all(void) {
    if (!lapic_is_enabled()) {
        return;
    }
    for (size_t index = 0U; index < sizeof(irq3_7) / sizeof(irq3_7[0]);
         ++index) {
        const uint8_t irq = irq3_7[index];
        const enum ioapic_status status =
            ioapic_route_isa_irq(irq, expected_vector(irq));
        const bool ready =
            status == IOAPIC_OK && ioapic_is_isa_irq_routed(irq);
        if (ready) {
            (void)pic_mask(irq);
            write_masked_marker(irq);
        } else {
            (void)pic_unmask(irq);
            write_fallback_marker(irq);
        }
    }
#ifdef HFOC_ISA_IRQ3_7_QEMU_TEST
    isa_irq3_7_qemu_test_init();
#endif
}

bool isa_irq3_7_is_routed(uint8_t irq) {
    return is_irq3_7(irq) && ioapic_is_isa_irq_routed(irq);
}

void isa_irq3_7_handle_irq(uint8_t irq) {
    if (!is_irq3_7(irq)) {
        return;
    }
#ifdef HFOC_ISA_IRQ3_7_QEMU_TEST
    const uint16_t iobase = uart_iobase_for_irq(irq);
    char received = '\0';
    if (iobase != 0U) {
        while ((uart_inb((uint16_t)(iobase + 5U)) & 0x01U) != 0U) {
            const char next = (char)uart_inb(iobase);
            if (received == '\0') {
                received = next;
            }
        }
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        if (irq == 3U) {
            serial_write(uart_irq3_marker);
        } else {
            serial_write("[UART] irq=");
            serial_write_u64(irq);
            serial_write(" vector=");
            serial_write_hex_u8(expected_vector(irq));
            serial_write(" char=");
        }
        if (received != '\0') {
            serial_putc(received);
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#else
    switch (irq) {
    case 3U:
        serial_write("[ISA ] irq=3 vector=0x44\n");
        break;
    case 4U:
        serial_write("[ISA ] irq=4 vector=0x45\n");
        break;
    case 5U:
        serial_write("[ISA ] irq=5 vector=0x46\n");
        break;
    case 6U:
        serial_write("[ISA ] irq=6 vector=0x47\n");
        break;
    case 7U:
        serial_write("[ISA ] irq=7 vector=0x48\n");
        break;
    default:
        break;
    }
#endif
}
