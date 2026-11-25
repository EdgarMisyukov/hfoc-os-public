#!/usr/bin/env bash
# ITEM-476: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The phases describe the work they are published beside (round 14 follow-up).
#
# The local production run
# `production-storage-boot-20260923T190005Z-561658` read
# `[LOOP-PHASES] turns=74 console_us=10032 xhci_us=11211 input_us=23152
#  net_us=226474 other_us=0 work_us=264132`: the parts summed to 270869 us, so
# they exceeded the turn's work by 6737 us and the QEMU proof's
# "the loop's phases exceed the turn's work" fired.  The cause was ordering,
# not measurement: a phase span accumulated into the window the moment it
# ended, while `work_us` and `console_us` were folded only at the next turn
# boundary.  A record published mid-turn therefore described N completed turns
# of work beside N completed turns plus one unfinished turn's spans.
#
# The fix folds a finished turn's spans in the same step that folds its work
# and console cost, so the published window holds completed turns only:
#
#   * `kernel_loop_phase_us[]` is the turn in flight; `kernel_loop_phase_turn_begin()`
#     adds it to `kernel_loop_phase_window_us[]` and clears it, right where it
#     adds `kernel_loop_phase_previous_work_us` and the console cost.
#   * `kernel_loop_phase_window_reset()` resets the completed-turn window and
#     leaves the in-flight spans alone, so no span is counted twice or dropped.
#   * `log_production_loop_phases()` reads the window, and `other_us` stays the
#     remainder `work_us - accounted`, so the parts can never exceed the work.
#   * The QEMU production proof checks the invariant in every qualifying record,
#     not only in the one it prints, so timing luck cannot hide a regression.

for item476_file in \
    kernel/src/kernel.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item476_file ]] || {
        echo "ITEM-476: отсутствует $item476_file" >&2
        exit 1
    }
done

# ------------------------------------------- the completed-turn window --
grep -Fq 'static uint64_t kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_COUNT];' \
    kernel/src/kernel.c || {
    echo "ITEM-476: окно завершённых оборотов не объявлено" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_window_us[index] += kernel_loop_phase_us[index];' \
    kernel/src/kernel.c || {
    echo "ITEM-476: спаны не сворачиваются в окно на границе оборота" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_us[index] = 0U;' kernel/src/kernel.c || {
    echo "ITEM-476: спаны оборота не очищаются после сворачивания" >&2
    exit 1
}

# The fold happens inside `kernel_loop_phase_turn_begin()` and before the
# work/console/hlt lines it must stay aligned with, and the reset leaves the
# in-flight accumulator alone.
item476_fold_body=$(
    awk '/^static void kernel_loop_phase_turn_begin\(void\) \{/,/^\}/' \
        kernel/src/kernel.c
)
grep -Fq 'kernel_loop_phase_window_us[index] += kernel_loop_phase_us[index];' \
    <<<"$item476_fold_body" || {
    echo "ITEM-476: сворачивание не внутри начала оборота" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_work_us += work;' <<<"$item476_fold_body" || {
    echo "ITEM-476: сворачивание не рядом с работой оборота" >&2
    exit 1
}
item476_fold_phase_line=$(
    grep -n 'kernel_loop_phase_window_us\[index\] +=' \
        kernel/src/kernel.c | cut -d: -f1
)
item476_fold_work_line=$(
    grep -n 'kernel_loop_phase_work_us += work;' \
        kernel/src/kernel.c | cut -d: -f1
)
[[ -n $item476_fold_phase_line && -n $item476_fold_work_line &&
   $item476_fold_phase_line -lt $item476_fold_work_line ]] || {
    echo "ITEM-476: спаны сворачиваются после работы оборота" >&2
    exit 1
}
item476_reset_body=$(
    awk '/^static void kernel_loop_phase_window_reset\(void\) \{/,/^\}/' \
        kernel/src/kernel.c
)
grep -Fq 'kernel_loop_phase_window_us[index] = 0U;' <<<"$item476_reset_body" || {
    echo "ITEM-476: сброс окна не очищает завершённые обороты" >&2
    exit 1
}
if grep -Fq 'kernel_loop_phase_us[index] = 0U;' <<<"$item476_reset_body"; then
    echo "ITEM-476: сброс окна стирает спаны оборота в полёте" >&2
    exit 1
fi

# The publish reads the window, and the remainder stays a remainder.
for item476_phase in XHCI INPUT NETWORK; do
    grep -Fq "= kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_$item476_phase];" \
        kernel/src/kernel.c || {
        echo "ITEM-476: запись фаз читает не окно завершённых оборотов:" \
            "$item476_phase" >&2
        exit 1
    }
done
grep -Fq 'other_us = work_us > accounted ? work_us - accounted : 0U;' \
    kernel/src/kernel.c || {
    echo "ITEM-476: other_us не является остатком работы" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
# Every qualifying record keeps its parts inside the work, not only the last.
grep -Fq 'the loop'"'"'s phases exceed the turn'"'"'s work in a record' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-476: QEMU-проба не сверяет каждую запись фаз" >&2
    exit 1
}
grep -Fq 'candidate_sum = sum(int(candidate[key]) for key in (' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-476: QEMU-проба не считает сумму каждой записи" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item476_doc in \
    'ITEM-476' \
    'kernel_loop_phase_window_us' \
    'completed turns'; do
    grep -Fq -- "$item476_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-476:" \
            "$item476_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-476' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-476" >&2
    exit 1
}
grep -Fq 'ITEM-476' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-476" >&2
    exit 1
}
