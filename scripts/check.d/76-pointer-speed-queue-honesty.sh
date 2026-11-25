#!/usr/bin/env bash
# ITEM-470: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The pointer's speed, the mouse queue that covers a loop turn, and the literal
# loop-turn counter (СРОЧНО-20, round 9).
#
# The owner's 2026-09-22 run after ITEM-469 confirmed the keyboard
# (`kbd_mse=0`, `kbd_dropped=0`, `kbd_armed=3`) and named the pointer: "the
# mouse is slow".  Three facts were measured on the machine and in the code:
#
#   * `pointer_move()` adds a report's delta 1:1 in pixels, so on the owner's
#     3840x2160 screen, whose automatic glyph scale is 3, the interface is three
#     times bigger while the arrow travels the same pixels - it crawls relative
#     to what the operator is aiming at;
#   * the pointer's endpoint is 1 ms (`binterval=1`) but the role kept the
#     keyboard's three armed transfers, so a burst faster than ~300 reports/s
#     exhausts the queue and the device's NAKs are dropped silently - `mouse_mse`
#     is a controller state and never sees them;
#   * `passes` (~99/s, two service passes per turn) and `iter_us` (~82 ms,
#     ~12/s) disagreed eight-fold in the same window with no counter that could
#     say which one described the main loop.
#
# This item scales the pointer by `console_scale` (the number `[FB]` already
# publishes, with a soft acceleration for fast reports and scale 1 left exactly
# as it was), deepens the mouse role to sixteen armed transfers in a sixteen-slot
# pool (the keyboard keeps its three), adds a `no_trb` counter for a completion
# that left the endpoint unarmed, and adds an explicit loop-turn counter beside
# `iter_us`.  This slice pins all three and the QEMU proof that reads them.

for item470_file in \
    kernel/include/pointer.h \
    kernel/src/pointer.c \
    kernel/src/console.c \
    kernel/src/xhci.c \
    kernel/include/xhci.h \
    kernel/src/kernel.c \
    kernel/include/console.h \
    tests/pointer-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item470_file ]] || {
        echo "ITEM-470: отсутствует $item470_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# The pointer's pure speed rule: the glyph scale and a bounded, soft
# acceleration, with the scale-one path the identity.
for item470_pointer_symbol in \
    '#define POINTER_ACCEL_MIN_DELTA 8' \
    '#define POINTER_ACCEL_MAX_GAIN 3U' \
    'int32_t pointer_scale_delta(int32_t delta, uint32_t scale);' \
    'bool pointer_move_scaled(struct pointer_state *state, int32_t dx, int32_t dy,'; do
    grep -Fq -- "$item470_pointer_symbol" kernel/include/pointer.h || {
        echo "ITEM-470: модуль указателя не объявляет:" \
            "$item470_pointer_symbol" >&2
        exit 1
    }
done
for item470_pointer_impl in \
    'scale <= 1U' \
    'accel = 1U + (uint32_t)(magnitude / POINTER_ACCEL_MIN_DELTA);' \
    'if (accel > POINTER_ACCEL_MAX_GAIN) {' \
    'scaled = (int64_t)delta * (int64_t)scale * (int64_t)gain' \
    'return pointer_move_gained(state, dx, dy, bounds, scale, 1U);' \
    'return pointer_move(state, pointer_apply_delta(dx, scale, gain),' \
    'pointer_apply_delta(dy, scale, gain), bounds);'; do
    grep -Fq -- "$item470_pointer_impl" kernel/src/pointer.c || {
        echo "ITEM-470: реализация масштаба не содержит:" \
            "$item470_pointer_impl" >&2
        exit 1
    }
done
grep -Fq 'pointer_apply_delta(event->dx, console_scale, console_pointer_gain)' \
    kernel/src/console.c || {
    echo "ITEM-470: консоль не применяет масштаб к дельте" >&2
    exit 1
}
grep -Fq 'console_scale, console_pointer_gain)' kernel/src/console.c || {
    echo "ITEM-470: консоль не передаёт свой масштаб указателю" >&2
    exit 1
}
for item470_pointer_claim in \
    'scale one is the exact movement it always was' \
    'a scaled move multiplies the report delta by the glyph scale' \
    'a fast scaled report is carried farther than the glyph scale alone' \
    'a scaled move past the edge still stops at the last pixel' \
    'the scaled entry point keeps scale one bit for bit'; do
    grep -Fq -- "$item470_pointer_claim" tests/pointer-host.c || {
        echo "ITEM-470: проба указателя не утверждает:" \
            "$item470_pointer_claim" >&2
        exit 1
    }
done
# The console's own case proves it passes its mode scale to the pointer.
grep -Fq 'the console moves the arrow by the report scaled to the mode' \
    tests/console-host.c || {
    echo "ITEM-470: проба консоли не утверждает масштаб указателя" >&2
    exit 1
}

# ---------------------------------------------------------------------- B --
# The mouse queue covers one loop turn: sixteen slots, the keyboard keeps three,
# the interrupt ring is deep enough for sixteen outstanding TRBs and the
# per-role data region holds the pool.
for item470_queue_define in \
    '#define XHCI_REPORT_SLOTS 16U' \
    '#define XHCI_ARM_DEPTH 3U' \
    '#define XHCI_MOUSE_ARM_DEPTH 16U' \
    '#define XHCI_INTERRUPT_RING_TRBS 64U' \
    '#define XHCI_DEVICE_DATA_STRIDE 0x3000U' \
    '#define XHCI_DMA_MAX_BYTES (4U * 1024U * 1024U)'; do
    grep -Fq -- "$item470_queue_define" kernel/src/xhci.c || {
        echo "ITEM-470: очередь мыши не объявлена:" \
            "$item470_queue_define" >&2
        exit 1
    }
done
for item470_queue_impl in \
    'static uint8_t xhci_role_arm_depth(uint8_t role) {' \
    'return role == XHCI_HID_ROLE_MOUSE ? XHCI_MOUSE_ARM_DEPTH' \
    'device->armed_count < xhci_role_arm_depth(device->role)' \
    'XHCI_INTERRUPT_RING_TRBS, 1U);' \
    'uint32_t no_trb_count;' \
    '++device->no_trb_count;'; do
    grep -Fq -- "$item470_queue_impl" kernel/src/xhci.c || {
        echo "ITEM-470: очередь мыши не реализована:" \
            "$item470_queue_impl" >&2
        exit 1
    }
done
for item470_queue_accessor in \
    'uint32_t xhci_keyboard_no_trb_count(void);' \
    'uint32_t xhci_mouse_no_trb_count(void);'; do
    grep -Fq -- "$item470_queue_accessor" kernel/include/xhci.h || {
        echo "ITEM-470: доступ к окну без TRB не объявлен:" \
            "$item470_queue_accessor" >&2
        exit 1
    }
    grep -Fq -- "${item470_queue_accessor%;}" kernel/src/xhci.c || {
        echo "ITEM-470: доступ к окну без TRB не реализован:" \
            "$item470_queue_accessor" >&2
        exit 1
    }
done
grep -Fq '" no_trb="' kernel/src/xhci.c || {
    echo "ITEM-470: [INPUT-DIAG] не несёт окно без TRB" >&2
    exit 1
}
grep -Fq '" mouse_no_trb="' kernel/src/kernel.c || {
    echo "ITEM-470: [PRODUCTION-INPUT] не несёт mouse_no_trb" >&2
    exit 1
}

# ---------------------------------------------------------------------- C --
# The literal loop-turn counter, incremented where the turn is timed and
# published beside `iter_us`.
grep -Fq 'uint64_t kernel_loop_iteration_count(void);' \
    kernel/include/console.h || {
    echo "ITEM-470: счётчик оборотов не объявлен" >&2
    exit 1
}
grep -Fq 'uint64_t kernel_loop_iteration_count(void) {' kernel/src/kernel.c || {
    echo "ITEM-470: счётчик оборотов не реализован" >&2
    exit 1
}
grep -Fq '++production_iteration_count;' kernel/src/kernel.c || {
    echo "ITEM-470: счётчик оборотов не инкрементируется в главном цикле" >&2
    exit 1
}
grep -Fq '" loops="' kernel/src/xhci.c || {
    echo "ITEM-470: [LOOP-DIAG] не несёт счётчик оборотов" >&2
    exit 1
}
grep -Fq '" loops="' kernel/src/kernel.c || {
    echo "ITEM-470: [PRODUCTION-INPUT] не несёт счётчик оборотов" >&2
    exit 1
}
grep -Fq 'xhci_diag_u64(line, kernel_loop_iteration_count());' \
    kernel/src/xhci.c || {
    echo "ITEM-470: [LOOP-DIAG] не публикует счётчик оборотов" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
# The QEMU input proof parses the once-a-second record and requires the pointer
# queue, the unarmed-window counter and the loop-turn counter.
grep -Fq 'int(input_fields["mouse_armed"]) < 16' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-470: QEMU-проба не требует очередь мыши" >&2
    exit 1
}
grep -Fq 'int(input_fields["mouse_no_trb"]) != 0' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-470: QEMU-проба не требует отсутствия окна без TRB" >&2
    exit 1
}
grep -Fq 'int(input_fields["loops"]) <= 0' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-470: QEMU-проба не требует счётчик оборотов" >&2
    exit 1
}
grep -Fq 'int(input_fields["mouse_mse"]) != 0' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-470: QEMU-проба не требует mouse_mse=0" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item470_doc in \
    'ITEM-470' \
    'pointer_scale_delta' \
    'XHCI_MOUSE_ARM_DEPTH' \
    'mouse_no_trb' \
    'loops='; do
    grep -Fq -- "$item470_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-470:" \
            "$item470_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-470' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-470" >&2
    exit 1
}
grep -Fq 'ITEM-470' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-470" >&2
    exit 1
}
