#!/usr/bin/env bash
# ITEM-484: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The arrow appears on mouse readiness, and the deferred echo replays inside
# the input batch (round 22).
#
# The owner's hardware run showed two residual delays after ITEM-482/483.  The
# pointer only painted when a report moved it, so a zero report or a first bare
# click left the screen without an arrow.  And when the body was full the
# batched scroll buffered the echo, while the only replay ran at the very end
# of the turn - after the network, the heartbeat and the diagnostics - even
# though the single input-batch barrier stood earlier.  This item:
#
#   * adds `console_pointer_ready()`, called by the loop while
#     `local_input_mouse_ready()` is true, which centres the arrow and paints
#     it exactly once, before the first movement and without a click;
#   * replays the deferred batch inside the input batch, just before the one
#     `console_end_input_batch()`, so the echo does not wait for the turn's
#     tail;
#   * raises the batch drain flag for an echo cell and for the deferred
#     replay (`console_note_echo_paint()`);
#   * keeps the budgets, the queues, the xHCI transport, the watchdog, the gain
#     and the batch scroll unchanged, and does not bring back the per-character
#     full redraw.

for item484_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    scripts/check.sh \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item484_file ]] || {
        echo "ITEM-484: отсутствует $item484_file" >&2
        exit 1
    }
done

# ------------------------------------------------ the readiness announcement --
grep -Fq 'void console_pointer_ready(void);' kernel/include/console.h || {
    echo "ITEM-484: консоль не объявляет готовность указателя" >&2
    exit 1
}
grep -Fq 'static bool pointer_initial_shown;' kernel/src/console.c || {
    echo "ITEM-484: первый показ стрелки не помечен" >&2
    exit 1
}
grep -Fq 'void console_pointer_ready(void) {' kernel/src/console.c || {
    echo "ITEM-484: готовность указателя не реализована" >&2
    exit 1
}
grep -Fq 'if (!framebuffer_ready() || pointer_initial_shown) {' \
    kernel/src/console.c || {
    echo "ITEM-484: готовность не рисует стрелку ровно один раз" >&2
    exit 1
}
grep -Fq 'pointer_initial_shown = true;' kernel/src/console.c || {
    echo "ITEM-484: первый показ не фиксируется" >&2
    exit 1
}
# The batch must not paint an initial arrow on its own: a bare click or a zero
# report cannot be the mechanism of appearance.
if grep -Fq 'pointer_initial_shown = true;' <(sed -n \
        '/void console_process_mouse_batch/,/^}/p' kernel/src/console.c); then
    echo "ITEM-484: батч мыши сам показывает стрелку" >&2
    exit 1
fi

# ------------------------------------------------- the loop announces it once --
grep -Fq 'if (local_input_mouse_ready()) {' kernel/src/kernel.c || {
    echo "ITEM-484: цикл не спрашивает готовность мыши" >&2
    exit 1
}
grep -Fq 'console_pointer_ready();' kernel/src/kernel.c || {
    echo "ITEM-484: цикл не объявляет готовность указателя" >&2
    exit 1
}

# ------------------------------------- the deferred echo replays in the batch --
grep -Fq 'static void console_note_echo_paint(void) {' kernel/src/console.c || {
    echo "ITEM-484: клетка эха не поднимает флаг барьера" >&2
    exit 1
}
grep -Fq 'console_note_echo_paint();' kernel/src/console.c || {
    echo "ITEM-484: отрисовка эха не отмечает барьер" >&2
    exit 1
}
grep -Fq 'if (console_keyboard_echo_active || console_replaying) {' \
    kernel/src/console.c || {
    echo "ITEM-484: флаг барьера не привязан к эху" >&2
    exit 1
}
# The replay must precede the single drain of the loop's input batch.  The
# input-batch flush is kernel.c's first `console_flush_deferred()` call; the
# end-of-turn one stays after it.
item484_flush_line=$(grep -n -F 'console_flush_deferred();' kernel/src/kernel.c |
    head -n1 | cut -d: -f1)
item484_drain_line=$(grep -n -F 'console_end_input_batch();' kernel/src/kernel.c |
    head -n1 | cut -d: -f1)
if [[ -z $item484_flush_line || -z $item484_drain_line ||
      $item484_flush_line -ge $item484_drain_line ]]; then
    echo "ITEM-484: отложенный реплей не предшествует барьеру входной пачки" >&2
    exit 1
fi
# The end-of-turn flush stays for records and diagnostics.
if [[ $(grep -c -F 'console_flush_deferred();' kernel/src/kernel.c) -lt 2 ]]; then
    echo "ITEM-484: флаш конца оборота пропал" >&2
    exit 1
fi

# ------------------------------------------------------------- host proofs --
for item484_proof in \
    test_the_pointer_appears_on_mouse_readiness \
    test_many_batches_close_with_one_drain \
    test_the_deferred_echo_replays_inside_the_batch; do
    grep -Fq "static void $item484_proof(void) {" tests/console-host.c || {
        echo "ITEM-484: нет host-пробы $item484_proof" >&2
        exit 1
    }
    grep -Fq "$item484_proof();" tests/console-host.c || {
        echo "ITEM-484: host-проба $item484_proof не вызывается" >&2
        exit 1
    }
done
for item484_claim in \
    'a click without movement does not create the cursor' \
    'readiness paints the arrow exactly once' \
    'the first movement repaints the arrow in its new place' \
    'three batches close with exactly one drain' \
    'the deferred echo replays inside the input batch'; do
    grep -Fq -- "$item484_claim" tests/console-host.c || {
        echo "ITEM-484: host-проба не утверждает:" "$item484_claim" >&2
        exit 1
    }
done

# --------------------------------------------------------------- QEMU proof --
# The QEMU proof keeps requiring the budgets, the contiguous exact echo and the
# unchanged loss counters; ITEM-484 does not weaken its witnesses.
for item484_qemu in \
    'a turn served more keyboard events than the keyboard budget' \
    'a turn drew the pointer more times than the pointer budget' \
    'the keyboard service gap is outside the budget' \
    'the USB keyboard was never echoed' \
    'a pointer completion left the endpoint without a TRB'; do
    grep -Fq -- "$item484_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-484: QEMU-проба не требует:" "$item484_qemu" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents --
for item484_doc in \
    'ITEM-484' \
    'console_pointer_ready' \
    'console_note_echo_paint'; do
    grep -Fq -- "$item484_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-484:" \
            "$item484_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-484' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-484" >&2
    exit 1
}
grep -Fq 'ITEM-484' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-484" >&2
    exit 1
}
