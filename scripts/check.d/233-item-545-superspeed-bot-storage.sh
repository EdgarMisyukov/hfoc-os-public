#!/usr/bin/env bash
# ITEM-545: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The owner's 2026-10-17 hardware evidence: the Toshiba L875 boots the live
# Linux from the SuperSpeed stick in the blue port (`lsusb -t`: Bus 004, 5000M,
# Mass Storage, `usb-storage`, device `24a9:205a`), while the HFOC kernel that
# ran on the same machine bound its USB 2 devices and reported
# `[INSTALL-MEDIUM] state=UNAVAILABLE reason=NO_DEVICE` for the stick.  The
# stick's own descriptors name a SuperSpeed BOT interface whose bulk endpoints
# each carry an endpoint companion with `bMaxBurst = 3`, and the same shape is
# what QEMU's `usb-bot` declares (`wMaxPacketSize = 1024`, `bMaxBurst = 15`).
#
# Two facts this slice pins, both of which were missing before the item:
#   * a device on a USB 3 port is only visible after its link finishes
#     training, and Linux's `xhci_start()` polls the roothubs for 500 ms
#     (`run_graceperiod`) for exactly that reason.  The driver's post-reset
#     window returned as soon as *one* port reported a connect - the full-speed
#     mouse on the USB 2 half answered in milliseconds - so the SuperSpeed port
#     beside it was believed empty on its first read.  The window now ends only
#     when no port that samples a connect is left pending.
#   * the SuperSpeed endpoint companion (USB 3.0 section 9.6.7) is read, and
#     its `bMaxBurst` reaches the xHCI endpoint context's Max Burst Size field
#     (xHCI 1.1 section 6.2.3.5).  The parser used to step over descriptor type
#     0x30, so every SuperSpeed endpoint context said "one packet per burst".
#
# The QEMU half is the positive proof the item asks for: a SuperSpeed BOT disk
# on the USB3 half of the controller is enumerated, bound and its marked sector
# 0 read, and the bound record names the burst the endpoint context carried.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item545_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item545_root" || exit 1

item545_files=(
    kernel/include/xhci_trb.h
    kernel/include/usb_storage.h
    kernel/src/xhci_trb.c
    kernel/src/xhci.c
    kernel/src/usb_storage.c
    tests/xhci-host.c
    tests/usb-storage-host.c
    scripts/test-qemu-usb-two-controllers.sh
    docs/ROADMAP.md
)
for item545_file in "${item545_files[@]}"; do
    [[ -f $item545_file ]] || {
        echo "ITEM-545: отсутствует $item545_file" >&2
        exit 1
    }
done

# 1. The controller's window waits for every port that can still report a
#    connect, and it is the 500 ms Linux gives a just-started controller.
for item545_pin in \
    'bool xhci_port_connect_pending(uint32_t portsc);' \
    '#define XHCI_PORT_TRAINING_MS 500U' \
    'bool xhci_port_connect_pending(uint32_t portsc) {' \
    'return xhci_port_link_samples_connect(xhci_port_link_state(portsc));' \
    'for (uint32_t waited = 0U; waited < XHCI_PORT_TRAINING_MS;' \
    'if (xhci_port_connect_pending(' \
    'if (!pending) {'; do
    case $item545_pin in
    'bool xhci_port_connect_pending(uint32_t portsc);'|\
    '#define XHCI_PORT_TRAINING_MS 500U')
        item545_file=kernel/include/xhci_trb.h
        ;;
    'bool xhci_port_connect_pending(uint32_t portsc) {'|\
    'return xhci_port_link_samples_connect(xhci_port_link_state(portsc));')
        item545_file=kernel/src/xhci_trb.c
        ;;
    *)
        item545_file=kernel/src/xhci.c
        ;;
    esac
    grep -Fq -- "$item545_pin" "$item545_file" || {
        echo "ITEM-545: $item545_file не несёт '$item545_pin'" >&2
        exit 1
    }
done
# The old early exit - the window cancelled by the first connect anywhere - is
# gone, and the per-port debounce window is unchanged.
if grep -A 6 -F 'static void xhci_ports_settle_after_reset(void) {' \
        kernel/src/xhci.c | grep -Fq '& XHCI_PORTSC_CCS) != 0U) {'; then
    echo "ITEM-545: окно контроллера по-прежнему отменяет первый connect" >&2
    exit 1
fi
grep -Fq -- '#define XHCI_PORT_SETTLE_MS 100U' kernel/include/xhci_trb.h || {
    echo "ITEM-545: окно debounce порта ослаблено или потеряно" >&2
    exit 1
}

# 2. The endpoint companion is read, and the burst reaches the endpoint
#    context.  The plain builder keeps programming zero for every endpoint that
#    declares none, and the storage path uses the burst variant.
for item545_pin in \
    'endpoint_type == 0x30U' \
    'found.bulk_in_max_burst = bytes[endpoint_offset + 2U];' \
    'found.bulk_out_max_burst = bytes[endpoint_offset + 2U];' \
    'uint8_t bulk_in_max_burst;' \
    'uint8_t bulk_out_max_burst;'; do
    grep -Fq -- "$item545_pin" kernel/src/usb_storage.c kernel/include/usb_storage.h || {
        echo "ITEM-545: парсер не несёт '$item545_pin'" >&2
        exit 1
    }
done
grep -Fq -- 'xhci_endpoint_context_burst(' kernel/src/xhci.c || {
    echo "ITEM-545: storage не программирует Max Burst Size" >&2
    exit 1
}
grep -Fq -- 'config->bulk_in_max_burst' kernel/src/xhci.c || {
    echo "ITEM-545: контекст bulk IN не получает burst" >&2
    exit 1
}
grep -Fq -- 'config->bulk_out_max_burst' kernel/src/xhci.c || {
    echo "ITEM-545: контекст bulk OUT не получает burst" >&2
    exit 1
}
grep -Fq -- '((uint32_t)max_burst << 16)' kernel/src/xhci_trb.c || {
    echo "ITEM-545: Max Burst Size не пишется в dword 4 (биты 23:16)" >&2
    exit 1
}
grep -Fq -- 'average_trb_length, 0U);' kernel/src/xhci_trb.c || {
    echo "ITEM-545: обычный построитель контекста потерял нулевой burst" >&2
    exit 1
}
# The bound record names what the context carried, so the next hardware run
# shows whether the device declared a burst at all.
grep -Fq -- 'XHCI_PUBLISH(" burst_in=");' kernel/src/xhci.c || {
    echo "ITEM-545: запись не называет burst_in" >&2
    exit 1
}
grep -Fq -- 'XHCI_PUBLISH(" burst_out=");' kernel/src/xhci.c || {
    echo "ITEM-545: запись не называет burst_out" >&2
    exit 1
}

# 3. Both host proofs drive the new halves: the parser against a SuperSpeed
#    configuration with two companions, the context against the burst and the
#    pending-port rule against the two port shapes.
for item545_pin in \
    'test_parser_reads_the_superspeed_companion();' \
    'config.bulk_in_max_burst == 3U &&' \
    'test_parser_defaults_the_burst_without_a_companion();' \
    'test_parser_keeps_the_companion_with_its_own_endpoint();'; do
    grep -Fq -- "$item545_pin" tests/usb-storage-host.c || {
        echo "ITEM-545: host-проба парсера не несёт '$item545_pin'" >&2
        exit 1
    }
done
for item545_pin in \
    'test_port_connect_pending();' \
    'test_endpoint_context_burst();' \
    'xhci_endpoint_context_burst(entry, XHCI_EP_TYPE_BULK_IN, 1024U, 0U,' \
    '((value >> 16) & 0xFFU) == 15U'; do
    grep -Fq -- "$item545_pin" tests/xhci-host.c || {
        echo "ITEM-545: host-проба контекста не несёт '$item545_pin'" >&2
        exit 1
    }
done

# 4. The QEMU proof carries phase E: the SuperSpeed BOT disk is forced onto the
#    first physical port (the USB3 half), and the proof requires the binding,
#    the burst the companion declared and the marked sector the bulk-IN path
#    read - with the host's marker still untouched afterwards.
item545_probe=scripts/test-qemu-usb-two-controllers.sh
for item545_pin in \
    "-device 'usb-bot,id=ssbot,bus=x0.0,port=1'" \
    "-device 'scsi-hd,bus=ssbot.0,drive=ssstick'" \
    'speed=SUPER role=STORAGE state=BOUND reason=OK' \
    'the SuperSpeed stick did not land on the USB3 half' \
    'the bound record names no endpoint burst' \
    'burst_in=(\d+) burst_out=(\d+)' \
    'the SuperSpeed companion'"'"'s burst was not programmed' \
    'marker = b"HFOC-USB3-BOT"' \
    'the host'"'"'s marked sector is untouched' \
    'A SuperSpeed BOT stick on the USB3 half binds, carries its companion burst and reads its marked sector'; do
    grep -Fq -- "$item545_pin" "$item545_probe" || {
        echo "ITEM-545: QEMU-проба не несёт '$item545_pin'" >&2
        exit 1
    }
done
# The port is pinned, not left to QEMU's attach order: the same device also
# supports USB 2, and a USB 2 port would prove nothing about the USB3 half.
if grep -Fq -- "-device 'usb-bot,id=ssbot,bus=x0.0'" "$item545_probe"; then
    echo "ITEM-545: QEMU-проба больше не закрепляет порт USB3" >&2
    exit 1
fi

# 5. ITEM-547 has its own history section now, this item's own record survives as a
#    history section beside ITEM-544's, and the roadmap records it.
item545_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 3 '^## Item ITEM-547$' "$item545_queue" | grep -Fq 'ITEM-547' || {
    echo "ITEM-545: очередь потеряла исторический раздел ITEM-547" >&2
    exit 1
}
grep -Fq '## Item ITEM-544' "$item545_queue" || {
    echo "ITEM-545: очередь потеряла исторический раздел ITEM-544" >&2
    exit 1
}
grep -A 3 '^## Item ITEM-545$' "$item545_queue" | grep -Fq 'ITEM-545' || {
    echo "ITEM-545: очередь потеряла исторический раздел ITEM-545" >&2
    exit 1
}
grep -Fq 'ITEM-545' docs/ROADMAP.md || {
    echo "ITEM-545: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
