#!/usr/bin/env bash
# ITEM-592: the 8168evl cold start, aligned with the reference driver for the
#           owner's XID and bounded by the confirmed profile.  Sourced in
#           pinned order by scripts/check.sh; run ShellCheck on its own.

# The Toshiba run of 40b35ec3 (report CRC 3022F9A1) showed a PHY that answered
# every read and never saw a link partner: `bmcr=0x1140 bmsr=0x7949 lpa=0x0000
# link=DOWN aneg=0`, with `adv=0x01E1 adv_written=0x03E1`.  Comparing the
# production path with Linux r8169 v6.12 for RTL_GIGA_MAC_VER_34 (the row
# `{ 0x7c8, 0x2c8 }`, which the 0x2C9 XID matches) found:
#
#   * the 8168evl start runs EPHY and clock stages before the MAC is enabled
#     (`rtl_hw_start_8168e_2()`), which HFOC never did;
#   * the receive data-valid gate (MISC bit 19) is a G/H/EP setting, and
#     HFOC wrote it for the whole extended family, the 8168evl included;
#   * RX_EARLY_OFF and TXCFG_AUTO_FIFO are also generation-bounded, and HFOC
#     applied them family-wide;
#   * register 4 can only advertise 10/100 (its 0x0200 bit is 100BASE-T4);
#     1000BASE-T lives in register 9, which HFOC never wrote;
#   * the PHY's page register decides what "register 4" means, and nothing
#     selected page 0.
#
# The claims pinned here:
#
#   * the XID table's row carries the profile, and 0x2C8/0x2C9 is the 8168evl;
#   * every generation bound is a question about the profile, and the G/H
#     behaviour is unchanged;
#   * the cold start is one sequenced run with named stages, a first failure
#     and bounded waits;
#   * the EPHY window uses its own reference timing, and extended-register
#     addresses above 0xff are addressable;
#   * the PHY stage selects page 0, writes register 4 and register 9, reads
#     both back, and keeps the first error without rewriting the verdict;
#   * the record carries the profile, the stages, the PHYstatus and BMCR the
#     part came up with, the BMSR samples, the gigabit advertisement and the
#     partner's status, the page, and each read's verdict;
#   * the firmware dependency the driver cannot satisfy is named, not faked.
driver_header=kernel/include/rtl8168.h
driver_source=kernel/src/rtl8168.c
adapter_source=kernel/src/appliance_production_network_x86.c
boot_header=kernel/include/appliance_production_network_boot.h
record_source=kernel/src/appliance_production_network_phy_record.c

grep -Fq 'enum rtl8168_profile {' "$driver_header" || {
    echo "ITEM-592: the chip profile is not a named generation" >&2
    exit 1
}
grep -Fq 'RTL8168_PROFILE_8168EVL = 3' "$driver_header" || {
    echo "ITEM-592: the 8168evl generation is missing" >&2
    exit 1
}
grep -Fq 'enum rtl8168_profile profile;' "$driver_header" || {
    echo "ITEM-592: the chip does not carry its profile" >&2
    exit 1
}
grep -Fq 'RTL8168_PROFILE_8168EVL }' "$driver_source" || {
    echo "ITEM-592: no table row resolves to the 8168evl" >&2
    exit 1
}
grep -Fq 'chip->profile = rtl8168_xid_profile(chip->xid);' "$driver_source" || {
    echo "ITEM-592: the identify does not resolve the profile" >&2
    exit 1
}

# The generation bounds.
grep -Fq 'return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168G);' \
    "$driver_source" || {
    echo "ITEM-592: the receive gate is not bounded to the G generation" >&2
    exit 1
}
grep -Fq 'if (rtl8168_profile_rxdv_gate(chip)) {' "$driver_source" || {
    echo "ITEM-592: the bring-up does not ask the profile about the gate" >&2
    exit 1
}
if grep -A 1 'if (chip->family == RTL8168_CHIP_EXTENDED) {' "$driver_source" \
        | grep -Fq 'uint32_t misc;'; then
    echo "ITEM-592: the receive gate is family-wide again" >&2
    exit 1
fi
grep -Fq 'if (rtl8168_profile_rx_early_off(chip)) {' "$driver_source" || {
    echo "ITEM-592: RX_EARLY_OFF is not bounded to its generation" >&2
    exit 1
}
grep -Fq 'if (rtl8168_profile_tx_auto_fifo(chip)) {' "$driver_source" || {
    echo "ITEM-592: TXCFG_AUTO_FIFO is not bounded to its generation" >&2
    exit 1
}
grep -Fq 'return rtl8168_profile_at_least(chip, RTL8168_PROFILE_8168EVL);' \
    "$driver_source" || {
    echo "ITEM-592: the auto FIFO minimum generation is wrong" >&2
    exit 1
}

# The EPHY window and its reference timing.
grep -Fq '#define RTL8168_REG_EPHY_ACCESS 0x80U' "$driver_header" || {
    echo "ITEM-592: the EPHY window is not named" >&2
    exit 1
}
grep -Fq '#define RTL8168_EPHY_ACCESS_POLLS 100U' "$driver_header" || {
    echo "ITEM-592: the EPHY wait is not bounded by the reference count" >&2
    exit 1
}
grep -Fq 'enum rtl8168_status rtl8168_ephy_write(' "$driver_header" || {
    echo "ITEM-592: no EPHY access function is declared" >&2
    exit 1
}
grep -Fq '{ 0x09U, 0x0000U, 0x0080U },' "$driver_source" || {
    echo "ITEM-592: the EPHY table row for register 9 is missing" >&2
    exit 1
}
grep -Fq '{ 0x0CU, 0x3DF0U, 0x0200U },' "$driver_source" || {
    echo "ITEM-592: the EPHY table row for register 0x0c is missing" >&2
    exit 1
}
# The extended-register address is 11 bits: 0x1D0 is not 0xD0.
grep -Fq '#define RTL8168_ERIAR_ADDRESS_MASK UINT32_C(0x000007FF)' \
    "$driver_header" || {
    echo "ITEM-592: the extended-register address mask is missing" >&2
    exit 1
}
grep -Fq 'rtl8168_eri_write(io, 0x1D0U' "$driver_source" ||
    grep -Fq 'rtl8168_eri_modify(io, 0x1D0U' "$driver_source" || {
    echo "ITEM-592: the clock stage does not address 0x1d0" >&2
    exit 1
}
grep -Fq 'rtl8168_eri_write(io, 0xD0U, 0x07FF0060U)' "$driver_source" || {
    echo "ITEM-592: the clock stage's fixed word is missing" >&2
    exit 1
}
grep -Fq '(0xFFU ^ RTL8168_MCU_NOW_IS_OOB)' "$driver_source" || {
    echo "ITEM-592: the out-of-band bit is not cleared" >&2
    exit 1
}
grep -Fq 'misc | RTL8168_MISC_PWM_EN' "$driver_source" || {
    echo "ITEM-592: the PWM block is not turned on" >&2
    exit 1
}
grep -Fq 'value | RTL8168_DLLPR_PFM_EN' "$driver_source" || {
    echo "ITEM-592: the power-mode clock is not enabled" >&2
    exit 1
}
# The gate bit must not be part of the evl clock stage.
if grep -Fq 'RTL8168_MISC_RXDV_GATED_EN' kernel/src/rtl8168.c &&
   grep -Fq 'rtl8168_evl_clock_stage' kernel/src/rtl8168.c; then
    clock_body=$(sed -n '/static enum rtl8168_status rtl8168_evl_clock_stage(/,/^}/p' \
        kernel/src/rtl8168.c)
    if [[ $clock_body == *RXDV_GATED* ]]; then
        echo "ITEM-592: the evl clock stage writes the G-generation gate bit" >&2
        exit 1
    fi
fi

# The cold-boot composition and its report.
grep -Fq 'enum rtl8168_status rtl8168_cold_boot(' "$driver_header" || {
    echo "ITEM-592: no cold-boot entry point is declared" >&2
    exit 1
}
for stage in 'RTL8168_STAGE_EPHY' 'RTL8168_STAGE_CLOCK' 'RTL8168_STAGE_MAC' \
    'RTL8168_STAGE_PHY' 'RTL8168_STAGE_ANEG'; do
    grep -Fq "$stage" "$driver_header" || {
        echo "ITEM-592: the ${stage} stage is not named" >&2
        exit 1
    }
done
grep -Fq 'report->first_failure = status;' "$driver_source" || {
    echo "ITEM-592: the cold boot does not keep the first failure" >&2
    exit 1
}
grep -Fq 'report->firmware_skipped =' "$driver_source" || {
    echo "ITEM-592: the firmware dependency is not named" >&2
    exit 1
}
grep -Fq 'rtl8168_cold_boot(' "$adapter_source" || {
    echo "ITEM-592: the adapter does not run the cold boot" >&2
    exit 1
}
# The bring-up itself goes through the sequenced run; the runtime path may
# still restart negotiation on a PHY that reset itself (as phylib does).
if grep -Fq 'rtl8168_bring_up(&apn.rtl.io, &apn.rtl.chip, &configuration' \
    "$adapter_source"; then
    echo "ITEM-592: the adapter still starts the MAC outside the sequence" >&2
    exit 1
fi
grep -Fq 'apn.rtl.boot' "$adapter_source" || {
    echo "ITEM-592: the adapter does not keep the cold-boot report" >&2
    exit 1
}

# The advertisement: page 0, registers 4 and 9, read-back with verdicts.
grep -Fq 'rtl8168_phy_select_page(io, RTL8168_PHY_PAGE_STANDARD)' \
    "$driver_source" || {
    echo "ITEM-592: the PHY page is not selected before the reads" >&2
    exit 1
}
grep -Fq '#define RTL8168_PHY_ADVERTISE_DEFAULT 0x01E1U' "$driver_header" || {
    echo "ITEM-592: register 4 still claims 100BASE-T4" >&2
    exit 1
}
grep -Fq '#define RTL8168_PHY_REG_CTRL1000 0x09U' "$driver_header" || {
    echo "ITEM-592: the 1000BASE-T advertisement register is not named" >&2
    exit 1
}
grep -Fq 'rtl8168_phy_write(io, RTL8168_PHY_REG_CTRL1000,' "$driver_source" || {
    echo "ITEM-592: 1000BASE-T is never advertised" >&2
    exit 1
}
grep -Fq 'RTL8168_PHY_REG_STAT1000' "$driver_source" || {
    echo "ITEM-592: the partner's 1000BASE-T status is never read" >&2
    exit 1
}
grep -Fq 'state->advertise_1000_valid = true;' "$driver_source" || {
    echo "ITEM-592: the gigabit advertisement read is not verified" >&2
    exit 1
}
grep -Fq 'uint8_t phy_status_initial;' "$boot_header" || {
    echo "ITEM-592: the diagnostics do not keep the initial PHYstatus" >&2
    exit 1
}
grep -Fq 'uint16_t bmsr_sequence[RTL8168_PHY_BMSR_SAMPLES];' "$boot_header" || {
    echo "ITEM-592: the diagnostics do not keep the BMSR samples" >&2
    exit 1
}
grep -Fq 'bool lpa_1000_valid;' "$boot_header" || {
    echo "ITEM-592: the diagnostics do not keep the partner's gigabit status" >&2
    exit 1
}
for record in '[NET-PHY-BOOT]' '[NET-PHY-GIG]' '[NET-PHY-SAMPLES]'; do
    grep -Fq "$record" "$record_source" || {
        echo "ITEM-592: the ${record} record is not composed" >&2
        exit 1
    }
done
grep -Fq 'appliance_production_network_phy_detail_publish(&info, serial_write);' \
    kernel/src/kernel.c || {
    echo "ITEM-592: the boot journal does not publish the evidence records" >&2
    exit 1
}
grep -Fq 'appliance_production_network_phy_boot_line(&info, phy,' \
    kernel/src/kernel.c || {
    echo "ITEM-592: the diagnostic report does not carry the stage evidence" >&2
    exit 1
}

# The proofs.
for case in 'test_cold_boot_evl_sequence' 'test_cold_boot_evl_rejects_a_missing_stage' \
    'test_cold_boot_evl_refusals' 'test_cold_boot_evl_recovers_the_phy_page' \
    'test_cold_boot_evl_keeps_the_first_error' 'test_cold_boot_evl_samples_the_bmsr' \
    'test_generation_bounds'; do
    grep -Fq "$case" tests/rtl8168-host.c || {
        echo "ITEM-592: the ${case} proof is missing" >&2
        exit 1
    }
done
grep -Fq 'the model refuses the MAC enable that skipped the ephy/clock stages' \
    tests/rtl8168-host.c || {
    echo "ITEM-592: the wrong-order proof is missing" >&2
    exit 1
}
grep -Fq 'no generation-foreign register and no page-mismatched PHY access' \
    tests/rtl8168-host.c || {
    echo "ITEM-592: the model does not refuse a foreign register" >&2
    exit 1
}
grep -Fq 'no standard register was touched before page 0 was selected' \
    tests/rtl8168-host.c || {
    echo "ITEM-592: the PHY page recovery is not proven" >&2
    exit 1
}
grep -Fq '[NET-PHY-BOOT] profile=RTL8168evl/8111evl ' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-592: the journal proof does not require the stage record" >&2
    exit 1
}
grep -Fq '[NET-PHY-GIG] adv1000=0x0300 ' tests/net-phy-journal-host.c || {
    echo "ITEM-592: the journal proof does not require the gigabit record" >&2
    exit 1
}
grep -Fq 'the journal carries the BMSR sample line' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-592: the journal proof does not require the sample record" >&2
    exit 1
}
grep -Fq 'ITEM-592' docs/ROADMAP.md || {
    echo "ITEM-592: ROADMAP does not record the cold start" >&2
    exit 1
}
grep -Fq 'ITEM-592' docs/ARCHITECTURE.md || {
    echo "ITEM-592: ARCHITECTURE does not record the sequence" >&2
    exit 1
}


# ITEM-593: the remaining PHY preparation is part of this same cold start.
# The behavioral assertions run in the existing canonical host targets.
for stage in RTL8168_STAGE_PHY_RESUME RTL8168_STAGE_PHY_TUNE RTL8168_STAGE_PHY_RESET; do
    grep -Fq "$stage" "$driver_source" || {
        echo "ITEM-593: missing cold-start stage $stage" >&2
        exit 1
    }
done
for proof in test_evl_phy_reference_sequence test_evl_phy_intermediate_failures \
    test_evl_phy_reset_timeout_and_page_failure test_evl_phy_other_profiles_unchanged \
    test_evl_aneg_command_failure test_evl_phy_control_refusals; do
    grep -Fq "$proof();" tests/rtl8168-host.c || {
        echo "ITEM-593: production PHY proof is not run: $proof" >&2
        exit 1
    }
done
grep -Fq 'test_negotiation_evidence_is_observation();' tests/net-phy-journal-host.c || {
    echo 'ITEM-593: negotiation observation/export proof is not run' >&2
    exit 1
}
grep -Fq 'diag_export_save(&session)' tests/net-phy-journal-host.c || {
    echo 'ITEM-593: PHY proof does not exercise real diag save' >&2
    exit 1
}

grep -Fq 'test_refused_probe_is_visible_without_backend();' tests/net-phy-journal-host.c || {
    echo 'ITEM-593: failed production adapter probe is not covered' >&2
    exit 1
}

grep -Fq 'test_cold_adapter_delay_is_measured();' tests/net-phy-journal-host.c || {
    echo 'ITEM-593: real cold-adapter delay proof is not run' >&2
    exit 1
}

grep -Fq 'test_phy_reference_wait_reports_completion();' tests/net-phy-journal-host.c || {
    echo 'ITEM-593: checked production reference wait is not covered' >&2
    exit 1
}

grep -Fq 'test_phy_sample_survives_later_read_failure();' tests/rtl8168-host.c || {
    echo 'ITEM-593: partial-read BMSR evidence retention is not covered' >&2
    exit 1
}

# ITEM-594: first transaction evidence must reach both published paths.
grep -Fq 'test_first_phy_transaction_is_published();' tests/net-phy-journal-host.c || {
    echo 'ITEM-594: first transaction behavior proof is not run' >&2
    exit 1
}
for failure_source in kernel/src/appliance_production_network_phy_record.c kernel/src/kernel.c; do
    grep -Fq 'appliance_production_network_phy_failure_line' "$failure_source" || {
        echo "ITEM-594: failure record is not wired through $failure_source" >&2
        exit 1
    }
done
