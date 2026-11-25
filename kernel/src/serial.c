#include "serial.h"
#include "arch/x86_64/io.h"

#define COM1 0x3f8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x0b);
}

void serial_putc(char value) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
        __asm__ volatile ("pause");
    }
    outb(COM1, (uint8_t)value);
}

static void serial_write_text(const char *text) {
    while (*text != '\0') {
        if (*text == '\n') {
            serial_putc('\r');
        }
        serial_putc(*text++);
    }
}

void serial_write(const char *text) {
    const uint64_t flags = cpu_irq_save();
    serial_write_text(text);
    cpu_irq_restore(flags);
}

uint64_t serial_sequence_begin(void) {
    return cpu_irq_save();
}

void serial_sequence_end(uint64_t flags) {
    cpu_irq_restore(flags);
}

void serial_write_u64(uint64_t value) {
    char buffer[21];
    unsigned int index = 0;
    const uint64_t flags = cpu_irq_save();

    if (value == 0) {
        serial_putc('0');
    } else {
        while (value > 0) {
            buffer[index++] = (char)('0' + value % 10);
            value /= 10;
        }
        while (index > 0) {
            serial_putc(buffer[--index]);
        }
    }
    cpu_irq_restore(flags);
}

void serial_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    const uint64_t flags = cpu_irq_save();

    serial_write_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> shift) & 0x0f]);
    }
    cpu_irq_restore(flags);
}
