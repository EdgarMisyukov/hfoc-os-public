#ifndef HFOC_ISA_IRQ3_7_H
#define HFOC_ISA_IRQ3_7_H

#include <stdbool.h>
#include <stdint.h>

#define IOAPIC_IRQ3_VECTOR 0x44U
#define IOAPIC_IRQ4_VECTOR 0x45U
#define IOAPIC_IRQ5_VECTOR 0x46U
#define IOAPIC_IRQ6_VECTOR 0x47U
#define IOAPIC_IRQ7_VECTOR 0x48U
#define ISA_IRQ3_7_FIRST 3U
#define ISA_IRQ3_7_LAST 7U

void isa_irq3_7_route_all(void);
bool isa_irq3_7_is_routed(uint8_t irq);
void isa_irq3_7_handle_irq(uint8_t irq);

#endif
