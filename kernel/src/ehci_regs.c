/*
 * ITEM-566: the one place the operational window's address is computed.
 *
 * See kernel/include/ehci_regs.h: an adapter that lost CAPLENGTH would still
 * pass an engine test with a substituted register file, so the arithmetic is
 * production code and the proof drives it over a fake BAR.
 */

#include "ehci_regs.h"

uint32_t ehci_reg_read(const volatile uint8_t *bar, uint8_t caplength,
                       uint32_t offset) {
    return *(const volatile uint32_t *)(bar + (uint32_t)caplength + offset);
}

void ehci_reg_write(volatile uint8_t *bar, uint8_t caplength, uint32_t offset,
                    uint32_t value) {
    *(volatile uint32_t *)(bar + (uint32_t)caplength + offset) = value;
}
