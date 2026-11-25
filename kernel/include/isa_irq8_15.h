#ifndef HFOC_ISA_IRQ8_15_H
#define HFOC_ISA_IRQ8_15_H

#include <stdbool.h>
#include <stdint.h>

#define IOAPIC_IRQ8_VECTOR 0x49U
#define IOAPIC_IRQ9_VECTOR 0x4AU
#define IOAPIC_IRQ10_VECTOR 0x4BU
#define IOAPIC_IRQ11_VECTOR 0x4CU
#define IOAPIC_IRQ12_VECTOR 0x4DU
#define IOAPIC_IRQ13_VECTOR 0x4EU
#define IOAPIC_IRQ14_VECTOR 0x4FU
#define IOAPIC_IRQ15_VECTOR 0x50U
#define ISA_IRQ8_15_FIRST 8U
#define ISA_IRQ8_15_LAST 15U

void isa_irq8_15_route_all(void);
bool isa_irq8_15_is_routed(uint8_t irq);
void isa_irq8_15_handle_irq(uint8_t irq);

#endif
