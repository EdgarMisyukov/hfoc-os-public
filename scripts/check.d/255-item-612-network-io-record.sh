#!/usr/bin/env bash
# ITEM-612: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.
#
# The owner's 2026-11-15 Toshiba export proved link-up and a DHCP lease while
# the peer's ARP/ICMP/TCP never reached the listener.  The counters that locate
# that loss were only in the ten-second heartbeat, not in `diag save`, so the
# snapshot could not say whether the peer's request arrived.  This slice pins
# the one composition that now publishes them through every shared path.

for item612_file in \
    kernel/include/appliance_production_network_io_record.h \
    kernel/src/appliance_production_network_io_record.c \
    tests/network-io-record-host.c \
    scripts/test-qemu-production-network.sh \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item612_file ]] || {
        echo "ITEM-612: отсутствует $item612_file" >&2
        exit 1
    }
done

for item612_record in \
    '[NET-IO] available=' \
    '[NET-FRAME] available=1' \
    'reason=NO_BACKEND' \
    'tx_sub' \
    'tx_recl' \
    'tx_lost' \
    'tx_full' \
    'tx_rst' \
    'tx_err' \
    'rx_ok' \
    'rx_drop' \
    'rx_err' \
    'rx_arm' \
    'arp_req_rx' \
    'arp_rep_rx' \
    'arp_req_tx' \
    'arp_rep_tx' \
    'icmp_echo_rx' \
    'icmp_other_rx' \
    'icmp_rep_tx' \
    'syn_rx' \
    'synack_tx' \
    'rst_tx' \
    'accepted_rx'; do
    grep -Fq -- "$item612_record" \
        kernel/src/appliance_production_network_io_record.c || {
        echo "ITEM-612: композиция не публикует:" "$item612_record" >&2
        exit 1
    }
done
grep -Fq 'appliance_production_network_io_record.h' kernel/src/kernel.c || {
    echo "ITEM-612: status report не включает запись счётчиков" >&2
    exit 1
}
grep -Fq 'appliance_production_network_io_record_line(' kernel/src/kernel.c ||
    {
        echo "ITEM-612: status report не публикует [NET-IO]" >&2
        exit 1
    }
grep -Fq 'appliance_production_network_frame_record_line(' \
    kernel/src/kernel.c || {
    echo "ITEM-612: status report не публикует [NET-FRAME]" >&2
    exit 1
}
grep -Fq 'appliance_production_network_mac_text(' kernel/src/kernel.c || {
    echo "ITEM-612: [NET-ADDR] не называет MAC активного netif" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX' \
    kernel/include/appliance_production_network_io_record.h || {
    echo "ITEM-612: граница MAC не зафиксирована" >&2
    exit 1
}
for item612_qemu in \
    'io_line = wait_console_record("[NET-IO]")' \
    'frame_line = wait_console_record("[NET-FRAME]")' \
    '"[NET-IO]"' \
    '"[NET-FRAME]"' \
    'tx_sub=' \
    'rx_arm=' \
    'arp_req_rx=' \
    'accepted_rx='; do
    grep -Fq -- "$item612_qemu" scripts/test-qemu-production-network.sh || {
        echo "ITEM-612: production QEMU proof не требует:" "$item612_qemu" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the single-quoted text is the Makefile's own words
grep -Fq '$(MAKE) test-network-io-record-host' Makefile || {
    echo "ITEM-612: host proof не зарегистрирован в suite" >&2
    exit 1
}
