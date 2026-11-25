#!/usr/bin/env bash
# ITEM-645: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The 2026-11-22 Toshiba capture is the receive-stopped shape: all 16 RX
# descriptors owned by the NIC and IntrStatus `0xA5 -> 0xF5` with no further
# frame.  A part that runs out of receive descriptors sets RxOverflow and does
# not restart until the bit is cleared; the driver acknowledged the status word
# once at bring-up and never again.  This slice pins the runtime acknowledge,
# the drained-ring rule, the published history and both host proofs.
# shellcheck disable=SC2016

item645_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item645_root" || exit 1

for item645_file in \
    kernel/include/rtl8168.h \
    kernel/src/rtl8168.c \
    kernel/include/appliance_production_network_boot.h \
    kernel/src/appliance_production_network_io_record.c \
    kernel/src/appliance_production_network_x86.c \
    tests/rtl8168-host.c \
    tests/net-phy-journal-host.c \
    tests/network-io-record-host.c \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item645_file ]] || {
        echo "ITEM-645: отсутствует $item645_file" >&2
        exit 1
    }
done

# 1. The driver publishes the event bits and the acknowledge.
grep -Fq 'enum rtl8168_status rtl8168_intr_ack_events(' \
    kernel/include/rtl8168.h || {
    echo "ITEM-645: нет объявления rtl8168_intr_ack_events" >&2
    exit 1
}
grep -Fq '#define RTL8168_INTR_RX_OVERFLOW 0x0010U' \
    kernel/include/rtl8168.h || {
    echo "ITEM-645: нет бита RxOverflow" >&2
    exit 1
}
grep -Fq '#define RTL8168_INTR_RX_FIFO_OVERFLOW 0x0040U' \
    kernel/include/rtl8168.h || {
    echo "ITEM-645: нет бита RxFIFOOver" >&2
    exit 1
}
grep -Fq '#define RTL8168_INTR_RX_STOPPED' kernel/include/rtl8168.h || {
    echo "ITEM-645: нет пары receive-stopped" >&2
    exit 1
}

# 2. The acknowledge is one write-one-to-clear write of the observed mask.
grep -Fq 'rtl8168_read16(io, RTL8168_REG_INTR_STATUS, &status)' \
    kernel/src/rtl8168.c || {
    echo "ITEM-645: acknowledge не читает IntrStatus" >&2
    exit 1
}
grep -Fq 'rtl8168_write16(io, RTL8168_REG_INTR_STATUS, ack)' \
    kernel/src/rtl8168.c || {
    echo "ITEM-645: acknowledge не пишет наблюдённую маску" >&2
    exit 1
}
grep -Fq 'write-one-to-clear' kernel/src/rtl8168.c || {
    echo "ITEM-645: нет фиксации W1C-семантики" >&2
    exit 1
}
grep -Fq 'status == RTL8168_INTR_STATUS_CLEAR' kernel/src/rtl8168.c || {
    echo "ITEM-645: all-ones status не распознан как мёртвая шина" >&2
    exit 1
}
# 3. The production poll pass proves a free descriptor before it clears the
#    stopped pair, and always acknowledges after the drain.
grep -Fq 'descriptors_available = true;' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-645: poll не фиксирует свободный дескриптор" >&2
    exit 1
}
grep -Fq 'apn_rtl_ack_events(descriptors_available);' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-645: poll не подтверждает события после дренажа" >&2
    exit 1
}
grep -Fq 'RTL8168_INTR_RX_STOPPED' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-645: адаптер не различает receive-stopped" >&2
    exit 1
}

# 4. The live record keeps the overflow/recovery history.
grep -Fq 'uint32_t rx_overflow_events;' \
    kernel/include/appliance_production_network_boot.h || {
    echo "ITEM-645: счётчики не несут историю overflow" >&2
    exit 1
}
grep -Fq 'uint32_t rx_overflow_acks;' \
    kernel/include/appliance_production_network_boot.h || {
    echo "ITEM-645: счётчики не несут историю recovery" >&2
    exit 1
}
grep -Fq 'counters->rx_diag.rx_overflow_events = apn.rtl.rx_overflow;' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-645: адаптер не публикует историю overflow" >&2
    exit 1
}
grep -Fq '"rx_ovf"' kernel/src/appliance_production_network_io_record.c || {
    echo "ITEM-645: [NET-RX] не печатает rx_ovf" >&2
    exit 1
}
grep -Fq '"rx_ovf_ack"' kernel/src/appliance_production_network_io_record.c || {
    echo "ITEM-645: [NET-RX] не печатает rx_ovf_ack" >&2
    exit 1
}

# 5. Both host proofs drive the real code, and the fixture models W1C.
grep -Fq 'test_intr_events_ack' tests/rtl8168-host.c || {
    echo "ITEM-645: нет host-пробы acknowledge драйвера" >&2
    exit 1
}
grep -Fq 'test_rx_progress_after_overflow_ack' tests/rtl8168-host.c || {
    echo "ITEM-645: нет host-пробы прогресса после acknowledge" >&2
    exit 1
}
grep -Fq 'fixture->intr_status & (uint16_t)~value' tests/rtl8168-host.c || {
    echo "ITEM-645: fixture не моделирует W1C" >&2
    exit 1
}
grep -Fq 'test_adapter_acknowledges_receive_stop' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-645: нет host-пробы политики адаптера" >&2
    exit 1
}
grep -Fq 'rx_ovf_ack=0x1' tests/network-io-record-host.c || {
    echo "ITEM-645: нет host-пробы полей [NET-RX]" >&2
    exit 1
}

# 6. The item is recorded without taking over the moving heading.
item645_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '**ITEM-645 — the poll loop acknowledges a stopped receiver.**' \
    "$item645_queue" || {
    echo "ITEM-645: очередь не называет текущий пункт" >&2
    exit 1
}
grep -A 4 '^## Item ITEM-644$' "$item645_queue" | grep -Fq 'ITEM-644' || {
    echo "ITEM-645: очередь потеряла исторический раздел ITEM-644" >&2
    exit 1
}
grep -Fq 'ITEM-645' docs/ROADMAP.md || {
    echo "ITEM-645: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
