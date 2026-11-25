#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
DOC_ROOT=${1:-$ROOT}
cd "$ROOT"

required_docs=(
    docs/hfoc-installation-guide.md
    docs/hfoc-dual-boot-guide.md
    docs/hfoc-first-boot-guide.md
    docs/hfoc-administrator-guide.md
    docs/hfoc-recovery-guide.md
    docs/hfoc-supported-hardware-matrix.md
    docs/hfoc-known-limitations.md
    release/hfoc-hardware-acceptance-report.md
)

for doc in "${required_docs[@]}"; do
    [[ -f "$DOC_ROOT/$doc" ]] || {
        echo "Missing HFOC release documentation: $doc" >&2
        exit 1
    }
done

check_phrases() {
    local doc=$1
    shift
    local phrase
    for phrase in "$@"; do
        grep -Fq -- "$phrase" "$DOC_ROOT/$doc" || {
            echo "HFOC release doc $doc is missing phrase: $phrase" >&2
            exit 1
        }
    done
}

check_phrases docs/hfoc-installation-guide.md \
    '## Purpose' \
    '## Safety Before Installation' \
    '## Installation Choices' \
    '## Validation And Acceptance' \
    'NOT TESTED' \
    'explicit confirmation'

check_phrases docs/hfoc-dual-boot-guide.md \
    '## Linux Dual Boot' \
    '## Windows Dual Boot' \
    '## Shared ESP' \
    '## Boot Menu' \
    'NOT TESTED' \
    'Preserve'

check_phrases docs/hfoc-first-boot-guide.md \
    '## Administrator Password' \
    '## Ready Output' \
    'NOT TESTED' \
    'must never'

check_phrases docs/hfoc-administrator-guide.md \
    '## Local Console' \
    '## HTTPS Management' \
    '## Operational Checks' \
    'NOT TESTED' \
    'authentication'

check_phrases docs/hfoc-recovery-guide.md \
    '## Bootloader Recovery' \
    '## Recovery Mode' \
    '## Factory Reset' \
    'NOT TESTED' \
    'explicit confirmation'

for hardware_needle in UEFI BIOS CPU RAM APIC Timer Keyboard Console SATA NVMe PCI NIC DHCP Static IPv4 HTTPS Management; do
    grep -Eiq "$hardware_needle" \
        "$DOC_ROOT/docs/hfoc-supported-hardware-matrix.md" || {
        echo "Hardware matrix is missing category: $hardware_needle" >&2
        exit 1
    }
done

for status_file in \
    docs/hfoc-supported-hardware-matrix.md \
    release/hfoc-hardware-acceptance-report.md; do
    if grep -Eq '^\| [A-Za-z0-9 /_-]+ \| PASS \|' \
            "$DOC_ROOT/$status_file"; then
        echo "HFOC hardware status file contains PASS without report: $status_file" >&2
        exit 1
    fi
done

grep -Fq '| Current status |' \
    "$DOC_ROOT/docs/hfoc-supported-hardware-matrix.md" || {
    echo "Hardware matrix must declare current status column" >&2
    exit 1
}
grep -Fq '| Check | Result | Observed result |' \
    "$DOC_ROOT/release/hfoc-hardware-acceptance-report.md" || {
    echo "Hardware report must declare checklist columns" >&2
    exit 1
}

echo "HFOC release documentation verification: OK"
