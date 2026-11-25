#include "arch/x86_64/timer.h"
#include "arch/x86_64/io.h"
#include "lapic_timer.h"
#include "platform_reset.h"

#define PIT_COMMAND 0x43
#define PIT_CHANNEL0 0x40
#define PIT_BASE_HZ 1193182U

static volatile uint64_t tick_count;
static volatile uint64_t second_events;
static uint32_t timer_frequency = HFOC_TIMER_HZ;

void timer_init(uint32_t frequency) {
    if (frequency == 0U) {
        frequency = HFOC_TIMER_HZ;
    }
    timer_frequency = frequency;
    uint32_t divisor = PIT_BASE_HZ / frequency;
    if (divisor == 0U) {
        divisor = 1U;
    }
    if (divisor > 0xffffU) {
        divisor = 0xffffU;
    }
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xffU));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8U) & 0xffU));
}

void timer_handle_irq(void) {
    ++tick_count;
    if ((tick_count % timer_frequency) == 0U) {
        ++second_events;
    }
    lapic_timer_on_pit_tick();
}

uint64_t timer_ticks(void) {
    return tick_count;
}

uint64_t timer_uptime_seconds(void) {
    return tick_count / timer_frequency;
}

bool timer_take_second_event(void) {
    if (second_events == 0U) {
        return false;
    }
    --second_events;
    return true;
}

void timer_delay_ms(uint32_t milliseconds) {
    /*
     * ITEM-429: the wait owns PIT channel 0 and gives it back.
     *
     * `platform_reset_delay_ms()` is the busy-wait that works whether or not
     * the channel was programmed (ITEM-417), which is what the NVMe bring-up
     * needs: it runs after `timer_init()` but before `cpu_sti()`, so no tick
     * fires and the counter has to be read directly.  What it cannot do is
     * leave the channel in its own rate mode: the boot programmed channel 0
     * for the 100 Hz tick that `timer_ticks()`, the lwIP clock and the boot
     * log's `ticks=` are all derived from, and a driver's wait must not slow
     * every one of them for the rest of the session.  Re-programming the timer
     * after the wait restores the divisor and the mode without touching the
     * tick or second counters.
     */
    platform_reset_delay_ms(milliseconds);
    timer_init(timer_frequency);
}

/* The PHY clock needs a completed reference interval, not an early stall.
 * Restore the configured timer on both success and refusal. Existing void
 * delay callers keep their established bounded behavior. */
bool timer_delay_ms_checked(uint32_t milliseconds) {
    const bool completed = platform_reset_delay_ms_checked(milliseconds);
    timer_init(timer_frequency);
    return completed;
}

/*
 * ITEM-463: the cycle counter and its measured frequency.
 *
 * The console's costs are published in microseconds because the machine's
 * 100 Hz tick cannot resolve them.  The counter itself is `rdtsc`; the
 * frequency is measured over at least two PIT ticks after interrupts are
 * enabled, so it is the rate of the machine the kernel actually runs on
 * instead of a constant no architecture promises.  The calibration arithmetic
 * is the only place a cycle count and a tick count meet, so a caller only ever
 * asks for the microseconds between two cycle readings.
 */
static uint64_t cycle_base;
static uint64_t cycles_per_second;
static bool cycle_clock_ready;
/* ITEM-472: the measured IRQ tick rate and the window it is measured over. */
static uint64_t tick_rate_cycles;
static uint64_t tick_rate_ticks;
static uint64_t tick_rate_hz;

/*
 * ITEM-472 (СРОЧНО-22): the cycle counter is calibrated against the PIT's own
 * hardware counter, not against the IRQ tick counter.
 *
 * The round-10 run published `cal_hz=17 222 012 730` beside a real TSC of
 * 3.893 GHz.  The old calibration divided `cycles` by `timer_ticks()` and
 * assumed the kernel counted 100 of those a second.  It did not: a loop turn
 * runs for tens of milliseconds with interrupts held off through a paint, and
 * the PIT edges that arrive while they are off are counted nowhere.  Every
 * *counted* tick therefore stood for several real ticks, and the TSC looked
 * several times faster than it is - one mistake that scaled every `us=` the
 * console published.
 *
 * The PIT crystal is the honest reference and it is readable without an
 * interrupt: `platform_reset_delay_ms()` owns channel 0 at the PIT base
 * frequency for exactly the requested span, so the sample is wall time no
 * matter how many IRQs the CPU took.  The 100 Hz tick is restored immediately
 * after, and the boot's first turns are the only time this runs.
 */
#define TIMER_CYCLE_CALIBRATION_MS 100U

uint64_t timer_cycles(void) {
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32U) | (uint64_t)low;
}

void timer_cycle_clock_start(void) {
    uint64_t start;
    uint64_t elapsed;

    start = timer_cycles();
    cycle_base = start;
    platform_reset_delay_ms(TIMER_CYCLE_CALIBRATION_MS);
    elapsed = timer_cycles() - start;
    /* The delay owns PIT channel 0; give the 100 Hz tick back at once. */
    timer_init(timer_frequency);
    cycles_per_second = (elapsed * 1000U) / TIMER_CYCLE_CALIBRATION_MS;
    cycle_clock_ready = cycles_per_second != 0U;
    tick_rate_cycles = start;
    tick_rate_ticks = timer_ticks();
    tick_rate_hz = 0U;
}

void timer_cycle_clock_poll(void) {
    uint64_t cycles;
    uint64_t ticks;

    if (!cycle_clock_ready) {
        return;
    }
    if (tick_rate_cycles == 0U) {
        tick_rate_cycles = timer_cycles();
        tick_rate_ticks = timer_ticks();
        return;
    }
    cycles = timer_cycles() - tick_rate_cycles;
    /* Half a real second, so one unlucky turn cannot publish the rate. */
    if (cycles * 2U < cycles_per_second) {
        return;
    }
    ticks = timer_ticks() - tick_rate_ticks;
    tick_rate_hz = (ticks * cycles_per_second) / cycles;
    tick_rate_cycles = timer_cycles();
    tick_rate_ticks = timer_ticks();
}

bool timer_cycle_clock_ready(void) {
    return cycle_clock_ready;
}

uint64_t timer_cycles_per_second(void) {
    return cycles_per_second;
}

/*
 * ITEM-472: the counted IRQ ticks per real second.  `tick_count` only advances
 * when an interrupt is delivered, so a value below `HFOC_TIMER_HZ` is the
 * number that says how much real time the tick counter lost; the loop
 * diagnostic publishes it beside `cal_hz` and `second=`.
 */
uint64_t timer_measured_tick_hz(void) {
    return tick_rate_hz;
}

uint64_t timer_real_seconds(void) {
    if (!cycle_clock_ready || cycles_per_second == 0U) {
        return 0U;
    }
    return (timer_cycles() - cycle_base) / cycles_per_second;
}

uint64_t timer_real_milliseconds(void) {
    if (!cycle_clock_ready || cycles_per_second == 0U) {
        return 0U;
    }
    return ((timer_cycles() - cycle_base) * 1000U) / cycles_per_second;
}

uint64_t timer_cycles_to_us(uint64_t cycles) {
    if (!cycle_clock_ready || cycles_per_second == 0U) {
        return 0U;
    }
    return (cycles * 1000000U) / cycles_per_second;
}
