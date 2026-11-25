#!/usr/bin/env bash
# ITEM-451: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The refusal names itself.
#
# The 2026-09-19 hardware run published a READY backend, a live link, an
# acquired DHCP lease and a bound panel while the owner's laptop could neither
# ping the machine nor open its panel, and the only receive line was
# `rx_ok=5 rx_drop=407` - one number for a peer's ARP request, an unrelated
# IPv6 frame and a refused ICMP echo alike.  This slice pins the record that
# replaces that silence:
#
#   * the three flags a frame has to pass before lwIP sees it - `bound`,
#     `proto_published`, `netif_live` - are published in `[PRODUCTION-NET]`
#     and take part in that record's change decision, so the record cannot
#     stay silent when the path opens;
#   * the frame path counts by class: ARP requests and replies received, ARP
#     requests and replies sent, ICMP echo requests and other ICMP received,
#     ICMP echo replies sent, TCP SYNs received and the SYN+ACKs and RSTs sent
#     in answer, other IPv4 and every non-ARP/IPv4 ethertype; `malformed` is
#     reserved for a frame that could not be parsed, so unrelated LAN traffic
#     can never be read as a broken receiver again;
#   * the heartbeat `[PRODUCTION-LOOP]` publishes every one of those fields,
#     because that is the line a durable log carries;
#   * the QEMU proof on the leased address answers a panel SYN from the same
#     segment with a SYN+ACK, refuses a closed port with an RST, and then
#     requires the machine's own record to name each class it saw;
#   * the host proof feeds one frame of every class and requires the counters,
#     the refusal of an unrelated ethertype as a class rather than as
#     `malformed`, and the accessor's null refusal.
for network_names_file in \
    kernel/include/hfoc_lwip_proto.h \
    kernel/src/hfoc_lwip_proto.c \
    kernel/include/appliance_production_network_boot.h \
    kernel/src/appliance_production_network_x86.c \
    kernel/src/appliance_boot_log.c \
    kernel/src/kernel.c \
    tests/lwip-arp-icmp-host.c \
    scripts/test-qemu-production-network.sh; do
    [[ -f $network_names_file ]] || {
        echo "ITEM-451: отсутствует $network_names_file" >&2
        exit 1
    }
done

# One frame-path owner publishes one copy of the per-class counters.
for network_names_symbol in \
    hfoc_lwip_proto_counters \
    struct\ hfoc_lwip_proto_counters \
    arp_request_rx \
    arp_reply_rx \
    arp_request_tx \
    arp_reply_tx \
    icmp_echo_rx \
    icmp_other_rx \
    icmp_reply_tx \
    tcp_syn_rx \
    tcp_syn_ack_tx \
    tcp_rst_tx \
    ipv4_other_rx \
    non_ipv4_rx \
    malformed_rx \
    rate_limited_rx \
    accepted_rx; do
    grep -Fq "$network_names_symbol" \
        kernel/include/hfoc_lwip_proto.h || {
        echo "ITEM-451: адаптер не публикует $network_names_symbol" >&2
        exit 1
    }
done

# Every class the record claims to separate is really counted, on the way in
# and on the way out; a class claimed but never incremented would be a silent
# zero again.
for network_names_counter in \
    '++proto->arp_request_rx;' \
    '++proto->arp_reply_rx;' \
    '++proto->arp_request_tx;' \
    '++proto->icmp_echo_rx;' \
    '++proto->icmp_other_rx;' \
    '++proto->tcp_syn_rx;' \
    '++proto->tcp_syn_ack_tx;' \
    '++proto->tcp_rst_tx;' \
    '++proto->ipv4_other_rx;' \
    '++proto->non_ipv4_rx;' \
    'counters->arp_reply_tx = proto->arp_reply_count;' \
    'counters->icmp_reply_tx = proto->icmp_reply_count;' \
    'counters->malformed_rx = proto->malformed_count;' \
    'counters->accepted_rx = proto->rx_count;'; do
    grep -Fq -- "$network_names_counter" \
        kernel/src/hfoc_lwip_proto.c || {
        echo "ITEM-451: класс не считается: $network_names_counter" >&2
        exit 1
    }
done

# The two refusals keep their own names: an unrelated ethertype is a class,
# and only a frame that cannot be parsed is malformed.
grep -Fq 'ITEM-451: every other ethertype' kernel/src/hfoc_lwip_proto.c || {
    echo "ITEM-451: чужой ethertype не назван классом" >&2
    exit 1
}
grep -Fq 'so unrelated LAN traffic can never be read as a broken appliance' \
    kernel/include/hfoc_lwip_proto.h || {
    echo "ITEM-451: шапка счётчиков не объясняет классы" >&2
    exit 1
}

# The accessor refuses nothing silently: both arguments are checked.
grep -Fq 'if (proto == NULL || counters == NULL) {' \
    kernel/src/hfoc_lwip_proto.c || {
    echo "ITEM-451: чтение счётчиков не отвергает NULL" >&2
    exit 1
}

# The boot record carries the three entry flags, and they take part in the
# change decision - a flag that flips has to republish the record.
for network_names_flag in \
    'NET_RECORD_WRITE(" bound=");' \
    'NET_RECORD_WRITE(" proto_published=");' \
    'NET_RECORD_WRITE(" netif_live=");' \
    'bound == last_bound &&' \
    'proto_published == last_proto_published &&' \
    'netif_live == last_netif_live) {'; do
    grep -Fq -- "$network_names_flag" kernel/src/kernel.c || {
        echo "ITEM-451: запись не публикует флаг: $network_names_flag" >&2
        exit 1
    }
done
for network_names_accessor in \
    appliance_production_network_proto_published \
    appliance_production_network_netif_live; do
    grep -Fq "$network_names_accessor" \
        kernel/include/appliance_production_network.h || {
        echo "ITEM-451: нет доступа к флагу $network_names_accessor" >&2
        exit 1
    }
    grep -Fq "$network_names_accessor" \
        kernel/src/appliance_production_network.c || {
        echo "ITEM-451: флаг $network_names_accessor не реализован" >&2
        exit 1
    }
done

# The heartbeat is the line a durable log carries, so every class is on it.
for network_names_field in \
    'NET_RECORD_WRITE(" arp_req_rx=");' \
    'NET_RECORD_WRITE(" arp_rep_rx=");' \
    'NET_RECORD_WRITE(" arp_rep_tx=");' \
    'NET_RECORD_WRITE(" arp_req_tx=");' \
    'NET_RECORD_WRITE(" icmp_echo_rx=");' \
    'NET_RECORD_WRITE(" icmp_other_rx=");' \
    'NET_RECORD_WRITE(" icmp_rep_tx=");' \
    'NET_RECORD_WRITE(" syn_rx=");' \
    'NET_RECORD_WRITE(" synack_tx=");' \
    'NET_RECORD_WRITE(" rst_tx=");' \
    'NET_RECORD_WRITE(" ipv4_other_rx=");' \
    'NET_RECORD_WRITE(" nonipv4_rx=");' \
    'NET_RECORD_WRITE(" malformed_rx=");' \
    'NET_RECORD_WRITE(" limited_rx=");'; do
    grep -Fq -- "$network_names_field" kernel/src/kernel.c || {
        echo "ITEM-451: heartbeat не публикует $network_names_field" >&2
        exit 1
    }
done

# Both backends read the same adapter, so the counters are filled before the
# backend split and a driver that refused the part still names its traffic.
grep -Fq '(void)hfoc_lwip_proto_counters(&apn.network.proto, &counters->classes);' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-451: счётчики классов не читаются из адаптера" >&2
    exit 1
}

# The first frame of each class is an event, not a clock tick: the record that
# names it is published on the loop turn it first appears, reaches the durable
# log, and is bounded by the class count plus the two refusal events.
for network_names_event in \
    '[PRODUCTION-FRAME] class=' \
    '"ARP_REQUEST"' \
    '"ICMP_ECHO"' \
    '"TCP_SYN"' \
    'class=TCP_REFUSED' \
    '"NON_IPV4"' \
    'log_production_frame_classes();' \
    'production_frame_class_published'; do
    grep -Fq -- "$network_names_event" kernel/src/kernel.c || {
        echo "ITEM-451: событие класса не публикуется: $network_names_event" >&2
        exit 1
    }
done
grep -Fq '"[PRODUCTION-FRAME]",' kernel/src/appliance_boot_log.c || {
    echo "ITEM-451: запись класса не попадает в durable-журнал" >&2
    exit 1
}

# The QEMU proof opens the panel from the same segment and then requires the
# record to name it; a proof that only echoed would not have caught the defect
# the owner found, and a proof that waited for the PIT-driven heartbeat would
# fail on a loaded runner.
for network_names_proof in \
    'def build_tcp_segment(' \
    'def wait_tcp_reply(' \
    'def wait_frame_class(' \
    'the appliance never named %s ([PRODUCTION-FRAME] class=%s)' \
    'the appliance never answered the panel SYN to 443' \
    'the panel refused the panel SYN with a TCP RST' \
    'proto_published' ; do
    grep -Fq -- "$network_names_proof" \
        scripts/test-qemu-production-network.sh || {
        echo "ITEM-451: QEMU-проба не утверждает: $network_names_proof" >&2
            exit 1
        }
done
for network_names_class in TCP_SYN TCP_REFUSED ARP_REQUEST ICMP_ECHO; do
    grep -Fq -- "\"$network_names_class\"" \
        scripts/test-qemu-production-network.sh || {
        echo "ITEM-451: QEMU-проба не ждёт класс $network_names_class" >&2
        exit 1
    }
done

# The host proof feeds one frame of every class.
for network_names_host in \
    'static void test_named_classes(void)' \
    'test_named_classes();' \
    'counters.arp_request_rx == 1U' \
    'counters.arp_reply_tx == 1U' \
    'counters.icmp_echo_rx == 1U' \
    'counters.icmp_other_rx == 1U' \
    'counters.non_ipv4_rx == 1U' \
    'counters.tcp_syn_rx == 1U' \
    'counters.tcp_rst_tx == 1U' \
    'counters.ipv4_other_rx == 1U' \
    'counters.accepted_rx == 5U' \
    'hfoc_lwip_proto_counters(NULL, &counters)' ; do
    grep -Fq -- "$network_names_host" tests/lwip-arp-icmp-host.c || {
        echo "ITEM-451: host-проба не утверждает: $network_names_host" >&2
        exit 1
    }
done
