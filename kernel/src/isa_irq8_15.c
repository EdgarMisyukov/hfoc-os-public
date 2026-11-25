#include "isa_irq8_15.h"

#include "arch/x86_64/pic.h"
#include "ioapic.h"
#include "lapic.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>

#ifdef HFOC_VIRTIO_NET_FRAME_QEMU_TEST
bool virtio_net_frame_qemu_ack_irq11(void);
#endif

#ifdef HFOC_ISA_IRQ8_15_QEMU_TEST
struct isa_irq8_15_uart_port {
    uint8_t irq;
    uint16_t iobase;
};

static const char uart_irq8_marker[] =
    "[UART] irq=8 vector=0x49 char=";

static const struct isa_irq8_15_uart_port uart_ports[] = {
    {8U, 0x260U},
    {9U, 0x2a8U},
    {10U, 0x2b8U},
    {11U, 0x268U},
    {12U, 0x2e0U},
    {13U, 0x2e8U},
    {14U, 0x3e0U},
    {15U, 0x3e8U}
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

static void isa_irq8_15_qemu_test_init(void) {
    for (size_t index = 0U;
         index < sizeof(uart_ports) / sizeof(uart_ports[0]); ++index) {
        uart_enable_receive_interrupt(uart_ports[index].iobase);
    }
    serial_write("[ISA ] IRQ8-15 UART receive interrupts enabled.\n");
}

static void serial_write_hex_u8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_putc('0');
    serial_putc('x');
    serial_putc(digits[(value >> 4U) & 0x0FU]);
    serial_putc(digits[value & 0x0FU]);
}
#endif

#ifdef HFOC_ISA_IRQ8_15_HOST_TEST
enum ioapic_status host_ioapic_route_isa_irq(uint8_t irq, uint8_t vector);
bool host_ioapic_is_isa_irq_routed(uint8_t irq);
bool host_pic_mask(uint8_t irq);
bool host_pic_unmask(uint8_t irq);
#define ioapic_route_isa_irq host_ioapic_route_isa_irq
#define ioapic_is_isa_irq_routed host_ioapic_is_isa_irq_routed
#define pic_mask host_pic_mask
#define pic_unmask host_pic_unmask
#endif

static const uint8_t irq8_15[] = {
    8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U
};

static bool is_irq8_15(uint8_t irq) {
    return irq >= ISA_IRQ8_15_FIRST && irq <= ISA_IRQ8_15_LAST;
}

static uint8_t expected_vector(uint8_t irq) {
    return (uint8_t)(0x41U + irq);
}

static void write_masked_marker(uint8_t irq) {
    switch (irq) {
    case 8U:
        serial_write("[ISA ] IRQ8 masked; IOAPIC vector=0x49 active.\n");
        break;
    case 9U:
        serial_write("[ISA ] IRQ9 masked; IOAPIC vector=0x4A active.\n");
        break;
    case 10U:
        serial_write("[ISA ] IRQ10 masked; IOAPIC vector=0x4B active.\n");
        break;
    case 11U:
        serial_write("[ISA ] IRQ11 masked; IOAPIC vector=0x4C active.\n");
        break;
    case 12U:
        serial_write("[ISA ] IRQ12 masked; IOAPIC vector=0x4D active.\n");
        break;
    case 13U:
        serial_write("[ISA ] IRQ13 masked; IOAPIC vector=0x4E active.\n");
        break;
    case 14U:
        serial_write("[ISA ] IRQ14 masked; IOAPIC vector=0x4F active.\n");
        break;
    case 15U:
        serial_write("[ISA ] IRQ15 masked; IOAPIC vector=0x50 active.\n");
        break;
    default:
        break;
    }
}

static void write_fallback_marker(uint8_t irq) {
    switch (irq) {
    case 8U:
        serial_write("[ISA ] IRQ8 unmasked; legacy PIC fallback retained.\n");
        break;
    case 9U:
        serial_write("[ISA ] IRQ9 unmasked; legacy PIC fallback retained.\n");
        break;
    case 10U:
        serial_write("[ISA ] IRQ10 unmasked; legacy PIC fallback retained.\n");
        break;
    case 11U:
        serial_write("[ISA ] IRQ11 unmasked; legacy PIC fallback retained.\n");
        break;
    case 12U:
        serial_write("[ISA ] IRQ12 unmasked; legacy PIC fallback retained.\n");
        break;
    case 13U:
        serial_write("[ISA ] IRQ13 unmasked; legacy PIC fallback retained.\n");
        break;
    case 14U:
        serial_write("[ISA ] IRQ14 unmasked; legacy PIC fallback retained.\n");
        break;
    case 15U:
        serial_write("[ISA ] IRQ15 unmasked; legacy PIC fallback retained.\n");
        break;
    default:
        break;
    }
}

void isa_irq8_15_route_all(void) {
    if (!lapic_is_enabled()) {
        return;
    }
    for (size_t index = 0U; index < sizeof(irq8_15) / sizeof(irq8_15[0]);
         ++index) {
        const uint8_t irq = irq8_15[index];
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
#ifdef HFOC_ISA_IRQ8_15_QEMU_TEST
    isa_irq8_15_qemu_test_init();
#endif
}

bool isa_irq8_15_is_routed(uint8_t irq) {
    return is_irq8_15(irq) && ioapic_is_isa_irq_routed(irq);
}

void isa_irq8_15_handle_irq(uint8_t irq) {
    if (!is_irq8_15(irq)) {
        return;
    }
#ifdef HFOC_ISA_IRQ8_15_QEMU_TEST
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
        if (irq == 8U) {
            serial_write(uart_irq8_marker);
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
    case 8U:
        serial_write("[ISA ] irq=8 vector=0x49\n");
        break;
    case 9U:
        serial_write("[ISA ] irq=9 vector=0x4A\n");
        break;
    case 10U:
        serial_write("[ISA ] irq=10 vector=0x4B\n");
        break;
    case 11U:
#ifdef HFOC_VIRTIO_NET_FRAME_QEMU_TEST
        (void)virtio_net_frame_qemu_ack_irq11();
#endif
        break;
    case 12U:
        serial_write("[ISA ] irq=12 vector=0x4D\n");
        break;
    case 13U:
        serial_write("[ISA ] irq=13 vector=0x4E\n");
        break;
    case 14U:
        serial_write("[ISA ] irq=14 vector=0x4F\n");
        break;
    case 15U:
        serial_write("[ISA ] irq=15 vector=0x50\n");
        break;
    default:
        break;
    }
#endif
}
