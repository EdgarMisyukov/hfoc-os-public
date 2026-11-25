#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

DOC=docs/hfoc-installer-architecture-audit.md
PLAN=docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md
DESIGN=docs/superpowers/specs/2026-09-06-item-338-installer-architecture-audit-design.md
PLAN_ITEM=docs/superpowers/plans/2026-09-06-item-338-installer-architecture-audit-implementation.md
README_DOC=README.md
ARCHITECTURE_DOC=docs/ARCHITECTURE.md
ROADMAP_DOC=docs/ROADMAP.md

fail() {
    echo "Item 338 installer architecture audit test: $*" >&2
    exit 1
}

for path in "$DOC" "$PLAN" "$DESIGN" "$PLAN_ITEM" \
    "$README_DOC" "$ARCHITECTURE_DOC" "$ROADMAP_DOC"; do
    [[ -f "$path" ]] ||
        fail "missing required file: $path"
done

for section in \
    '## Purpose' \
    '## Current State' \
    '## ISO Format And Boot Mode' \
    '## UEFI' \
    '## Legacy BIOS' \
    '## Installer Environment' \
    '## Hardware Probing' \
    '## Disk Discovery' \
    '## Partitioning Abstraction' \
    '## Filesystem' \
    '## Installation Target' \
    '## Bootloader' \
    '## Persistence' \
    '## Rollback And Failure Handling' \
    '## Logging' \
    '## Recovery' \
    '## Required Installer Architecture' \
    '## Safety Principles' \
    '## Dependency Ownership' \
    '## Non-Goals' \
    '## Acceptance'; do
    grep -Fq -- "$section" "$DOC" ||
        fail "authoritative audit is missing section: $section"
done

for phrase in \
    'no production installer subsystem' \
    'No production disk discovery exists' \
    'No partitioning abstraction exists' \
    'no general writable filesystem' \
    'No installation target exists' \
    'installer transaction journal' \
    'must never contain passwords, private keys, bearer tokens' \
    'bootloader recovery' \
    'Never run a destructive installation without explicit confirmation' \
    'Never format an existing OS partition in the automatic dual-boot path' \
    'Never claim physical-machine installation from QEMU/CI evidence'; do
    grep -Fq -- "$phrase" "$DOC" ||
        fail "authoritative audit does not contain required phrase: $phrase"
done

grep -Fq -- 'There is no disk' "$DOC" ||
    fail "authoritative audit does not state absence of disk bootloader install"
grep -Fq -- 'UEFI NVRAM entry creation' "$DOC" ||
    fail "authoritative audit does not state absence of UEFI NVRAM entry creation"

grep -Fq -- 'ITEM-338' "$DOC" ||
    fail "authoritative audit does not name ITEM-338"
grep -Fq -- 'ITEM-339' "$DOC" ||
    fail "authoritative audit does not assign ITEM-339 to discovery"
grep -Fq -- 'ITEM-340' "$DOC" ||
    fail "authoritative audit does not assign ITEM-340 to installation layout"
grep -Fq -- 'ITEM-341' "$DOC" ||
    fail "authoritative audit does not assign ITEM-341 to dual boot"
grep -Fq -- 'ITEM-342' "$DOC" ||
    fail "authoritative audit does not assign ITEM-342 to bootloader recovery"

grep -Fq -- '- [x] **ITEM-338**' "$PLAN" ||
    fail "installer plan does not mark ITEM-338 complete"
grep -Fq -- '- [x] **ITEM-339**' "$PLAN" ||
    fail "installer plan does not mark ITEM-339 complete"
grep -Fq -- '- [x] **ITEM-340**' "$PLAN" ||
    fail "installer plan does not mark ITEM-340 complete"
grep -Fq -- '- [x] **ITEM-341**' "$PLAN" ||
    fail "installer plan does not mark ITEM-341 complete"
grep -Fq -- '- [x] **ITEM-342**' "$PLAN" ||
    fail "installer plan does not mark ITEM-342 complete"
grep -Fq -- '- [x] **ITEM-343**' "$PLAN" ||
    fail "installer plan does not mark ITEM-343 complete"
grep -Fq -- '- [x] **ITEM-344**' "$PLAN" ||
    fail "installer plan does not mark ITEM-344 complete"
grep -Fq -- '- [x] **ITEM-345**' "$PLAN" ||
    fail "installer plan does not mark ITEM-345 complete"
grep -Fq -- '- [x] **ITEM-346**' "$PLAN" ||
    fail "installer plan does not mark ITEM-346 complete"
grep -Fq -- '- [x] **ITEM-347**' "$PLAN" ||
    fail "installer plan does not mark ITEM-347 complete"

for metadata_doc in "$README_DOC" "$ARCHITECTURE_DOC" "$ROADMAP_DOC"; do
    grep -Eiq 'installer architecture audit|installer architecture' \
        "$metadata_doc" ||
        fail "$metadata_doc does not document installer architecture audit"
done

echo "Item 338 installer architecture audit test: OK"
