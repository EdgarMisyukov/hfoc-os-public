#ifndef HFOC_LAPIC_TIMER_H
#define HFOC_LAPIC_TIMER_H

#include <stdbool.h>
#include <stdint.h>

enum lapic_timer_status {
    LAPIC_TIMER_IDLE = 0,
    LAPIC_TIMER_CALIBRATING,
    LAPIC_TIMER_OK,
    LAPIC_TIMER_NO_LAPIC,
    LAPIC_TIMER_BAD_SAMPLE,
    LAPIC_TIMER_OVERFLOW,
};

enum lapic_timer_runtime_status {
    LAPIC_TIMER_RUNTIME_IDLE = 0,
    LAPIC_TIMER_RUNTIME_PREPARED,
    LAPIC_TIMER_RUNTIME_ACTIVE,
    LAPIC_TIMER_RUNTIME_FAILED,
};

/*
 * ITEM-475 (round 14): which hardware source owns the 100 Hz tick.
 *
 * The ordinary production build keeps the legacy 8259 PIC + PIT as AGENTS.md
 * requires (`HFOC_PRODUCTION_PIT_TICK`): the owner's machine measured the
 * periodic Local APIC timer at ~2.4 Hz instead of 100, so the transition is
 * compiled out there.  The drill/QEMU builds that prove the transition keep
 * it, and the choice is published as `tick_source=` so a journal reader never
 * has to infer it.
 */
enum lapic_timer_tick_source {
    LAPIC_TIMER_TICK_SOURCE_PIT = 0,
    LAPIC_TIMER_TICK_SOURCE_LAPIC = 1,
};

void lapic_timer_start_calibration(uint32_t pit_hz);
void lapic_timer_on_pit_tick(void);
enum lapic_timer_status lapic_timer_status(void);
uint64_t lapic_timer_ticks_per_second(void);
bool lapic_timer_runtime_prepare(void);
bool lapic_timer_runtime_activate(void);
void lapic_timer_runtime_abort(void);
bool lapic_timer_runtime_active(void);
enum lapic_timer_runtime_status lapic_timer_runtime_status(void);
bool lapic_timer_runtime_transition_allowed(void);
enum lapic_timer_tick_source lapic_timer_tick_source_for(bool runtime_active);
enum lapic_timer_tick_source lapic_timer_tick_source(void);
const char *lapic_timer_tick_source_string(
    enum lapic_timer_tick_source source);
bool lapic_timer_oneshot_arm(uint32_t lapic_ticks);
void lapic_timer_oneshot_handle_irq(void);
bool lapic_timer_oneshot_active(void);

#endif
