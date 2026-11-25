#!/usr/bin/env bash
# ITEM-471: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The pointer stays visible and its clocks name themselves (СРОЧНО-21, round 10).
#
# The owner's round-9 run (ITEM-470) put the real numbers on the table:
#
#   * `mouse_armed=16`, `mouse_mse=0`, `mouse_no_trb=0`, `mouse_moves=32` - the
#     reports arrived and the arithmetic moved the pointer - but the cursor was
#     never seen: with scale 3 and the acceleration, one report could shift it by
#     up to ~1140 px in a single step, and at the right/bottom edge
#     `framebuffer_cursor_show()` drew one column of the arrow;
#   * `[POINTER]` published moves and costs and nothing about where the pointer
#     was, so "it moved" and "it is stuck in a corner" read alike;
#   * `loops` (~98 per 100-tick window) and `iter_us` (~80 000 us) still
#     disagreed ~8x, and one of the two clocks had to be lying.
#
# This item caps one report at POINTER_MAX_STEP, keeps the sprite wholly on the
# screen, publishes `x=`/`y=`/`clamped=`, re-measures the cycle rate over a
# ten-tick window instead of freezing the first two-tick sample, publishes the
# calibrated rate (`cal_hz=`) beside `loops`/`iter_us`, and pins the loop
# average and calibration arithmetic as pure functions a host proof drives.

for item471_file in \
    kernel/include/pointer.h \
    kernel/src/pointer.c \
    kernel/src/framebuffer.c \
    kernel/src/console.c \
    kernel/include/arch/x86_64/timer.h \
    kernel/src/arch/x86_64/timer.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c \
    tests/pointer-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item471_file ]] || {
        echo "ITEM-471: отсутствует $item471_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# One report cannot cross the screen, and the sprite origin keeps it whole.
for item471_pointer_decl in \
    '#define POINTER_MAX_STEP 128' \
    'int32_t pointer_sprite_origin(int32_t position, int32_t extent, int32_t sprite);'; do
    grep -Fq -- "$item471_pointer_decl" kernel/include/pointer.h || {
        echo "ITEM-471: модуль указателя не объявляет:" \
            "$item471_pointer_decl" >&2
        exit 1
    }
done
for item471_pointer_impl in \
    'if (scaled > POINTER_MAX_STEP) {' \
    'if (scaled < -POINTER_MAX_STEP) {' \
    'int32_t pointer_sprite_origin(int32_t position, int32_t extent, int32_t sprite)' \
    'return extent - sprite;'; do
    grep -Fq -- "$item471_pointer_impl" kernel/src/pointer.c || {
        echo "ITEM-471: реализация указателя не содержит:" \
            "$item471_pointer_impl" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- B --
# The framebuffer draws the arrow from the clamped origin, so the whole sprite
# is on the screen at the right and bottom edges.
grep -Fq 'pointer_sprite_origin(' kernel/src/framebuffer.c || {
    echo "ITEM-471: framebuffer не зажимает начало спрайта" >&2
    exit 1
}
grep -Fq 'cursor_saved_x = origin_x;' kernel/src/framebuffer.c || {
    echo "ITEM-471: framebuffer не сохраняет зажатое начало" >&2
    exit 1
}

# ---------------------------------------------------------------------- C --
# `[POINTER]` names the position and the clamp counter.
grep -Fq '" x="' kernel/src/console.c || {
    echo "ITEM-471: [POINTER] не несёт x=" >&2
    exit 1
}
grep -Fq '" y="' kernel/src/console.c || {
    echo "ITEM-471: [POINTER] не несёт y=" >&2
    exit 1
}
grep -Fq '" clamped="' kernel/src/console.c || {
    echo "ITEM-471: [POINTER] не несёт clamped=" >&2
    exit 1
}
grep -Fq 'pointer_now.clamped' kernel/src/console.c || {
    echo "ITEM-471: [POINTER] не читает счётчик зажатий" >&2
    exit 1
}

# ---------------------------------------------------------------------- D --
# ITEM-472 replaced ITEM-471's tick-count calibration with a PIT-hardware one,
# so this section pins the arithmetic helpers and the rate/uptime readings that
# survive; the honest-clock shape itself is slice 78's contract.
for item471_timer_decl in \
    'uint64_t timer_cycles_per_second(void);' \
    'uint64_t timer_real_seconds(void);' \
    'uint64_t timer_measured_tick_hz(void);' \
    'static inline uint64_t timer_calibration_rate(uint64_t cycles, uint64_t ticks,' \
    'static inline uint64_t timer_window_average_us(uint64_t window_us,'; do
    grep -Fq -- "$item471_timer_decl" kernel/include/arch/x86_64/timer.h || {
        echo "ITEM-471: timer.h не объявляет:" "$item471_timer_decl" >&2
        exit 1
    }
done
for item471_timer_impl in \
    'uint64_t timer_cycles_per_second(void) {' \
    'uint64_t timer_real_seconds(void) {'; do
    grep -Fq -- "$item471_timer_impl" kernel/src/arch/x86_64/timer.c || {
        echo "ITEM-471: timer.c не содержит:" "$item471_timer_impl" >&2
        exit 1
    }
done
grep -Fq 'timer_window_average_us(production_iteration_window_us,' \
    kernel/src/kernel.c || {
    echo "ITEM-471: средний такт не считается чистым помощником" >&2
    exit 1
}
grep -Fq '" cal_hz="' kernel/src/xhci.c || {
    echo "ITEM-471: [LOOP-DIAG] не публикует калибровку" >&2
    exit 1
}
grep -Fq 'timer_cycles_per_second()' kernel/src/xhci.c || {
    echo "ITEM-471: [LOOP-DIAG] не читает cal_hz" >&2
    exit 1
}

# The host proof drives the pointer cap, the sprite origin and both clock
# reconciliations.
for item471_claim in \
    'one fast report is capped at a single followable step' \
    'the arrow shifts left so its last column stays on the screen' \
    'the arrow shifts up so its last row stays on the screen' \
    'the average turn is the window divided by the turns' \
    'the calibrated rate is the cycles per tick times the tick rate'; do
    grep -Fq -- "$item471_claim" tests/pointer-host.c || {
        echo "ITEM-471: host-проба не утверждает:" "$item471_claim" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents --
for item471_doc in \
    'ITEM-471' \
    'POINTER_MAX_STEP' \
    'pointer_sprite_origin' \
    'cal_hz='; do
    grep -Fq -- "$item471_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-471:" \
            "$item471_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-471' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-471" >&2
    exit 1
}
grep -Fq 'ITEM-471' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-471" >&2
    exit 1
}
