#!/usr/bin/env bash
# ITEM-591: the PHYAR handshake keeps the gap the part requires, and the
#           advertisement is read back instead of assumed.  Sourced in pinned
#           order by scripts/check.sh; run ShellCheck on its own.

# The Toshiba run of 49ac1ffc configured a PHY that answered
# (`phyid=0x001C:0xC915`, `state=CONFIGURED`, `first_error=NONE`) while the
# link stayed down: `bmcr=0x1140 bmsr=0x7949 lpa=0x0000 link=DOWN aneg=0`, DHCP
# `WAITING_FOR_LINK`.  Two defects are visible in the code:
#
#   * `rtl8168_phy_read()`/`rtl8168_phy_write()` issued the next PHYAR command
#     as soon as the busy bit said the previous one was done.  The reference
#     driver waits 20 us after every completion indication first, because the
#     hardware spec requires it (Linux v6.12 r8169_main.c, r8169_mdio_read/write
#     and rtl_loop_wait);
#   * the record printed `adv=0x03E1` from the constant the driver wrote, so a
#     PHY that masked the gigabit pairs could not be told from one that took
#     them.
#
# The claims pinned here:
#
#   * the wait polls at the reference driver's 25 us interval, at most 20
#     times, so a part that never answers is given up inside a measured 500 us
#     budget instead of a tight loop of arbitrary length;
#   * every completed operation - and a failed wait too, as in the reference
#     driver - leaves a 20 us settle before the next command, through a
#     microsecond pause the platform supplies;
#   * the production MMIO adapter supplies that pause from the machine's
#     calibrated cycle counter, and the PHY path refuses a bus that cannot
#     wait rather than issuing back-to-back commands;
#   * MII register 4 is read back: the record separates the written value, the
#     value the part answered and whether the read answered, while CONFIGURED,
#     the link state and the auto-negotiation completion stay separate
#     results, and a failed read keeps the first error;
#   * the host model owns a microsecond clock and rejects a command issued
#     inside the settle window, so the four command orders are checked as the
#     part would see them.
driver_header=kernel/include/rtl8168.h
driver_source=kernel/src/rtl8168.c
adapter_source=kernel/src/appliance_production_network_x86.c
boot_header=kernel/include/appliance_production_network_boot.h
record_source=kernel/src/appliance_production_network_phy_record.c

grep -Fq '#define RTL8168_PHY_ACCESS_POLL_US 25U' "$driver_header" || {
    echo "ITEM-591: the poll interval is not the reference driver's 25 us" >&2
    exit 1
}
grep -Fq '#define RTL8168_PHY_ACCESS_POLLS 20U' "$driver_header" || {
    echo "ITEM-591: the poll budget is not the reference driver's 20 polls" >&2
    exit 1
}
grep -Fq '(RTL8168_PHY_ACCESS_POLL_US * RTL8168_PHY_ACCESS_POLLS)' \
    "$driver_header" || {
    echo "ITEM-591: the 500 us bound is not derived from the poll pair" >&2
    exit 1
}
grep -Fq '#define RTL8168_PHY_ACCESS_SETTLE_US 20U' "$driver_header" || {
    echo "ITEM-591: the settle gap is not 20 us" >&2
    exit 1
}
grep -Fq 'r8169_main.c' "$driver_header" || {
    echo "ITEM-591: the timing constants do not name their source" >&2
    exit 1
}
grep -Fq 'void (*delay_us)(void *context, uint32_t microseconds);' \
    "$driver_header" || {
    echo "ITEM-591: the bus has no microsecond pause" >&2
    exit 1
}
# ITEM-592 factored the loop into one parameterised handshake; the interval is
# still the reference driver's, passed by the PHYAR wrapper.
grep -Fq 'io->delay_us(io->context, poll_us);' "$driver_source" || {
    echo "ITEM-591: the busy wait does not sleep between polls" >&2
    exit 1
}
grep -Fq 'RTL8168_PHY_ACCESS_POLL_US,' "$driver_source" || {
    echo "ITEM-591: the PHYAR wait does not use the 25 us interval" >&2
    exit 1
}
grep -Fq 'for (attempt = 0U; attempt < polls; ++attempt) {' "$driver_source" || {
    echo "ITEM-591: the busy wait is not bounded by its poll budget" >&2
    exit 1
}
# ITEM-594: the same bounded wait also captures the transaction evidence.
grep -Fq 'RTL8168_PHY_ACCESS_POLLS, last, &io->phy_last);' "$driver_source" || {
    echo "ITEM-591: the PHYAR wait does not pass the 20-poll budget" >&2
    exit 1
}
read_body=$(sed -n '/enum rtl8168_status rtl8168_phy_read(/,/^}/p' \
    "$driver_source")
write_body=$(sed -n '/enum rtl8168_status rtl8168_phy_write(/,/^}/p' \
    "$driver_source")
read_settles=$(grep -Fc 'rtl8168_phy_settle(io);' <<<"$read_body")
write_settles=$(grep -Fc 'rtl8168_phy_settle(io);' <<<"$write_body")
[[ "$read_settles" == 3 && "$write_settles" == 1 ]] || {
    echo "ITEM-591: settle counts are read=${read_settles} write=${write_settles}, not 3/1" >&2
    exit 1
}
grep -Fq 'if (!rtl8168_phy_can_wait(io)) {' "$driver_source" || {
    echo "ITEM-591: a bus without a pause is not refused" >&2
    exit 1
}
grep -Fq 'io->delay_us(io->context, RTL8168_PHY_ACCESS_SETTLE_US);' \
    "$driver_source" || {
    echo "ITEM-591: the settle does not reach the bus" >&2
    exit 1
}
grep -Fq 'static void apn_rtl_delay_us(void *context, uint32_t microseconds)' \
    "$adapter_source" || {
    echo "ITEM-591: the production adapter supplies no microsecond pause" >&2
    exit 1
}
grep -Fq 'apn.rtl.io.delay_us = apn_rtl_delay_us;' "$adapter_source" || {
    echo "ITEM-591: the adapter's pause is not wired to the driver" >&2
    exit 1
}
# ITEM-593: cold init precedes the shared clock. Require a checked local reference.
grep -Fq 'timer_delay_ms_checked(APN_RTL_DELAY_CALIBRATION_MS)' "$adapter_source" || {
    echo "ITEM-591: the pause does not use the machine's calibrated clock" >&2
    exit 1
}

# The advertisement is read back, and the three answers stay separate.
advertise_reads=$(grep -Fc 'RTL8168_PHY_REG_ADVERTISE, &advertise' \
    "$driver_source")
[[ "$advertise_reads" == 2 ]] || {
    echo "ITEM-591: register 4 is read back ${advertise_reads} times, not by the configure and the refresh" >&2
    exit 1
}
grep -Fq 'state->advertise_written = RTL8168_PHY_ADVERTISE_DEFAULT;' \
    "$driver_source" || {
    echo "ITEM-591: the written advertisement is not recorded" >&2
    exit 1
}
grep -Fq 'state->advertise_valid = true;' "$driver_source" || {
    echo "ITEM-591: the read-back's validity is not recorded" >&2
    exit 1
}
grep -Fq 'rtl8168_phy_note_first_error(state);' "$driver_source" || {
    echo "ITEM-591: a failed read-back does not keep the first error" >&2
    exit 1
}
if grep -Fq 'state->advertise = RTL8168_PHY_ADVERTISE_DEFAULT;' \
    "$driver_source"; then
    echo "ITEM-591: the advertisement is the constant again" >&2
    exit 1
fi
grep -Fq 'uint16_t advertise_written;' "$boot_header" || {
    echo "ITEM-591: the diagnostics do not carry the written advertisement" >&2
    exit 1
}
grep -Fq 'bool advertise_valid;' "$boot_header" || {
    echo "ITEM-591: the diagnostics do not carry the read validity" >&2
    exit 1
}
grep -Fq 'info->advertise_written = phy->advertise_written;' "$record_source" || {
    echo "ITEM-591: the record does not copy the written advertisement" >&2
    exit 1
}
grep -Fq '" adv_written="' "$record_source" || {
    echo "ITEM-591: the record does not print the written advertisement" >&2
    exit 1
}
grep -Fq '" adv_read="' "$record_source" || {
    echo "ITEM-591: the record does not print the read's verdict" >&2
    exit 1
}

# The proofs.
grep -Fq 'test_phyar_settle_gap' tests/rtl8168-host.c || {
    echo "ITEM-591: no host proof of the inter-command gap" >&2
    exit 1
}
grep -Fq 'test_phyar_bounded_wait' tests/rtl8168-host.c || {
    echo "ITEM-591: no host proof of the bounded wait" >&2
    exit 1
}
grep -Fq 'test_phy_advertise_readback' tests/rtl8168-host.c || {
    echo "ITEM-591: no host proof of the advertisement read-back" >&2
    exit 1
}
for order in 'read -> read' 'read -> write' 'write -> read' 'write -> write'; do
    grep -Fq "$order" tests/rtl8168-host.c || {
        echo "ITEM-591: the ${order} order is not proven" >&2
        exit 1
    }
done
grep -Fq 'no PHYAR command is issued inside the 20 us settle window' \
    tests/rtl8168-host.c || {
    echo "ITEM-591: the model does not reject an early command" >&2
    exit 1
}
grep -Fq 'a part that completes late is waited for, not refused' \
    tests/rtl8168-host.c || {
    echo "ITEM-591: the delayed completion is not proven" >&2
    exit 1
}
grep -Fq 'a PHY that never completes a read is refused, not spun on' \
    tests/rtl8168-host.c || {
    echo "ITEM-591: the unanswering PHY is not proven bounded" >&2
    exit 1
}
grep -Fq 'a bus that cannot wait refuses the PHY before issuing a command' \
    tests/rtl8168-host.c || {
    echo "ITEM-591: the clockless bus is not proven refused" >&2
    exit 1
}
# ITEM-592 corrected register 4's constant to the 10/100 set; the journal pin
# follows the value the driver now writes.
grep -Fq 'adv_written=0x01E1 adv_read=OK' tests/net-phy-journal-host.c || {
    echo "ITEM-591: the journal proof does not require the read advertisement" >&2
    exit 1
}
grep -Fq 'the recorded advertisement is not the constant' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-591: the journal proof does not separate the constant" >&2
    exit 1
}
grep -Fq 'the journal does not print the constant as if it were the ' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-591: the journal proof accepts the constant as the value" >&2
    exit 1
}
grep -Fq 'ITEM-591' docs/ROADMAP.md || {
    echo "ITEM-591: ROADMAP does not record the timing and read-back" >&2
    exit 1
}
grep -Fq 'ITEM-591' docs/ARCHITECTURE.md || {
    echo "ITEM-591: ARCHITECTURE does not record the handshake" >&2
    exit 1
}
