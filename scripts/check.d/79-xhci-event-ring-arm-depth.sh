#!/usr/bin/env bash
# ITEM-473: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The event ring absorbs the pointer's burst, and the depth has a handle
# (СРОЧНО-23, round 12).
#
# The owner's round-11 run after ITEM-472 read `mouse_reports=24` and
# `completed=40` frozen from t=110 to t=211 while `armed=16`, `mse=0`,
# `no_trb=0`, `state=READY` and the controller still reported `running=1
# halted=0 error=0`.  The worker's code diagnosis: `XHCI_EVENT_TRBS` was
# sixteen - exactly the mouse's `XHCI_MOUSE_ARM_DEPTH` - so one burst of
# completions filled the event ring, the controller raised Event Ring Full (21)
# and stopped publishing events until the dequeue pointer moved.  The driver
# counted completion code 21 only on transfer events, and Event Ring Full
# arrives as a Host Controller Event TRB the decoder did not know; the error
# was published nowhere.
#
# This item makes the event ring its whole 4 KiB page (`XHCI_EVENT_TRBS` 256),
# adds the static assertions that every armed queue stays below the ring's
# capacity, adds the `ARM` console handle (`ARM` shows, `ARM <1..16>` sets the
# mouse depth), and publishes the reason: the controller `ringfull=`/`host_events=`
# counts, the per-role `[INPUT-CODE]` record with every completion code's count
# and the Endpoint State read from the device context, and the input tail's
# `mouse_arm_depth=`/`ringfull=` fields the QEMU proof requires to stay at the
# sixteen-deep queue with no event ring full.

for item473_file in \
    kernel/include/xhci.h \
    kernel/include/xhci_trb.h \
    kernel/src/xhci.c \
    kernel/src/xhci_trb.c \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/xhci-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item473_file ]] || {
        echo "ITEM-473: отсутствует $item473_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# The event ring is its whole page, and no armed queue can fill it.
grep -Fq '#define XHCI_EVENT_TRBS 256U' kernel/include/xhci_trb.h || {
    echo "ITEM-473: кольцо событий не расширено до страницы" >&2
    exit 1
}
for item473_ring_assert in \
    'XHCI_MOUSE_ARM_DEPTH < XHCI_EVENT_TRBS' \
    'XHCI_REPORT_SLOTS < XHCI_EVENT_TRBS' \
    'XHCI_ARM_DEPTH < XHCI_EVENT_TRBS' \
    '(size_t)XHCI_EVENT_TRBS * XHCI_TRB_BYTES <=' \
    'XHCI_OFF_ERST - XHCI_OFF_EVENT_RING'; do
    grep -Fq -- "$item473_ring_assert" kernel/src/xhci.c || {
        echo "ITEM-473: статическая проверка кольца не содержит:" \
            "$item473_ring_assert" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- B --
# `ARM` shows and sets the mouse's arm depth; the clamp is the driver's.
for item473_arm_impl in \
    '#define XHCI_ARM_DEPTH_MIN 1U' \
    '#define XHCI_ARM_DEPTH_MAX 16U' \
    'uint8_t xhci_arm_depth_clamp(uint32_t requested) {'; do
    grep -Fq -- "$item473_arm_impl" kernel/include/xhci_trb.h \
        kernel/src/xhci_trb.c || {
        echo "ITEM-473: предел глубины не объявлен:" \
            "$item473_arm_impl" >&2
        exit 1
    }
done
for item473_arm_driver in \
    'static uint8_t xhci_mouse_arm_depth_override;' \
    'xhci_mouse_arm_depth_override = xhci_arm_depth_clamp(requested);' \
    'uint32_t xhci_mouse_arm_depth(void) {' \
    'uint32_t xhci_mouse_arm_depth(void);' \
    'void xhci_set_mouse_arm_depth(uint32_t requested);'; do
    grep -Fq -- "$item473_arm_driver" kernel/src/xhci.c \
        kernel/include/xhci.h || {
        echo "ITEM-473: ручка глубины не реализована:" \
            "$item473_arm_driver" >&2
        exit 1
    }
done
grep -Fq 'static void console_arm_command(const char *line) {' \
    kernel/src/console.c || {
    echo "ITEM-473: консоль не умеет ARM" >&2
    exit 1
}
grep -Fq 'console_command_prefix(input_line, "arm ")' kernel/src/console.c || {
    echo "ITEM-473: консоль не принимает ARM <n>" >&2
    exit 1
}
grep -Fq 'xhci_set_mouse_arm_depth(value);' kernel/src/console.c || {
    echo "ITEM-473: ARM не задаёт глубину драйверу" >&2
    exit 1
}
grep -Fq '"[INPUT-ARM] mouse_arm_depth="' kernel/src/console.c || {
    echo "ITEM-473: ответ ARM не называет глубину" >&2
    exit 1
}
grep -Fq ' ARM' kernel/src/console.c || {
    echo "ITEM-473: HELP не называет ARM" >&2
    exit 1
}

# ---------------------------------------------------------------------- C --
# The Host Controller Event decodes, Event Ring Full is counted, and the
# diagnostics publish the reason and the endpoint state.
for item473_event_impl in \
    'XHCI_TRB_HOST_CONTROLLER_EVENT = 37' \
    'case XHCI_TRB_HOST_CONTROLLER_EVENT:'; do
    grep -Fq -- "$item473_event_impl" kernel/include/xhci_trb.h \
        kernel/src/xhci_trb.c || {
        echo "ITEM-473: Host Controller Event не разобран:" \
            "$item473_event_impl" >&2
        exit 1
    }
done
grep -Fq 'return "HOST_CONTROLLER_EVENT";' kernel/src/xhci_trb.c || {
    echo "ITEM-473: Host Controller Event не назван" >&2
    exit 1
}
for item473_ring_count in \
    'if (event->type == XHCI_TRB_HOST_CONTROLLER_EVENT) {' \
    '++xhci->controller_event_count;' \
    '++xhci->event_ring_full_events;' \
    '" host_events="' \
    '" ringfull="' \
    '" ep_state="' \
    'xhci_endpoint_state_string(' \
    'xhci_device_endpoint_state(device)' \
    '"[INPUT-CODE] role="' \
    'uint32_t xhci_event_ring_full_count(void) {' \
    'uint32_t xhci_controller_event_count(void) {'; do
    grep -Fq -- "$item473_ring_count" kernel/src/xhci.c || {
        echo "ITEM-473: причина не публикуется:" \
            "$item473_ring_count" >&2
        exit 1
    }
done
for item473_state_impl in \
    'uint32_t xhci_endpoint_state(const void *device_context, size_t context_size,' \
    'const char *xhci_endpoint_state_string(uint32_t state) {' \
    'return "Running";' \
    'return "Halted";' \
    'return "Stopped";'; do
    grep -Fq -- "$item473_state_impl" kernel/include/xhci_trb.h \
        kernel/src/xhci_trb.c || {
        echo "ITEM-473: состояние эндпоинта не читается:" \
            "$item473_state_impl" >&2
        exit 1
    }
done
grep -Fq 'uint32_t xhci_event_ring_full_count(void);' kernel/include/xhci.h || {
    echo "ITEM-473: доступ к счётчику кольца не объявлен" >&2
    exit 1
}

# The input tail carries the depth and the ring-full count; the core record
# keeps its 320-byte shape and does not grow these fields.
grep -Fq '" mouse_arm_depth="' kernel/src/kernel.c || {
    echo "ITEM-473: [PRODUCTION-INPUT-TAIL] не несёт глубину" >&2
    exit 1
}
grep -Fq '" ringfull="' kernel/src/kernel.c || {
    echo "ITEM-473: [PRODUCTION-INPUT-TAIL] не несёт ringfull" >&2
    exit 1
}
grep -Fq 'xhci_event_ring_full_count()' kernel/src/kernel.c || {
    echo "ITEM-473: запись ввода не читает счётчик кольца" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
# The QEMU proof requires a full turn of arm depth with no event ring full.
grep -Fq '[PRODUCTION-INPUT-TAIL]' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-473: QEMU-проба не читает хвост записи ввода" >&2
    exit 1
}
grep -Fq 'int(tail_fields["mouse_arm_depth"]) < 16' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-473: QEMU-проба не требует глубину 16" >&2
    exit 1
}
grep -Fq 'int(tail_fields["ringfull"]) != 0' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-473: QEMU-проба не требует отсутствие event ring full" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
for item473_claim in \
    'the deepest armable queue is below the event ring'"'"'s capacity' \
    'ARM 0 answers the minimum depth, never a disabled endpoint' \
    'ARM 3 is the ITEM-469 depth that moved the pointer' \
    'Event Ring Full arrives as completion code 21' \
    'the endpoint state is read from the context at its DCI' \
    'a running endpoint is named' \
    'ARM shows the mouse'"'"'s arm depth' \
    'ARM <n> sets and names the mouse'"'"'s arm depth' \
    'ARM clamps a depth above the mouse maximum'; do
    grep -Fq -- "$item473_claim" tests/xhci-host.c tests/console-host.c || {
        echo "ITEM-473: host-проба не утверждает:" "$item473_claim" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents --
for item473_doc in \
    'ITEM-473' \
    'XHCI_EVENT_TRBS' \
    'ARM' \
    'ringfull=' \
    'ep_state='; do
    grep -Fq -- "$item473_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-473:" \
            "$item473_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-473' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-473" >&2
    exit 1
}
grep -Fq 'ITEM-473' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-473" >&2
    exit 1
}
