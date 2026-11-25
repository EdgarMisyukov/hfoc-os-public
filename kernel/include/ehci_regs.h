#ifndef HFOC_EHCI_REGS_H
#define HFOC_EHCI_REGS_H

/*
 * ITEM-566: the operational register window, addressed the way the controller
 * decodes it: BAR0 + the CAPLENGTH the initialization module validated + the
 * register's offset inside the operational space.
 *
 * Why this is its own module: every register read and write in the transfer
 * path goes through it, and a test that substitutes a whole register file for
 * the engine cannot see an address mistake - if the adapter drops CAPLENGTH,
 * the engine's substituted registers still answer at the right offsets.  The
 * hardware run's schedule state (`ASE=0`, `ASS=0`, `PSS=1`) is exactly the kind
 * of fact that a wrong or stale base would produce, so the arithmetic is
 * production code with its own proof: a fake BAR, a non-zero CAPLENGTH, and a
 * check that the words land at CAPLENGTH + offset and nowhere else.
 */

#include <stdint.h>

uint32_t ehci_reg_read(const volatile uint8_t *bar, uint8_t caplength,
                       uint32_t offset);
void ehci_reg_write(volatile uint8_t *bar, uint8_t caplength, uint32_t offset,
                    uint32_t value);

#endif
