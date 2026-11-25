#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
CONFIG=${1:-"$ROOT/kernel/include/lwipopts.h"}
SYMBOL_SOURCE=${2:-}
LWIP_INCLUDE="$ROOT/third_party/lwip/src/include"
KERNEL_INCLUDE="$ROOT/kernel/include"
ARCH_CC="$KERNEL_INCLUDE/arch/cc.h"

fail() {
    echo "lwIP configuration verification failed: $*" >&2
    exit 1
}

[[ -f "$CONFIG" ]] || fail "missing config: $CONFIG"
[[ -f "$ARCH_CC" ]] || fail "missing compiler port header: $ARCH_CC"
[[ -f "$LWIP_INCLUDE/lwip/opt.h" ]] || fail "missing vendored lwIP headers"
[[ -f "$ROOT/kernel/include/hfoc_lwip.h" ]] || {
    fail "missing HFOC allocator boundary header: hfoc_lwip.h"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_time.h" ]] || {
    fail "missing HFOC monotonic time boundary header: hfoc_lwip_time.h"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_pbuf.h" ]] || {
    fail "missing HFOC pbuf/packet ownership boundary header: hfoc_lwip_pbuf.h"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_netif.h" ]] || {
    fail "missing HFOC netif lifecycle boundary header: hfoc_lwip_netif.h"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_proto.h" ]] || {
    fail "missing HFOC ARP/ICMP protocol boundary header: hfoc_lwip_proto.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_proto.c" ]] || {
    fail "missing HFOC ARP/ICMP protocol boundary source: hfoc_lwip_proto.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp.h" ]] || {
    fail "missing HFOC raw TCP listener boundary header: hfoc_lwip_tcp.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp.c" ]] || {
    fail "missing HFOC raw TCP listener boundary source: hfoc_lwip_tcp.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" ]] || {
    fail "missing HFOC TCP connection boundary header: hfoc_lwip_tcp_conn.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" ]] || {
    fail "missing HFOC TCP connection boundary source: hfoc_lwip_tcp_conn.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" ]] || {
    fail "missing HFOC four-slot connection pool boundary header: hfoc_lwip_tcp_pool.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" ]] || {
    fail "missing HFOC four-slot connection pool boundary source: hfoc_lwip_tcp_pool.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" ]] || {
    fail "missing HFOC connection deadline scheduler boundary header: hfoc_lwip_tcp_deadline.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" ]] || {
    fail "missing HFOC connection deadline scheduler boundary source: hfoc_lwip_tcp_deadline.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_backlog.h" ]] || {
    fail "missing HFOC listener backlog admission boundary header: hfoc_lwip_tcp_backlog.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" ]] || {
    fail "missing HFOC listener backlog admission boundary source: hfoc_lwip_tcp_backlog.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" ]] || {
    fail "missing HFOC RX backpressure boundary header: hfoc_lwip_tcp_rx_backpressure.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" ]] || {
    fail "missing HFOC RX backpressure boundary source: hfoc_lwip_tcp_rx_backpressure.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" ]] || {
    fail "missing HFOC TX backpressure boundary header: hfoc_lwip_tcp_tx_backpressure.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" ]] || {
    fail "missing HFOC TX backpressure boundary source: hfoc_lwip_tcp_tx_backpressure.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_quarantine.h" ]] || {
    fail "missing HFOC stale callback quarantine boundary header: hfoc_lwip_tcp_quarantine.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_quarantine.c" ]] || {
    fail "missing HFOC stale callback quarantine boundary source: hfoc_lwip_tcp_quarantine.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_isolation.h" ]] || {
    fail "missing HFOC malformed TCP isolation matrix boundary header: hfoc_lwip_tcp_isolation.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_isolation.c" ]] || {
    fail "missing HFOC malformed TCP isolation matrix boundary source: hfoc_lwip_tcp_isolation.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_close_policy.h" ]] || {
    fail "missing HFOC peer close and local abort policy boundary header: hfoc_lwip_tcp_close_policy.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_close_policy.c" ]] || {
    fail "missing HFOC peer close and local abort policy boundary source: hfoc_lwip_tcp_close_policy.c"
}
[[ -f "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" ]] || {
    fail "missing HFOC listener drain and restart boundary header: hfoc_lwip_tcp_drain_restart.h"
}
[[ -f "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" ]] || {
    fail "missing HFOC listener drain and restart boundary source: hfoc_lwip_tcp_drain_restart.c"
}
grep -Fq 'hfoc_lwip_tcp_drain_restart_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare drain init"
grep -Fq 'hfoc_lwip_tcp_drain_restart_start' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare drain start"
grep -Fq 'hfoc_lwip_tcp_drain_restart_stop_admission' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare stop admission"
grep -Fq 'hfoc_lwip_tcp_drain_restart_step' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare drain step"
grep -Fq 'hfoc_lwip_tcp_drain_restart_finish_drain' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare finish drain"
grep -Fq 'hfoc_lwip_tcp_drain_restart_restart' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_drain_restart.h" \
    || fail "hfoc_lwip_tcp_drain_restart.h must declare restart"
grep -Fq 'hfoc_lwip_tcp_drain_restart_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement drain init"
grep -Fq 'hfoc_lwip_tcp_drain_restart_start' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement drain start"
grep -Fq 'hfoc_lwip_tcp_drain_restart_stop_admission' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement stop admission"
grep -Fq 'hfoc_lwip_tcp_drain_restart_step' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement drain step"
grep -Fq 'hfoc_lwip_tcp_drain_restart_finish_drain' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement finish drain"
grep -Fq 'hfoc_lwip_tcp_drain_restart_restart' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_drain_restart.c" \
    || fail "hfoc_lwip_tcp_drain_restart.c must implement restart"
[[ -f "$ROOT/scripts/test-qemu-tcp-listener.sh" ]] || {
    fail "missing TCP listener QEMU proof script: test-qemu-tcp-listener.sh"
}
grep -Fq 'HFOC_TCP_LISTENER_QEMU_TEST' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing TCP listener QEMU proof gate in virtio_net_frame_qemu.c"
}
grep -Fq 'VNF_TCP_LISTENER_PORT 443U' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing TCP listener proof port bound in virtio_net_frame_qemu.c"
}
grep -Fq 'VNF_TCP_MAX_CONNECTIONS 4U' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing TCP listener proof connection bound in virtio_net_frame_qemu.c"
}
[[ -f "$ROOT/scripts/test-qemu-network-resource.sh" ]] || {
    fail "missing network resource QEMU proof script: test-qemu-network-resource.sh"
}
grep -Fq 'HFOC_NETWORK_RESOURCE_QEMU_TEST' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing network resource QEMU proof gate in virtio_net_frame_qemu.c"
}
grep -Fq 'VNF_NETRES_PACKET_COUNT 8U' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing network resource packet pool bound in virtio_net_frame_qemu.c"
}
grep -Fq 'VNF_NETRES_IDLE_TIMEOUT_MS 500U' \
    "$ROOT/kernel/src/virtio_net_frame_qemu.c" || {
    fail "missing network resource bounded idle timeout in virtio_net_frame_qemu.c"
}

CONFIG_DIR=$(cd -- "$(dirname -- "$CONFIG")" && pwd -P)
CONFIG_BASE=$(basename -- "$CONFIG")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [[ "$CONFIG_BASE" != "lwipopts.h" ]]; then
    cp "$CONFIG" "$WORK/lwipopts.h"
    CONFIG_DIR="$WORK"
fi

cat >"$WORK/probe.c" <<'EOF'
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "hfoc_lwip.h"
#include "hfoc_lwip_time.h"
#include "hfoc_lwip_pbuf.h"
#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_proto.h"
#include "hfoc_lwip_tcp.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_backlog.h"
#include "hfoc_lwip_tcp_rx_backpressure.h"
#include "hfoc_lwip_tcp_tx_backpressure.h"
#include "hfoc_lwip_tcp_quarantine.h"
#include "hfoc_lwip_tcp_isolation.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_drain_restart.h"

#ifndef HFOC_LWIP_CONFIG_VERSION
#error HFOC_LWIP_CONFIG_VERSION is required
#endif
#if HFOC_LWIP_CONFIG_VERSION != 1
#error HFOC_LWIP_CONFIG_VERSION must be 1
#endif
#if HFOC_LWIP_TIME_VERSION != 1
#error HFOC_LWIP_TIME_VERSION must be 1
#endif
#if HFOC_LWIP_TIME_SLOT_COUNT != 16
#error HFOC_LWIP_TIME_SLOT_COUNT must be 16
#endif
#if HFOC_LWIP_PBUF_VERSION != 1
#error HFOC_LWIP_PBUF_VERSION must be 1
#endif
#if HFOC_LWIP_PBUF_SLOT_COUNT != 8
#error HFOC_LWIP_PBUF_SLOT_COUNT must be 8
#endif
#if HFOC_LWIP_PBUF_CHAIN_MAX != 4
#error HFOC_LWIP_PBUF_CHAIN_MAX must be 4
#endif
#if HFOC_LWIP_PBUF_TOT_LEN_MAX != 2048
#error HFOC_LWIP_PBUF_TOT_LEN_MAX must be 2048
#endif
#if HFOC_LWIP_NETIF_VERSION != 1
#error HFOC_LWIP_NETIF_VERSION must be 1
#endif
#if HFOC_LWIP_NETIF_MTU != 1500
#error HFOC_LWIP_NETIF_MTU must be 1500
#endif
#if HFOC_LWIP_PROTO_VERSION != 1U
#error HFOC_LWIP_PROTO_VERSION must be 1U
#endif
#if HFOC_LWIP_PROTO_FRAME_MAX != 1518U
#error HFOC_LWIP_PROTO_FRAME_MAX must be 1518U
#endif
#if HFOC_LWIP_PROTO_ICMP_BUDGET != 4U
#error HFOC_LWIP_PROTO_ICMP_BUDGET must be 4U
#endif
#if HFOC_LWIP_PROTO_ICMP_WINDOW_MS != 1000U
#error HFOC_LWIP_PROTO_ICMP_WINDOW_MS must be 1000U
#endif
#if HFOC_LWIP_TCP_VERSION != 1U
#error HFOC_LWIP_TCP_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_BACKLOG != 4U
#error HFOC_LWIP_TCP_BACKLOG must be 4U
#endif
#if HFOC_LWIP_TCP_CONN_VERSION != 1U
#error HFOC_LWIP_TCP_CONN_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_CONN_POLL_INTERVAL != 1U
#error HFOC_LWIP_TCP_CONN_POLL_INTERVAL must be 1U
#endif
#if HFOC_LWIP_TCP_POOL_VERSION != 1U
#error HFOC_LWIP_TCP_POOL_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_POOL_SLOT_COUNT != 4U
#error HFOC_LWIP_TCP_POOL_SLOT_COUNT must be 4U
#endif
#if HFOC_LWIP_TCP_DEADLINE_VERSION != 1U
#error HFOC_LWIP_TCP_DEADLINE_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT != 4U
#error HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT must be 4U
#endif
#if HFOC_LWIP_TCP_BACKLOG_VERSION != 1U
#error HFOC_LWIP_TCP_BACKLOG_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_BACKLOG_CAPACITY != 4U
#error HFOC_LWIP_TCP_BACKLOG_CAPACITY must be 4U
#endif
#if HFOC_LWIP_TCP_QUARANTINE_VERSION != 1U
#error HFOC_LWIP_TCP_QUARANTINE_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT != 4U
#error HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT must be 4U
#endif
#if HFOC_LWIP_TCP_ISOLATION_VERSION != 1U
#error HFOC_LWIP_TCP_ISOLATION_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT != 4U
#error HFOC_LWIP_TCP_ISOLATION_SLOT_COUNT must be 4U
#endif
#if HFOC_LWIP_TCP_CLOSE_POLICY_VERSION != 1U
#error HFOC_LWIP_TCP_CLOSE_POLICY_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT != 4U
#error HFOC_LWIP_TCP_CLOSE_POLICY_SLOT_COUNT must be 4U
#endif
#if HFOC_LWIP_TCP_CLOSE_POLICY_REASON_COUNT != 6U
#error HFOC_LWIP_TCP_CLOSE_POLICY_REASON_COUNT must be 6U
#endif
#if HFOC_LWIP_TCP_DRAIN_RESTART_VERSION != 1U
#error HFOC_LWIP_TCP_DRAIN_RESTART_VERSION must be 1U
#endif
#if HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_COUNT != 5U
#error HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_COUNT must be 5U
#endif
#if HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MIN != 1U
#error HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MIN must be 1U
#endif
#if HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MAX != (UINT32_MAX / 4U)
#error HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MAX must be UINT32_MAX / 4
#endif

#define HFOC_REQUIRE_VALUE(option, expected) \
    _Static_assert((option) == (expected), #option " must be " #expected)

HFOC_REQUIRE_VALUE(NO_SYS, 1);
HFOC_REQUIRE_VALUE(LWIP_TIMERS, 1);
HFOC_REQUIRE_VALUE(LWIP_TCPIP_CORE_LOCKING, 0);
HFOC_REQUIRE_VALUE(SYS_LIGHTWEIGHT_PROT, 0);
HFOC_REQUIRE_VALUE(LWIP_CALLBACK_API, 1);
HFOC_REQUIRE_VALUE(LWIP_IPV4, 1);
HFOC_REQUIRE_VALUE(LWIP_ARP, 1);
HFOC_REQUIRE_VALUE(LWIP_ICMP, 1);
HFOC_REQUIRE_VALUE(LWIP_TCP, 1);
HFOC_REQUIRE_VALUE(TCP_LISTEN_BACKLOG, 1);
/*
 * ITEM-449: the forward relay is the second listener this profile has ever
 * had, and each relay session is two PCBs (the admitted client and the target
 * it dialled) beside the management serving session.  The frozen profile moves
 * with the item, deliberately and in one place: the listener that serves 443
 * and the listener that serves the declared forward port, plus the connection
 * PCBs both of them need.
 */
HFOC_REQUIRE_VALUE(MEMP_NUM_TCP_PCB, 7);
HFOC_REQUIRE_VALUE(MEMP_NUM_TCP_PCB_LISTEN, 3);
HFOC_REQUIRE_VALUE(LWIP_UDP, 0);
HFOC_REQUIRE_VALUE(LWIP_RAW, 0);
HFOC_REQUIRE_VALUE(LWIP_UDPLITE, 0);
HFOC_REQUIRE_VALUE(LWIP_DHCP, 0);
HFOC_REQUIRE_VALUE(LWIP_AUTOIP, 0);
HFOC_REQUIRE_VALUE(LWIP_ACD, 0);
HFOC_REQUIRE_VALUE(LWIP_DNS, 0);
HFOC_REQUIRE_VALUE(LWIP_IGMP, 0);
HFOC_REQUIRE_VALUE(LWIP_ALTCP, 0);
HFOC_REQUIRE_VALUE(LWIP_ALTCP_TLS, 0);
HFOC_REQUIRE_VALUE(LWIP_NETCONN, 0);
HFOC_REQUIRE_VALUE(LWIP_SOCKET, 0);
HFOC_REQUIRE_VALUE(LWIP_NETIF_API, 0);
HFOC_REQUIRE_VALUE(LWIP_NETIF_LOOPBACK, 0);
HFOC_REQUIRE_VALUE(LWIP_HAVE_LOOPIF, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6, 0);
HFOC_REQUIRE_VALUE(LWIP_ICMP6, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6_MLD, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6_DHCP6, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6_FRAG, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6_REASS, 0);
HFOC_REQUIRE_VALUE(LWIP_IPV6_AUTOCONFIG, 0);
HFOC_REQUIRE_VALUE(IP_REASSEMBLY, 0);
HFOC_REQUIRE_VALUE(IP_FRAG, 0);
HFOC_REQUIRE_VALUE(LWIP_CHECKSUM_CTRL_PER_NETIF, 0);
HFOC_REQUIRE_VALUE(LWIP_CHECKSUM_ON_COPY, 0);
HFOC_REQUIRE_VALUE(LWIP_TCP_SACK_OUT, 0);
HFOC_REQUIRE_VALUE(LWIP_WND_SCALE, 0);
HFOC_REQUIRE_VALUE(LWIP_STATS, 0);
HFOC_REQUIRE_VALUE(MEM_CUSTOM_ALLOCATOR, 1);
HFOC_REQUIRE_VALUE(MEMP_MEM_MALLOC, 1);
HFOC_REQUIRE_VALUE(MEM_USE_POOLS, 0);
HFOC_REQUIRE_VALUE(MEMP_USE_CUSTOM_POOLS, 0);
HFOC_REQUIRE_VALUE(LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT, 0);
HFOC_REQUIRE_VALUE(LWIP_SUPPORT_CUSTOM_PBUF, 1);

int main(void)
{
    return 0;
}
EOF

grep -Eq '^#define MEM_CUSTOM_MALLOC[[:space:]]+hfoc_lwip_mem_malloc[[:space:]]*$' \
    "$CONFIG" || fail "MEM_CUSTOM_MALLOC must be hfoc_lwip_mem_malloc"
grep -Eq '^#define MEM_CUSTOM_FREE[[:space:]]+hfoc_lwip_mem_free[[:space:]]*$' \
    "$CONFIG" || fail "MEM_CUSTOM_FREE must be hfoc_lwip_mem_free"
grep -Eq '^#define MEM_CUSTOM_CALLOC[[:space:]]+hfoc_lwip_mem_calloc[[:space:]]*$' \
    "$CONFIG" || fail "MEM_CUSTOM_CALLOC must be hfoc_lwip_mem_calloc"
grep -Eq '^#define LWIP_SUPPORT_CUSTOM_PBUF[[:space:]]+1[[:space:]]*$' \
    "$CONFIG" || fail "LWIP_SUPPORT_CUSTOM_PBUF must be 1"
grep -Eq '^#define LWIP_PBUF_CUSTOM_DATA[[:space:]].*hfoc_lwip_bridge' \
    "$CONFIG" || fail "LWIP_PBUF_CUSTOM_DATA must carry hfoc_lwip_bridge"
grep -Eq '^#define LWIP_PBUF_CUSTOM_DATA_INIT\(p\)' \
    "$CONFIG" || fail "LWIP_PBUF_CUSTOM_DATA_INIT must be defined"
grep -Eq '^#define HFOC_LWIP_PROTO_VERSION[[:space:]]+1U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "HFOC_LWIP_PROTO_VERSION must be 1U"
grep -Eq '^#define HFOC_LWIP_PROTO_FRAME_MAX[[:space:]]+1518U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "HFOC_LWIP_PROTO_FRAME_MAX must be 1518U"
grep -Eq '^#define HFOC_LWIP_PROTO_ICMP_BUDGET[[:space:]]+4U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "HFOC_LWIP_PROTO_ICMP_BUDGET must be 4U"
grep -Eq '^#define HFOC_LWIP_PROTO_ICMP_WINDOW_MS[[:space:]]+1000U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "HFOC_LWIP_PROTO_ICMP_WINDOW_MS must be 1000U"
grep -Fq 'hfoc_lwip_proto_init' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "hfoc_lwip_proto.h must declare proto init"
grep -Fq 'hfoc_lwip_proto_input' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "hfoc_lwip_proto.h must declare proto input"
grep -Fq 'hfoc_lwip_proto_lookup' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "hfoc_lwip_proto.h must declare ARP cache lookup"
grep -Fq 'hfoc_lwip_proto_shutdown' \
    "$ROOT/kernel/include/hfoc_lwip_proto.h" \
    || fail "hfoc_lwip_proto.h must declare proto shutdown"
grep -Eq '^#define HFOC_LWIP_TCP_VERSION[[:space:]]+1U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "HFOC_LWIP_TCP_VERSION must be 1U"
grep -Eq '^#define HFOC_LWIP_TCP_BACKLOG[[:space:]]+4U[[:space:]]*$' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "HFOC_LWIP_TCP_BACKLOG must be 4U"
grep -Fq 'hfoc_lwip_tcp_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "hfoc_lwip_tcp.h must declare listener init"
grep -Fq 'hfoc_lwip_tcp_bind' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "hfoc_lwip_tcp.h must declare listener bind"
grep -Fq 'hfoc_lwip_tcp_listen' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "hfoc_lwip_tcp.h must declare listener listen"
grep -Fq 'hfoc_lwip_tcp_shutdown' \
    "$ROOT/kernel/include/hfoc_lwip_tcp.h" \
    || fail "hfoc_lwip_tcp.h must declare listener shutdown"
grep -Fq 'hfoc_lwip_tcp_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp.c" \
    || fail "hfoc_lwip_tcp.c must implement listener init"
grep -Fq 'hfoc_lwip_tcp_bind' \
    "$ROOT/kernel/src/hfoc_lwip_tcp.c" \
    || fail "hfoc_lwip_tcp.c must implement listener bind"
grep -Fq 'hfoc_lwip_tcp_listen' \
    "$ROOT/kernel/src/hfoc_lwip_tcp.c" \
    || fail "hfoc_lwip_tcp.c must implement listener listen"
grep -Fq 'hfoc_lwip_tcp_shutdown' \
    "$ROOT/kernel/src/hfoc_lwip_tcp.c" \
    || fail "hfoc_lwip_tcp.c must implement listener shutdown"
grep -Fq 'hfoc_lwip_tcp_conn_accept' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection accept"
grep -Fq 'hfoc_lwip_tcp_conn_recv' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection recv"
grep -Fq 'hfoc_lwip_tcp_conn_sent' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection sent"
grep -Fq 'hfoc_lwip_tcp_conn_poll' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection poll"
grep -Fq 'hfoc_lwip_tcp_conn_error' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection error"
grep -Fq 'hfoc_lwip_tcp_conn_close' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection close"
grep -Fq 'hfoc_lwip_tcp_conn_abort' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_conn.h" \
    || fail "hfoc_lwip_tcp_conn.h must declare connection abort"
grep -Fq 'hfoc_lwip_tcp_conn_accept' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection accept"
grep -Fq 'hfoc_lwip_tcp_conn_recv' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection recv"
grep -Fq 'hfoc_lwip_tcp_conn_sent' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection sent"
grep -Fq 'hfoc_lwip_tcp_conn_poll' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection poll"
grep -Fq 'hfoc_lwip_tcp_conn_error' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection error"
grep -Fq 'hfoc_lwip_tcp_conn_close' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection close"
grep -Fq 'hfoc_lwip_tcp_conn_abort' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_conn.c" \
    || fail "hfoc_lwip_tcp_conn.c must implement connection abort"
grep -Fq 'hfoc_lwip_tcp_pool_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool init"
grep -Fq 'hfoc_lwip_tcp_pool_reserve' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool reserve"
grep -Fq 'hfoc_lwip_tcp_pool_publish' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool publish"
grep -Fq 'hfoc_lwip_tcp_pool_close' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool close"
grep -Fq 'hfoc_lwip_tcp_pool_abort' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool abort"
grep -Fq 'hfoc_lwip_tcp_pool_release' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_pool.h" \
    || fail "hfoc_lwip_tcp_pool.h must declare pool release"
grep -Fq 'hfoc_lwip_tcp_pool_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool init"
grep -Fq 'hfoc_lwip_tcp_pool_reserve' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool reserve"
grep -Fq 'hfoc_lwip_tcp_pool_publish' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool publish"
grep -Fq 'hfoc_lwip_tcp_pool_close' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool close"
grep -Fq 'hfoc_lwip_tcp_pool_abort' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool abort"
grep -Fq 'hfoc_lwip_tcp_pool_release' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_pool.c" \
    || fail "hfoc_lwip_tcp_pool.c must implement pool release"
grep -Fq 'hfoc_lwip_tcp_deadline_attach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" \
    || fail "hfoc_lwip_tcp_deadline.h must declare deadline attach"
grep -Fq 'hfoc_lwip_tcp_deadline_touch' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" \
    || fail "hfoc_lwip_tcp_deadline.h must declare deadline touch"
grep -Fq 'hfoc_lwip_tcp_deadline_begin_close' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" \
    || fail "hfoc_lwip_tcp_deadline.h must declare begin-close"
grep -Fq 'hfoc_lwip_tcp_deadline_check' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" \
    || fail "hfoc_lwip_tcp_deadline.h must declare deadline check"
grep -Fq 'hfoc_lwip_tcp_deadline_cancel' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_deadline.h" \
    || fail "hfoc_lwip_tcp_deadline.h must declare deadline cancel"
grep -Fq 'hfoc_lwip_tcp_deadline_attach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" \
    || fail "hfoc_lwip_tcp_deadline.c must implement deadline attach"
grep -Fq 'hfoc_lwip_tcp_deadline_touch' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" \
    || fail "hfoc_lwip_tcp_deadline.c must implement deadline touch"
grep -Fq 'hfoc_lwip_tcp_deadline_begin_close' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" \
    || fail "hfoc_lwip_tcp_deadline.c must implement begin-close"
grep -Fq 'hfoc_lwip_tcp_deadline_check' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" \
    || fail "hfoc_lwip_tcp_deadline.c must implement deadline check"
grep -Fq 'hfoc_lwip_tcp_deadline_cancel' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_deadline.c" \
    || fail "hfoc_lwip_tcp_deadline.c must implement deadline cancel"
grep -Fq 'hfoc_lwip_tcp_backlog_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_backlog.h" \
    || fail "hfoc_lwip_tcp_backlog.h must declare backlog init"
grep -Fq 'hfoc_lwip_tcp_backlog_admit' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_backlog.h" \
    || fail "hfoc_lwip_tcp_backlog.h must declare backlog admit"
grep -Fq 'hfoc_lwip_tcp_backlog_drain' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_backlog.h" \
    || fail "hfoc_lwip_tcp_backlog.h must declare backlog drain"
grep -Fq 'hfoc_lwip_tcp_backlog_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" \
    || fail "hfoc_lwip_tcp_backlog.c must implement backlog init"
grep -Fq 'hfoc_lwip_tcp_backlog_admit' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" \
    || fail "hfoc_lwip_tcp_backlog.c must implement backlog admit"
grep -Fq 'hfoc_lwip_tcp_backlog_drain' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" \
    || fail "hfoc_lwip_tcp_backlog.c must implement backlog drain"
grep -Fq 'tcp_backlog_delayed' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" \
    || fail "hfoc_lwip_tcp_backlog.c must use tcp_backlog_delayed"
grep -Fq 'tcp_backlog_accepted' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_backlog.c" \
    || fail "hfoc_lwip_tcp_backlog.c must use tcp_backlog_accepted"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" \
    || fail "hfoc_lwip_tcp_rx_backpressure.h must declare RX init"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_attach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" \
    || fail "hfoc_lwip_tcp_rx_backpressure.h must declare RX attach"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_receive' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" \
    || fail "hfoc_lwip_tcp_rx_backpressure.h must declare RX receive"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_release' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" \
    || fail "hfoc_lwip_tcp_rx_backpressure.h must declare RX release"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_detach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_rx_backpressure.h" \
    || fail "hfoc_lwip_tcp_rx_backpressure.h must declare RX detach"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must implement RX init"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_attach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must implement RX attach"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_receive' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must implement RX receive"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_release' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must implement RX release"
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_detach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must implement RX detach"
grep -Fq 'tcp_recved' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_rx_backpressure.c" \
    || fail "hfoc_lwip_tcp_rx_backpressure.c must use tcp_recved"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX init"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_attach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX attach"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_submit' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX submit"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_publish' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX publish"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_sent' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX sent"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_detach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_tx_backpressure.h" \
    || fail "hfoc_lwip_tcp_tx_backpressure.h must declare TX detach"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX init"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_attach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX attach"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_submit' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX submit"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_publish' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX publish"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_sent' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX sent"
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_detach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must implement TX detach"
grep -Fq 'tcp_write' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_tx_backpressure.c" \
    || fail "hfoc_lwip_tcp_tx_backpressure.c must use tcp_write"
grep -Fq 'hfoc_lwip_tcp_quarantine_init' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_quarantine.h" \
    || fail "hfoc_lwip_tcp_quarantine.h must declare quarantine init"
grep -Fq 'hfoc_lwip_tcp_quarantine_attach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_quarantine.h" \
    || fail "hfoc_lwip_tcp_quarantine.h must declare quarantine attach"
grep -Fq 'hfoc_lwip_tcp_quarantine_check' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_quarantine.h" \
    || fail "hfoc_lwip_tcp_quarantine.h must declare quarantine check"
grep -Fq 'hfoc_lwip_tcp_quarantine_detach' \
    "$ROOT/kernel/include/hfoc_lwip_tcp_quarantine.h" \
    || fail "hfoc_lwip_tcp_quarantine.h must declare quarantine detach"
grep -Fq 'hfoc_lwip_tcp_quarantine_init' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_quarantine.c" \
    || fail "hfoc_lwip_tcp_quarantine.c must implement quarantine init"
grep -Fq 'hfoc_lwip_tcp_quarantine_attach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_quarantine.c" \
    || fail "hfoc_lwip_tcp_quarantine.c must implement quarantine attach"
grep -Fq 'hfoc_lwip_tcp_quarantine_check' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_quarantine.c" \
    || fail "hfoc_lwip_tcp_quarantine.c must implement quarantine check"
grep -Fq 'hfoc_lwip_tcp_quarantine_detach' \
    "$ROOT/kernel/src/hfoc_lwip_tcp_quarantine.c" \
    || fail "hfoc_lwip_tcp_quarantine.c must implement quarantine detach"

cc -std=gnu11 -fsyntax-only -Wall -Wextra -Werror \
    -I"$CONFIG_DIR" -I"$KERNEL_INCLUDE" -I"$LWIP_INCLUDE" \
    "$WORK/probe.c"

scan_symbol_file() {
    local source=$1
    local symbol
    for symbol in \
        lwip_socket lwip_bind lwip_listen lwip_accept lwip_connect \
        lwip_send lwip_sendto lwip_recv lwip_recvfrom lwip_close \
        lwip_shutdown lwip_getaddrinfo lwip_freeaddrinfo lwip_ioctl \
        lwip_fcntl lwip_select lwip_read lwip_write \
        netconn_ sys_thread_new tcpip_init tcpip_thread tcpip_input \
        dhcp_start dns_gethostbyname dns_setserver \
        ip6_input icmp6_input nd6_input mld6_input altcp_new altcp_alloc; do
        if grep -Eq "(^|[^[:alnum:]_])${symbol}" "$source"; then
            fail "forbidden lwIP symbol '${symbol}' in $source"
        fi
    done
}

if [[ -n "$SYMBOL_SOURCE" ]]; then
    [[ -f "$SYMBOL_SOURCE" ]] || fail "missing symbol source: $SYMBOL_SOURCE"
    scan_symbol_file "$SYMBOL_SOURCE"
else
    mapfile -t boundary_sources < <(
        find "$ROOT/kernel" -type f \
            \( -name 'hfoc_lwip*.h' -o -name 'hfoc_lwip*.c' \) \
            -print 2>/dev/null || true
    )
    for candidate in "${boundary_sources[@]}"; do
        scan_symbol_file "$candidate"
    done
fi

echo "lwIP configuration verification: OK"
