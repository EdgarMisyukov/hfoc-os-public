#ifndef HFOC_SMP_H
#define HFOC_SMP_H

#include "acpi.h"
#include "limine_min.h"
#include <stdbool.h>
#include <stdint.h>

enum smp_status {
    SMP_OK = 0,
    SMP_BSP_ONLY = 1,
    SMP_INVALID_RESPONSE = 2,
    SMP_BSP_ID_MISMATCH = 3,
    SMP_OUT_OF_CAPACITY = 4,
};

enum smp_status smp_init(
    const struct limine_mp_response *mp_response,
    const struct acpi_madt_info *madt,
    uint64_t hhdm_offset);
void smp_ap_entry(struct limine_mp_info *info);
void smp_ap_idle(void);
bool smp_online(void);
uint32_t smp_online_ap_count(void);
const char *smp_status_string(enum smp_status status);

#ifdef HFOC_SMP_HOST_TEST
bool smp_test_auto_start(void);
void smp_test_note_ap_prepared(uint32_t index);
void smp_test_note_ap_entered(uint32_t index);
void smp_test_note_ap_idle(void);
#endif

#endif
