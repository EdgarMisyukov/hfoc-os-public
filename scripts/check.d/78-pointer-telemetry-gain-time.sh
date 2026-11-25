#!/usr/bin/env bash
# ITEM-472: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The pointer's deltas are numbers, the gain has a handle, and the clock is
# honest (СРОЧНО-22, round 11).
#
# The owner's round-10 run left three questions the journal could not answer:
#
#   * `mouse_reports=30`, `mouse_moves=27`, `mouse_armed=16`, `mouse_no_trb=0`
#     and one `[POINTER] x=1920 y=1083 clamped=0 moves=1` - the reports reached
#     the arithmetic, but the record went out once per 64 movements and carried
#     no sum of the applied deltas, so "the receiver sends tiny deltas" and "the
#     reports are lost on NAK" read alike;
#   * the feel of the pointer was fixed in code: `scale` is automatic and the
#     acceleration is constant, so the owner could not tune it in a run;
#   * `cal_hz=17 222 012 730` beside a real TSC of 3.893 GHz - the calibration
#     divided by the IRQ tick counter, which loses edges while interrupts are
#     held off through a paint, so every `us=` was scaled by the same mistake.
#
# This item publishes `[POINTER]` once per real second with the applied-delta
# sums (`dx=`/`dy=`), the largest single applied delta (`dmax=`) and the
# operator's `gain=`; adds the `PTR` command (`PTR` shows, `PTR <1..64>` sets)
# whose default 1 is the ITEM-471 arithmetic; calibrates the TSC against the
# PIT's own hardware counter instead of the IRQ tick counter; and publishes
# `tick_hz=` (counted IRQ ticks per real second) and `second=` (wall-clock
# uptime) in `[LOOP-DIAG]`.

for item472_file in \
    kernel/include/pointer.h \
    kernel/src/pointer.c \
    kernel/src/console.c \
    kernel/include/console.h \
    kernel/include/arch/x86_64/timer.h \
    kernel/src/arch/x86_64/timer.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c \
    tests/pointer-host.c \
    tests/console-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item472_file ]] || {
        echo "ITEM-472: отсутствует $item472_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# `[POINTER]` carries the applied deltas and the largest single one.
for item472_pointer_decl in \
    'int32_t pointer_apply_delta(int32_t delta, uint32_t scale, uint32_t gain);' \
    'bool pointer_move_gained(struct pointer_state *state, int32_t dx, int32_t dy,' \
    '#define POINTER_GAIN_MAX 64U'; do
    grep -Fq -- "$item472_pointer_decl" kernel/include/pointer.h || {
        echo "ITEM-472: pointer.h не объявляет:" "$item472_pointer_decl" >&2
        exit 1
    }
done
for item472_pointer_impl in \
    'int32_t pointer_apply_delta(int32_t delta, uint32_t scale, uint32_t gain)' \
    'gain > POINTER_GAIN_MAX' \
    'bool pointer_move_gained(struct pointer_state *state, int32_t dx, int32_t dy,'; do
    grep -Fq -- "$item472_pointer_impl" kernel/src/pointer.c || {
        echo "ITEM-472: реализация указателя не содержит:" \
            "$item472_pointer_impl" >&2
        exit 1
    }
done
for item472_console_field in \
    '" dx="' \
    '" dy="' \
    '" dmax="' \
    '" gain="'; do
    grep -Fq -- "$item472_console_field" kernel/src/console.c || {
        echo "ITEM-472: [POINTER] не несёт $item472_console_field" >&2
        exit 1
    }
done
grep -Fq 'console_pointer_window_dx' kernel/src/console.c || {
    echo "ITEM-472: [POINTER] не суммирует применённые дельты" >&2
    exit 1
}
grep -Fq 'console_pointer_window_dmax' kernel/src/console.c || {
    echo "ITEM-472: [POINTER] не публикует максимальную дельту" >&2
    exit 1
}

# The record is due on the honest clock, not on the counted IRQ ticks.
grep -Fq 'console_pointer_tick();' kernel/src/kernel.c || {
    echo "ITEM-472: главный цикл не публикует [POINTER] по честным часам" >&2
    exit 1
}
grep -Fq 'void console_pointer_tick(void);' kernel/include/console.h || {
    echo "ITEM-472: console.h не объявляет console_pointer_tick" >&2
    exit 1
}
grep -Fq 'second == console_pointer_published_second' kernel/src/console.c || {
    echo "ITEM-472: [POINTER] не выдерживает секунду" >&2
    exit 1
}

# ---------------------------------------------------------------------- B --
# `PTR` shows and sets the operator's gain; default 1.
grep -Fq 'console_command_prefix(input_line, "ptr ")' kernel/src/console.c || {
    echo "ITEM-472: консоль не принимает команду PTR <n>" >&2
    exit 1
}
grep -Fq 'static uint32_t console_pointer_gain = 1U;' kernel/src/console.c || {
    echo "ITEM-472: усиление по умолчанию не 1" >&2
    exit 1
}
grep -Fq '"[POINTER] gain="' kernel/src/console.c || {
    echo "ITEM-472: ответ PTR не называет усиление" >&2
    exit 1
}
grep -Fq ' PTR' kernel/src/console.c || {
    echo "ITEM-472: HELP не называет PTR" >&2
    exit 1
}

# ---------------------------------------------------------------------- C --
# The TSC is calibrated against the PIT hardware, not the IRQ tick counter,
# and the loop diagnostic publishes the counted tick rate and wall-clock
# uptime beside the calibrated rate.
for item472_timer_impl in \
    '#define TIMER_CYCLE_CALIBRATION_MS 100U' \
    'platform_reset_delay_ms(TIMER_CYCLE_CALIBRATION_MS);' \
    'uint64_t timer_measured_tick_hz(void) {' \
    'uint64_t timer_real_seconds(void) {'; do
    grep -Fq -- "$item472_timer_impl" kernel/src/arch/x86_64/timer.c || {
        echo "ITEM-472: timer.c не содержит:" "$item472_timer_impl" >&2
        exit 1
    }
done
if grep -Fq 'timer_calibration_rate(cycles, ticks, HFOC_TIMER_HZ)' \
    kernel/src/arch/x86_64/timer.c; then
    echo "ITEM-472: калибровка всё ещё считает IRQ-тики секундами" >&2
    exit 1
fi
grep -Fq '" tick_hz="' kernel/src/xhci.c || {
    echo "ITEM-472: [LOOP-DIAG] не публикует tick_hz" >&2
    exit 1
}
grep -Fq '" second="' kernel/src/xhci.c || {
    echo "ITEM-472: [LOOP-DIAG] не публикует second" >&2
    exit 1
}

# The host proofs drive the gain, the cap and the once-a-second window.
for item472_claim in \
    'the operator'"'"'s gain multiplies the movement' \
    'even the highest gain is capped at one followable step' \
    'PTR <n> sets and names the operator'"'"'s gain' \
    'the first movement publishes the applied delta, dmax and the gain' \
    'the next real second publishes only what landed in its window'; do
    grep -Fq -- "$item472_claim" tests/pointer-host.c tests/console-host.c || {
        echo "ITEM-472: host-проба не утверждает:" "$item472_claim" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents --
for item472_doc in \
    'ITEM-472' \
    'PTR' \
    'tick_hz=' \
    'dx='; do
    grep -Fq -- "$item472_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-472:" \
            "$item472_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-472' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-472" >&2
    exit 1
}
grep -Fq 'ITEM-472' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-472" >&2
    exit 1
}
