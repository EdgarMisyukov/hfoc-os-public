#!/usr/bin/env bash
# ITEM-589: one table answers "which chip" and "how is its PHY reached".
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

# The owner's Toshiba L875 reports XID 0x2C9.  The family table's mask row
# `{ 0x7C8, 0x2C8 }` matches it, so the production identify accepts the part -
# but the PHY list was a second, hand-written exact-XID list, and 0x2C9 was not
# in it.  The driver skipped PHY configuration, stayed on the PHYstatus byte
# and published link=DOWN with a live cable.  The claims pinned here:
#
#   * the family table row carries the PHY access, so a mask the table accepts
#     cannot be skipped by another list;
#   * the E/F-era rows are the PHYAR (clause-22) ones, the later G/H/EP rows
#     are OCP, and the classic parts keep no PHY claim at all;
#   * the regression drives the raw TxConfig the hardware read (0x2C900000)
#     through the production identify and through the production PHY
#     configuration, and keeps the configuration result and the first error;
#   * `[NET-PHY]` is printed on skip and refusal as well, with the XID, the
#     access method and the reason.
state_header=kernel/include/rtl8168.h
state_source=kernel/src/rtl8168.c
boot_header=kernel/include/appliance_production_network_boot.h

grep -Fq 'enum rtl8168_phy_access {' "$state_header" || {
    echo "ITEM-589: the PHY access vocabulary is missing" >&2
    exit 1
}
grep -Fq 'RTL8168_PHY_ACCESS_MII_PHYAR' "$state_header" || {
    echo "ITEM-589: the PHYAR access is not named" >&2
    exit 1
}
grep -Fq 'RTL8168_PHY_ACCESS_OCP' "$state_header" || {
    echo "ITEM-589: the OCP access is not named" >&2
    exit 1
}
grep -Fq 'rtl8168_phy_access_method(' "$state_header" || {
    echo "ITEM-589: no access query is declared" >&2
    exit 1
}
grep -Fq 'enum rtl8168_phy_access access;' "$state_source" || {
    echo "ITEM-589: the family table carries no PHY access column" >&2
    exit 1
}
# ITEM-592 added the profile column to the row, so the pin follows the row's
# first line: the mask still carries the PHYAR access.
grep -Fq '0x2C8U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_MII_PHYAR,' \
    "$state_source" || {
    echo "ITEM-589: the 0x2C8 mask row does not carry the PHYAR access" >&2
    exit 1
}
grep -Fq '0x541U, RTL8168_CHIP_EXTENDED, RTL8168_PHY_ACCESS_OCP,' \
    "$state_source" || {
    echo "ITEM-589: the late OCP part lost its own access" >&2
    exit 1
}
if grep -Fq 'chip->xid == 0x2C8U || chip->xid == 0x2C1U' "$state_source"; then
    echo "ITEM-589: the hand-written exact-XID PHY list is back" >&2
    exit 1
fi
grep -Fq 'return rtl8168_xid_table[index].access;' "$state_source" || {
    echo "ITEM-589: the access query is not table-driven" >&2
    exit 1
}
grep -Fq 'rtl8168_phy_access_method(&apn.rtl.chip)' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-589: the adapter does not record the access method" >&2
    exit 1
}
grep -Fq 'state->configure_attempted = true;' "$state_source" || {
    echo "ITEM-589: the configuration attempt is not recorded" >&2
    exit 1
}
grep -Fq 'state->first_error' "$state_source" || {
    echo "ITEM-589: the first PHY error is not kept" >&2
    exit 1
}
grep -Fq 'UINT32_C(0x2C900000)' tests/rtl8168-host.c || {
    echo "ITEM-589: the regression does not drive the raw 0x2C9 TxConfig" >&2
    exit 1
}
grep -Fq 'the 0x2C9 PHY is reached through PHYAR, not skipped' \
    tests/rtl8168-host.c || {
    echo "ITEM-589: the regression does not pin the 0x2C9 access" >&2
    exit 1
}
grep -Fq 'a refused PHY configuration keeps its first error' \
    tests/rtl8168-host.c || {
    echo "ITEM-589: the regression does not pin the first error" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_SKIPPED' "$boot_header" || {
    echo "ITEM-589: the skipped state is not part of the PHY evidence" >&2
    exit 1
}
# ITEM-590 moved the answer's derivation and the record's composition into
# kernel/src/appliance_production_network_phy_record.c; the claims below follow
# the code they pin, so a record that loses the reason or the state still fails.
grep -Fq 'NO_PHY_MANAGEMENT' "$state_source" \
    kernel/src/appliance_production_network_x86.c \
    kernel/src/appliance_production_network_phy_record.c || {
    echo "ITEM-589: the skip reason is not published" >&2
    exit 1
}
grep -Fq '[NET-PHY] state=' kernel/src/kernel.c \
    kernel/src/appliance_production_network_phy_record.c || {
    echo "ITEM-589: the record does not name the PHY state" >&2
    exit 1
}
# shellcheck disable=SC2016  # the proof's spelling, not this script's value
grep -Fq 'QEMU_MEMORY=${QEMU_MEMORY:-2176M}' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-589: the update proof does not keep its small guest footprint" >&2
    exit 1
}
grep -Fq 'ITEM-589' docs/ROADMAP.md || {
    echo "ITEM-589: ROADMAP does not record the access fix" >&2
    exit 1
}
