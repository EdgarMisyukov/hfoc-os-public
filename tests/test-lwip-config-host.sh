#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

CONFIG="$ROOT/kernel/include/lwipopts.h"
VERIFY="$ROOT/scripts/verify-lwip-config.sh"
TEST_DIR=$(mktemp -d)

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

expect_fail() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

verify() {
    bash "$VERIFY" "$@"
}

make_fixture() {
    local target=$1
    mkdir -p "$target"
    cp "$CONFIG" "$target/lwipopts.h"
}

replace_fixture() {
    local target=$1
    local macro=$2
    local value=$3
    make_fixture "$target"
    sed -i "s/^#define ${macro}[[:space:]].*/#define ${macro} ${value}/" \
        "$target/lwipopts.h"
}

verify "$CONFIG"

replace_fixture "$TEST_DIR/enabled-socket" LWIP_SOCKET 1
expect_fail 'enabled lwIP sockets' verify \
    "$TEST_DIR/enabled-socket/lwipopts.h"

replace_fixture "$TEST_DIR/enabled-netconn" LWIP_NETCONN 1
expect_fail 'enabled lwIP netconn' verify \
    "$TEST_DIR/enabled-netconn/lwipopts.h"

replace_fixture "$TEST_DIR/disabled-tcp" LWIP_TCP 0
expect_fail 'disabled lwIP TCP' verify \
    "$TEST_DIR/disabled-tcp/lwipopts.h"

replace_fixture "$TEST_DIR/enabled-udp" LWIP_UDP 1
expect_fail 'enabled lwIP UDP' verify \
    "$TEST_DIR/enabled-udp/lwipopts.h"

replace_fixture "$TEST_DIR/enabled-dhcp" LWIP_DHCP 1
replace_fixture "$TEST_DIR/enabled-dns" LWIP_DNS 1
expect_fail 'enabled lwIP DHCP/DNS' verify \
    "$TEST_DIR/enabled-dhcp/lwipopts.h"
expect_fail 'enabled lwIP DNS' verify \
    "$TEST_DIR/enabled-dns/lwipopts.h"

replace_fixture "$TEST_DIR/enabled-ipv6" LWIP_IPV6 1
replace_fixture "$TEST_DIR/enabled-reassembly" IP_REASSEMBLY 1
replace_fixture "$TEST_DIR/enabled-frag" IP_FRAG 1
expect_fail 'enabled lwIP IPv6' verify \
    "$TEST_DIR/enabled-ipv6/lwipopts.h"
expect_fail 'enabled lwIP reassembly' verify \
    "$TEST_DIR/enabled-reassembly/lwipopts.h"
expect_fail 'enabled lwIP fragmentation' verify \
    "$TEST_DIR/enabled-frag/lwipopts.h"

replace_fixture "$TEST_DIR/enabled-per-netif-offload" \
    LWIP_CHECKSUM_CTRL_PER_NETIF 1
replace_fixture "$TEST_DIR/enabled-copy-offload" LWIP_CHECKSUM_ON_COPY 1
expect_fail 'enabled per-netif checksum offload' verify \
    "$TEST_DIR/enabled-per-netif-offload/lwipopts.h"
expect_fail 'enabled checksum-on-copy offload' verify \
    "$TEST_DIR/enabled-copy-offload/lwipopts.h"

replace_fixture "$TEST_DIR/disabled-custom-allocator" \
    MEM_CUSTOM_ALLOCATOR 0
replace_fixture "$TEST_DIR/disabled-memp-allocator" MEMP_MEM_MALLOC 0
replace_fixture "$TEST_DIR/enabled-use-pools" MEM_USE_POOLS 1
replace_fixture "$TEST_DIR/disabled-custom-pbuf" \
    LWIP_SUPPORT_CUSTOM_PBUF 0
expect_fail 'disabled lwIP custom allocator' verify \
    "$TEST_DIR/disabled-custom-allocator/lwipopts.h"
expect_fail 'disabled lwIP memp allocator' verify \
    "$TEST_DIR/disabled-memp-allocator/lwipopts.h"
expect_fail 'enabled lwIP use-pools path' verify \
    "$TEST_DIR/enabled-use-pools/lwipopts.h"
expect_fail 'disabled lwIP custom pbuf support' verify \
    "$TEST_DIR/disabled-custom-pbuf/lwipopts.h"

make_fixture "$TEST_DIR/wrong-custom-malloc"
sed -i 's/^#define MEM_CUSTOM_MALLOC .*/#define MEM_CUSTOM_MALLOC hfoc_lwip_wrong_malloc/' \
    "$TEST_DIR/wrong-custom-malloc/lwipopts.h"
expect_fail 'wrong lwIP custom malloc adapter' verify \
    "$TEST_DIR/wrong-custom-malloc/lwipopts.h"

make_fixture "$TEST_DIR/missing-version"
sed -i '/^#define HFOC_LWIP_CONFIG_VERSION /d' \
    "$TEST_DIR/missing-version/lwipopts.h"
expect_fail 'missing lwIP config version' verify \
    "$TEST_DIR/missing-version/lwipopts.h"

make_fixture "$TEST_DIR/missing-custom-data"
sed -i '/^#define LWIP_PBUF_CUSTOM_DATA /d' \
    "$TEST_DIR/missing-custom-data/lwipopts.h"
expect_fail 'missing lwIP pbuf custom data' verify \
    "$TEST_DIR/missing-custom-data/lwipopts.h"

SYMBOL_DIR="$TEST_DIR/symbols"
mkdir -p "$SYMBOL_DIR"

cat >"$SYMBOL_DIR/socket.c" <<'EOF'
int lwip_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return -1;
}
EOF
expect_fail 'forbidden lwIP socket symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/socket.c"

cat >"$SYMBOL_DIR/netconn.c" <<'EOF'
void netconn_new(void)
{
}
EOF
expect_fail 'forbidden lwIP netconn symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/netconn.c"

cat >"$SYMBOL_DIR/tcpip.c" <<'EOF'
void tcpip_init(void)
{
}
EOF
expect_fail 'forbidden lwIP tcpip symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/tcpip.c"

cat >"$SYMBOL_DIR/dhcp.c" <<'EOF'
void dhcp_start(void)
{
}
EOF
expect_fail 'forbidden lwIP DHCP symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/dhcp.c"

cat >"$SYMBOL_DIR/dns.c" <<'EOF'
void dns_gethostbyname(void)
{
}
EOF
expect_fail 'forbidden lwIP DNS symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/dns.c"

cat >"$SYMBOL_DIR/ipv6.c" <<'EOF'
void ip6_input(void)
{
}
EOF
expect_fail 'forbidden lwIP IPv6 symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/ipv6.c"

cat >"$SYMBOL_DIR/altcp.c" <<'EOF'
void altcp_new(void)
{
}
EOF
expect_fail 'forbidden lwIP altcp symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/altcp.c"

echo 'lwIP configuration host test: OK'
