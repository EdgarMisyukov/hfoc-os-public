#!/usr/bin/env bash
# ITEM-467: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The input path works under a network flood, and a silent pointer names why.
#
# The owner's 2026-09-21 run on `ec6eb9db` showed two things at once:
#
#   * the keyboard answered late and lost characters while the wire carried a
#     stream (`ARP` requests and hundreds of dropped frames), because the main
#     loop fed every queued frame to the stack before it reached the input path
#     again;
#   * the pointer was bound but every counter stayed at zero (`reports=0
#     transfers=1 moves=0 presses=0`), because the interrupt transfer had been
#     armed once and never completed - the receiver's three-byte boot mouse
#     answer against the driver's four-byte request, with no short-packet
#     completion;
#   * and none of it could be measured: the counters rode at the end of the
#     ~500-character heartbeat, which the durable journal cut off before them.
#
# This slice pins the three fixes:
#
#   * `xhci_trb_interrupt_in()` sets ISP, so a short packet completes the
#     transfer and it is re-armed; the reason a silent device is silent is one
#     pure word (`xhci_no_report_reason()`);
#   * the receive path processes a bounded number of frames per loop turn
#     (`APN_RX_BUDGET_PER_PASS`), so the next turn reaches the keyboard;
#   * the input facts get their own short, journal-safe record
#     (`[PRODUCTION-INPUT]`) with `kbd_reports`, `kbd_dropped`,
#     `kbd_gap_ms_max`, `iter_max_us`, the pointer's `pending`/`reposted`/
#     `last_ms` and the machine's own reason.
for input_under_load_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    kernel/src/appliance_production_network_x86.c \
    tests/xhci-host.c \
    scripts/hfoc-qemu-netload-peer.py \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $input_under_load_file ]] || {
        echo "ITEM-467: отсутствует $input_under_load_file" >&2
        exit 1
    }
done

# 1. The interrupt transfer asks for a short-packet event, and the silent
#    device's reason is a pure decision the host proof drives.
grep -Fq 'struct xhci_trb xhci_trb_interrupt_in(' \
    kernel/include/xhci_trb.h || {
    echo "ITEM-467: TRB короткого пакета не объявлен" >&2
    exit 1
}
grep -Fq 'const char *xhci_no_report_reason(' kernel/include/xhci_trb.h || {
    echo "ITEM-467: причина молчания не объявлена чистым решением" >&2
    exit 1
}
grep -Fq 'trb.dword[3] |= XHCI_TRB_ISP;' kernel/src/xhci_trb.c || {
    echo "ITEM-467: короткий пакет не завершает передачу" >&2
    exit 1
}
grep -Fq 'trb.dword[3] |= XHCI_TRB_IOC;' kernel/src/xhci_trb.c || {
    echo "ITEM-467: короткий пакет потерял событие завершения" >&2
    exit 1
}
for input_under_load_reason in \
    '"INTERRUPT_PENDING"' \
    '"COMPLETION_WITHOUT_REPORT"' \
    '"NOT_ARMED"'; do
    grep -Fq -- "$input_under_load_reason" kernel/src/xhci_trb.c || {
        echo "ITEM-467: причина молчания не называет форму:" \
            "$input_under_load_reason" >&2
        exit 1
    }
done

# 2. The driver arms every role with the short-packet TRB and keeps the
#    re-arm/completion counters the record publishes.
grep -Fq 'trb = xhci_trb_interrupt_in(1U,' kernel/src/xhci.c || {
    echo "ITEM-467: драйвер не вооружает передачу TRB короткого пакета" >&2
    exit 1
}
for input_under_load_driver in \
    'uint32_t repost_count;' \
    'uint64_t last_report_ticks;' \
    'device->last_report_ticks = timer_ticks();' \
    '++device->repost_count;'; do
    grep -Fq -- "$input_under_load_driver" kernel/src/xhci.c || {
        echo "ITEM-467: драйвер не ведёт счётчики указателя:" \
            "$input_under_load_driver" >&2
        exit 1
    }
done
for input_under_load_accessor in \
    'bool xhci_keyboard_pending(void);' \
    'uint32_t xhci_keyboard_repost_count(void);' \
    'uint32_t xhci_keyboard_last_report_ms(void);' \
    'bool xhci_mouse_pending(void);' \
    'uint32_t xhci_mouse_repost_count(void);' \
    'uint32_t xhci_mouse_last_report_ms(void);' \
    'const char *xhci_mouse_no_report_reason(void);'; do
    grep -Fq -- "$input_under_load_accessor" kernel/include/xhci.h || {
        echo "ITEM-467: доступ к состоянию передачи не объявлен:" \
            "$input_under_load_accessor" >&2
        exit 1
    }
done

# 3. The input facts have their own short durable record with every required
#    field, and it is emitted on the heartbeat's own cadence.
grep -Fq 'static void log_production_input_record(void)' kernel/src/kernel.c || {
    echo "ITEM-467: нет отдельной записи ввода" >&2
    exit 1
}
grep -Fq 'log_production_input_record();' kernel/src/kernel.c || {
    echo "ITEM-467: отдельная запись ввода не вызывается" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE("[PRODUCTION-INPUT] kbd_reports=");' \
    kernel/src/kernel.c || {
    echo "ITEM-467: запись ввода не публикует отчёты клавиатуры" >&2
    exit 1
}
for input_under_load_record in \
    '" kbd_dropped="' \
    '" kbd_gap_ms_max="' \
    '" kbd_pending="' \
    '" kbd_reposted="' \
    '" mouse_reports="' \
    '" mouse_moves="' \
    '" mouse_presses="' \
    '" mouse_pending="' \
    '" mouse_reposted="' \
    '" mouse_last_ms="' \
    '" mouse_reason="' \
    '" iter_max_us="'; do
    grep -Fq -- "$input_under_load_record" kernel/src/kernel.c || {
        echo "ITEM-467: запись ввода не несёт поле:" \
            "$input_under_load_record" >&2
        exit 1
    }
done

# 4. One loop turn processes a bounded number of received frames, so the input
#    path is reached again within a bounded time.
grep -Fq '#define APN_RX_BUDGET_PER_PASS 16U' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-467: у приёма нет бюджета на проход" >&2
    exit 1
}
# ITEM-494: the Intel backend was added as a third receive path, and it applies
# the same bound - so the count of bounded receive passes is three now, not two.
test "$(grep -Fc 'budget < APN_RX_BUDGET_PER_PASS' \
    kernel/src/appliance_production_network_x86.c)" = 3 || {
    echo "ITEM-467/494: бюджет приёма применён не ко всем бэкендам" >&2
    exit 1
}

# 5. The host proof drives the short-packet TRB and every reason shape.
for input_under_load_host in \
    'test_interrupt_short_packet_and_reason();' \
    '"an interrupt IN transfer completes on a short packet too"' \
    '"a plain Normal TRB does not ask for the short-packet event"' \
    '"an unfinished transfer names itself as pending"' \
    '"a completion that carried no report names that"' \
    '"a device that was never armed names that"' \
    '"a report outranks a pending flag"'; do
    grep -Fq -- "$input_under_load_host" tests/xhci-host.c || {
        echo "ITEM-467: host-проба не утверждает:" \
            "$input_under_load_host" >&2
        exit 1
    }
done

# 6. The QEMU proof floods the receive path while typing and moving the mouse,
#    and requires the machine's own numbers.
# shellcheck disable=SC2016  # the proof's own literal text is what the search matches
for input_under_load_qemu in \
    'hfoc-qemu-netload-peer.py' \
    '-netdev "socket,id=netload,listen=127.0.0.1:$NETLOAD_PORT"' \
    "-device 'usb-mouse,bus=xhci.0'" \
    'move_mouse(24, 17)' \
    'click_mouse()' \
    '[PRODUCTION-FRAME] class=ARP_REQUEST' \
    'int(input_fields["kbd_dropped"]) != 0' \
    'int(input_fields["kbd_gap_ms_max"]) > 500' \
    'int(input_fields["iter_max_us"]) > 500000' \
    'int(input_fields["mouse_reports"]) <= 0' \
    'int(input_fields["mouse_moves"]) <= 0' \
    'int(input_fields["mouse_presses"]) <= 0'; do
    grep -Fq -- "$input_under_load_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-467: QEMU-проба не требует:" \
            "$input_under_load_qemu" >&2
        exit 1
    }
done
grep -Fq 'GUEST_MAC = bytes((0x52, 0x54, 0x00, 0x12, 0x34, 0x56))' \
    scripts/hfoc-qemu-netload-peer.py || {
    echo "ITEM-467: peer не называет MAC гостя" >&2
    exit 1
}
grep -Fq 'BROADCAST' scripts/hfoc-qemu-netload-peer.py || {
    echo "ITEM-467: peer не шлёт широковещательные кадры" >&2
    exit 1
}
# The socket backend has no DHCP server, and without an address the production
# network stays offline and classifies nothing; the peer has to give one while
# it floods.
grep -Fq 'def dhcp_request(' scripts/hfoc-qemu-netload-peer.py || {
    echo "ITEM-467: peer не разбирает DHCP гостя" >&2
    exit 1
}
grep -Fq 'def build_dhcp_reply(' scripts/hfoc-qemu-netload-peer.py || {
    echo "ITEM-467: peer не отвечает гостю по DHCP" >&2
    exit 1
}
grep -Fq 'leased = False' scripts/hfoc-qemu-netload-peer.py || {
    echo "ITEM-467: peer не ведёт состояние аренды" >&2
    exit 1
}

# 7. The item is recorded where a maintainer looks.
for input_under_load_doc in \
    'ITEM-467' \
    '[PRODUCTION-INPUT]' \
    'INTERRUPT_PENDING'; do
    grep -Fq -- "$input_under_load_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-467:" \
            "$input_under_load_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-467' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-467" >&2
    exit 1
}
grep -Fq 'ITEM-467' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-467" >&2
    exit 1
}
