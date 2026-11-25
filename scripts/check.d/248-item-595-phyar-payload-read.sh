#!/usr/bin/env bash
# ITEM-595: PHYAR completion and payload are separate reads. Sourced in pinned
# order by scripts/check.sh; run ShellCheck on its own.

driver_source=kernel/src/rtl8168.c
driver_header=kernel/include/rtl8168.h
host_test=tests/rtl8168-host.c
journal_test=tests/net-phy-journal-host.c

grep -Fq 'rtl8168_phy_wait(io, true, NULL);' "$driver_source" || {
    echo "ITEM-595: PHYAR read does not check completion before payload" >&2
    exit 1
}
grep -Fq 'rtl8168_read32(io, RTL8168_REG_PHY_ACCESS, &payload)' \
    "$driver_source" || {
    echo "ITEM-595: PHYAR payload is not read separately" >&2
    exit 1
}
wait_line=$(grep -nF 'rtl8168_phy_wait(io, true, NULL);' "$driver_source" |
    cut -d: -f1)
payload_line=$(grep -nF \
    'rtl8168_read32(io, RTL8168_REG_PHY_ACCESS, &payload)' \
    "$driver_source" | cut -d: -f1)
[[ -n "$wait_line" && -n "$payload_line" && "$wait_line" -lt "$payload_line" ]] || {
    echo "ITEM-595: PHYAR payload read is not after completion" >&2
    exit 1
}
grep -Fq 'io->phy_last.reason = RTL8168_PHY_FAILURE_MMIO_READ;' \
    "$driver_source" || {
    echo "ITEM-595: failed payload read is not named MMIO_READ" >&2
    exit 1
}
grep -Fq '*value = (uint16_t)(payload & RTL8168_PHY_ACCESS_DATA_MASK);' \
    "$driver_source" || {
    echo "ITEM-595: PHY value does not come from the separate payload" >&2
    exit 1
}
if grep -Fq 'const enum rtl8168_status status = rtl8168_phy_wait(io, true, &result);' \
    "$driver_source"; then
    echo "ITEM-595: the completion sample is still returned as PHY data" >&2
    exit 1
fi

grep -Fq 'Last successful completion-poll sample, retained on payload failure.' \
    "$driver_header" || {
    echo "ITEM-595: the completion sample's evidence meaning is not declared" >&2
    exit 1
}
grep -Fq 'Low16 of the separate payload read performed after completion.' \
    "$driver_header" || {
    echo "ITEM-595: the payload's evidence meaning is not declared" >&2
    exit 1
}

grep -Fq 'test_phyar_separate_completion_and_payload();' "$host_test" || {
    echo "ITEM-595: separate completion/payload proof is not run" >&2
    exit 1
}
grep -Fq 'test_phyar_payload_read_failure_keeps_poll_evidence();' "$host_test" || {
    echo "ITEM-595: payload-read failure proof is not run" >&2
    exit 1
}
grep -Fq 'cold start reads PHY data after, not from, the completion sample' \
    "$host_test" || {
    echo "ITEM-595: production cold start is not proven with stale completion" >&2
    exit 1
}
grep -Fq 'payload MMIO failure keeps completion evidence and page0 recovery' \
    "$host_test" || {
    echo "ITEM-595: payload failure evidence or page recovery is not proven" >&2
    exit 1
}
grep -Fq 'cold start separates each PHY completion sample from its payload' \
    "$journal_test" || {
    echo "ITEM-595: journal model does not separate completion and payload" >&2
    exit 1
}
grep -Fq 'reason=MMIO_READ_FAILED cmd=0x001F0000 last=0x801F0000 polls=2' \
    "$journal_test" || {
    echo "ITEM-595: journal failure does not retain completion evidence" >&2
    exit 1
}
grep -Fq 'ITEM-595' docs/ARCHITECTURE.md || {
    echo "ITEM-595: ARCHITECTURE does not record the protocol correction" >&2
    exit 1
}
grep -Fq 'ITEM-595' docs/ROADMAP.md || {
    echo "ITEM-595: ROADMAP does not record the protocol correction" >&2
    exit 1
}
grep -Fq 'ITEM-595' docs/rtl8168evl-phy-audit.md || {
    echo "ITEM-595: PHY audit does not record the protocol correction" >&2
    exit 1
}
