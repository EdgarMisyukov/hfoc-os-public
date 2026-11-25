#ifndef HFOC_PANIC_H
#define HFOC_PANIC_H

#include "arch/x86_64/interrupts.h"

_Noreturn void kernel_panic(const char *message);
/*
 * ITEM-390: a panic may carry one bounded, single-line detail (numbers behind
 * the failure). It is printed to serial and to the panic screen, so a machine
 * without a serial console still shows why the kernel stopped instead of one
 * sentence with no diagnostics. The detail is copied into a bounded buffer.
 */
_Noreturn void kernel_panic_detail(const char *message, const char *detail);
_Noreturn void kernel_panic_exception(const char *message, const struct interrupt_frame *frame);

#endif
