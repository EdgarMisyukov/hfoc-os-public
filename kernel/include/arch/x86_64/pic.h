#ifndef HFOC_ARCH_X86_64_PIC_H
#define HFOC_ARCH_X86_64_PIC_H

#include <stdbool.h>
#include <stdint.h>

void pic_init(void);
void pic_mask_all(void);
bool pic_mask(uint8_t irq);
bool pic_unmask(uint8_t irq);
bool pic_send_eoi(uint8_t irq);
bool pic_is_spurious_irq(uint8_t irq, bool *is_spurious);
bool pic_irq0_mask_save(uint8_t *out_previous_master_mask);
bool pic_master_mask_restore(uint8_t previous_master_mask);
bool pic_disable_transactional(void);
uint8_t pic_master_mask(void);
uint8_t pic_slave_mask(void);

#endif
