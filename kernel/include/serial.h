#ifndef HFOC_SERIAL_H
#define HFOC_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char value);
void serial_write(const char *text);
void serial_write_u64(uint64_t value);
void serial_write_hex(uint64_t value);

/*
 * Marker lines that are assembled from more than one serial_write*() call must
 * be emitted inside one serial_sequence_begin()/serial_sequence_end() pair.
 * Every call is IRQ-safe on its own, but an interrupt handler can print an IRQ
 * trace line between two calls and split the marker, which breaks the marker
 * parsers used by the QEMU proofs (and looks like a timeout).
 */
uint64_t serial_sequence_begin(void);
void serial_sequence_end(uint64_t flags);

#endif
