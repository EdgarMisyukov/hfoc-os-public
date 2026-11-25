#ifndef HFOC_ARCH_X86_64_TIMER_H
#define HFOC_ARCH_X86_64_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#define HFOC_TIMER_HZ 100U

void timer_init(uint32_t frequency);
void timer_handle_irq(void);
uint64_t timer_ticks(void);
uint64_t timer_uptime_seconds(void);
bool timer_take_second_event(void);
/*
 * ITEM-429: a bounded millisecond wait that leaves the timer as timer_init()
 * programmed it.
 *
 * The NVMe bring-up waits for a controller-ready transition, and it runs
 * before `cpu_sti()`, so the tick counter the rest of the kernel measures time
 * with does not move yet: the wait has to read a hardware counter.  This
 * wrapper owns PIT channel 0 for the duration of the wait and hands it back
 * afterwards, because the boot already programmed that channel through
 * timer_init() for the 100 Hz tick every clock in the machine is derived from
 * - a wait by one driver must not slow them all down.
 */
void timer_delay_ms(uint32_t milliseconds);
/* Same timer restoration, with the reference counter's completion verdict. */
bool timer_delay_ms_checked(uint32_t milliseconds);

/*
 * ITEM-463: the cycle counter behind the console's microsecond costs.
 *
 * The 100 Hz tick cannot see a paint or a scroll - both finish inside one tick,
 * so the millisecond reading was "0 or 1" for the row move and the per-pixel
 * move it replaced alike.  The console reads the processor's cycle counter
 * (`rdtsc`) instead, and the counter's frequency is measured against the PIT
 * tick a moment after interrupts are enabled: it is the machine's own rate, not
 * an assumed constant.  `timer_cycle_clock_start()` is called once after
 * `cpu_sti()`, `timer_cycle_clock_poll()` once per loop turn until it is ready,
 * and a caller that needs a duration stores `timer_cycles()` and converts the
 * difference.  Before the clock is ready `timer_cycles_to_us()` answers 0, so a
 * cost recorded that early is not mistaken for a measured one.
 */
void timer_cycle_clock_start(void);
void timer_cycle_clock_poll(void);
bool timer_cycle_clock_ready(void);
uint64_t timer_cycles(void);
uint64_t timer_cycles_to_us(uint64_t cycles);
/* ITEM-471: the TSC rate the cycle clock settled on, for the record. */
uint64_t timer_cycles_per_second(void);
/*
 * ITEM-472 (СРОЧНО-22): the honest clock's two readings.  The TSC rate is
 * calibrated against the PIT hardware, so `timer_real_seconds()` is wall time
 * even when the IRQ tick counter lost edges; `timer_measured_tick_hz()` is how
 * many IRQ ticks the kernel actually counted in the last real second, so a
 * value under `HFOC_TIMER_HZ` names exactly how much the tick counter lost.
 */
uint64_t timer_real_seconds(void);
/* ITEM-652: the same monotonic cycle-clock uptime, at millisecond resolution,
   for records whose duration is smaller than one second but must still agree
   with their own measured slice counters. */
uint64_t timer_real_milliseconds(void);
uint64_t timer_measured_tick_hz(void);

/*
 * ITEM-471 (СРОЧНО-21): the calibration arithmetic, out of the state machine
 * so a host proof can drive it.
 *
 * The owner's round-9 log read `loops` (~98 per 100-tick heartbeat window)
 * beside `iter_us` (~80 000 us) and the two could not both describe the same
 * second.  One of the two clocks is lying: either the cycle counter's rate is
 * mis-calibrated, or the PIT tick under-counts real time because edges are
 * lost while interrupts are held off through a slow paint.  The item cannot
 * decide which by assuming the other; it makes both arithmetic paths pure and
 * pinned, so the next run reads them side by side instead of guessing.
 *
 * `timer_calibration_rate()` is the old one-line calibration, factored out:
 * the TSC rate from a sample of `cycles` over `ticks` PIT ticks at `hz` ticks
 * per second.  `timer_window_average_us()` is the loop-period arithmetic:
 * the average turn over a window of `window_us` microseconds and `turns`
 * turns, and it answers 0 for an empty window instead of dividing by zero.
 */
static inline uint64_t timer_calibration_rate(uint64_t cycles, uint64_t ticks,
                                              uint32_t hz)
{
    if (ticks == 0U) {
        return 0U;
    }
    return cycles * hz / ticks;
}

static inline uint64_t timer_window_average_us(uint64_t window_us,
                                               uint64_t turns)
{
    if (turns == 0U) {
        return 0U;
    }
    return window_us / turns;
}

#endif
