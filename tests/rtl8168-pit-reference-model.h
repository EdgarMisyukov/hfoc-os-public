/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial */
#ifndef TEST_RTL8168_PIT_REFERENCE_MODEL_H
#define TEST_RTL8168_PIT_REFERENCE_MODEL_H

/* Exercise the production reference wait, including its stall bound. Only
 * the PIT ports are modeled; no copied wait algorithm or privileged I/O. */
#include "arch/x86_64/io.h"

static struct {
    uint16_t counter;
    uint16_t latched;
    bool high;
    bool stopped;
    unsigned samples;
    unsigned starts;
    unsigned reloads;
    unsigned foreign_ports;
    uint64_t counts;
} pit_reference;

static void pit_reference_outb(uint16_t port, uint8_t value)
{
    if (port == 0x43U && value == 0x34U) {
        ++pit_reference.starts;
        pit_reference.counter = 0U;
    } else if (port == 0x43U && value == 0U) {
        ++pit_reference.samples;
        if (!pit_reference.stopped) {
            pit_reference.counter = (uint16_t)(pit_reference.counter - 2000U);
            pit_reference.counts += 2000U;
        }
        pit_reference.latched = pit_reference.counter;
        pit_reference.high = false;
    } else if (port == 0x40U && value == 0U) {
        ++pit_reference.reloads;
    } else {
        ++pit_reference.foreign_ports;
    }
}

static uint8_t pit_reference_inb(uint16_t port)
{
    uint8_t value;
    if (port != 0x40U) {
        ++pit_reference.foreign_ports;
        return 0U;
    }
    value = pit_reference.high ? (uint8_t)(pit_reference.latched >> 8U)
                               : (uint8_t)pit_reference.latched;
    pit_reference.high = !pit_reference.high;
    return value;
}

#define outb pit_reference_outb
#define inb pit_reference_inb
#include "../kernel/src/platform_reset.c"
#undef inb
#undef outb

static void test_phy_reference_wait_reports_completion(void)
{
    memset(&pit_reference, 0, sizeof(pit_reference));
    CHECK(platform_reset_delay_ms_checked(0U) && pit_reference.samples == 0U,
          "zero reference interval performs no PIT I/O");
    CHECK(platform_reset_delay_ms_checked(100U) &&
              pit_reference.counts - 2000U >= 119318U &&
              pit_reference.starts == 1U && pit_reference.reloads == 2U &&
              pit_reference.foreign_ports == 0U,
          "production checked reference completes requested crystal counts");
    memset(&pit_reference, 0, sizeof(pit_reference));
    pit_reference.stopped = true;
    CHECK(!platform_reset_delay_ms_checked(100U) &&
              pit_reference.samples == 100001U && pit_reference.counts == 0U,
          "stopped production PIT reference refuses within its sample bound");
    memset(&pit_reference, 0, sizeof(pit_reference));
    pit_reference.stopped = true;
    platform_reset_delay_ms(100U);
    CHECK(pit_reference.samples == 100001U &&
              pit_reference.foreign_ports == 0U,
          "legacy void wait retains its bounded stopped-counter behavior");
}
#endif
