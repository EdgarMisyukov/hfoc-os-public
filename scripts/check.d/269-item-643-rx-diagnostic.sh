#!/usr/bin/env bash
# ITEM-643: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The live-outage report needs the receive path's register/descriptor state,
# not only cumulative frame counters.  This slice pins the read-only driver
# snapshot, its generic record and both host proofs; the actual MMIO values
# can only be proven on the Toshiba.
# shellcheck disable=SC2016

item643_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item643_root" || exit 1

for item643_file in \
    kernel/include/rtl8168.h \
    kernel/src/rtl8168.c \
    kernel/include/appliance_production_network_boot.h \
    kernel/include/appliance_production_network_io_record.h \
    kernel/src/appliance_production_network_io_record.c \
    kernel/src/appliance_production_network_x86.c \
    kernel/src/kernel.c \
    tests/rtl8168-host.c \
    tests/network-io-record-host.c \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item643_file ]] || {
        echo "ITEM-643: отсутствует $item643_file" >&2
        exit 1
    }
done

# 1. The driver snapshot exists and is a read-only composition.
grep -Fq 'enum rtl8168_status rtl8168_rx_diagnostic_read(' \
    kernel/include/rtl8168.h || {
    echo "ITEM-643: нет объявления rtl8168_rx_diagnostic_read" >&2
    exit 1
}
grep -Fq 'rtl8168_read16(io, RTL8168_REG_INTR_STATUS' kernel/src/rtl8168.c || {
    echo "ITEM-643: снимок не читает IntrStatus" >&2
    exit 1
}
grep -Fq 'IntrStatus is read but never' kernel/src/rtl8168.c || {
    echo "ITEM-643: снимок не фиксирует IntrStatus без acknowledge" >&2
    exit 1
}
grep -Fq 'RTL8168_DESC_OWN' kernel/src/rtl8168.c || {
    echo "ITEM-643: снимок не считает OWN-биты дескрипторов" >&2
    exit 1
}
if awk '/^enum rtl8168_status rtl8168_rx_diagnostic_read\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/rtl8168.c |
    grep -Fq 'rtl8168_write'; then
    echo "ITEM-643: read-only снимок пишет в MMIO" >&2
    exit 1
fi

# 2. The generic counters carry the snapshot and the RTL adapter fills it.
grep -Fq 'struct appliance_production_network_rx_diagnostic rx_diag;' \
    kernel/include/appliance_production_network_boot.h || {
    echo "ITEM-643: счётчики не несут rx_diag" >&2
    exit 1
}
grep -Fq 'rtl8168_rx_diagnostic_read(' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-643: RTL adapter не заполняет снимок" >&2
    exit 1
}

# 3. The shared status/heartbeat publish the [NET-RX] record.
grep -Fq 'appliance_production_network_rx_record_line(' \
    kernel/src/appliance_production_network_io_record.c || {
    echo "ITEM-643: [NET-RX] не композируется" >&2
    exit 1
}
grep -Fq 'appliance_production_network_rx_record_line(' kernel/src/kernel.c || {
    echo "ITEM-643: kernel не публикует [NET-RX]" >&2
    exit 1
}

# 4. Both host proofs drive the production code and the record contract.
grep -Fq 'test_rx_diagnostic' tests/rtl8168-host.c || {
    echo "ITEM-643: нет host-пробы драйвера" >&2
    exit 1
}
grep -Fq 'test_rx_available' tests/network-io-record-host.c || {
    echo "ITEM-643: нет host-пробы [NET-RX]" >&2
    exit 1
}
# 5. The item is recorded in the queue and the roadmap without taking over the
#    moving `## Current item` heading (slice 52 owns it).
item643_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '**ITEM-643 — the receive path publishes a live outage snapshot.**' \
    "$item643_queue" || {
    echo "ITEM-643: очередь не называет текущий пункт" >&2
    exit 1
}
grep -A 4 '^## Item ITEM-642$' "$item643_queue" | grep -Fq 'ITEM-642' || {
    echo "ITEM-643: очередь потеряла исторический раздел ITEM-642" >&2
    exit 1
}
grep -Fq 'ITEM-643' docs/ROADMAP.md || {
    echo "ITEM-643: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
