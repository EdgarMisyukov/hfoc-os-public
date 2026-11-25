#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

DOC=docs/hfoc-appliance-update-package-architecture.md
DOC_FACTORY=docs/hfoc-appliance-factory-reset.md
ROADMAP_PLAN=docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md
AUDIT=docs/hfoc-appliance-platform-architecture-audit.md
ROADMAP_PUBLIC=docs/ROADMAP.md

fail() {
    echo "Item 329 update package architecture test: $*" >&2
    exit 1
}

[[ -f "$DOC" ]] ||
    fail "missing authoritative document: $DOC"
[[ -f "$DOC_FACTORY" ]] ||
    fail "missing authoritative factory reset document: $DOC_FACTORY"
[[ -f "$ROADMAP_PLAN" ]] ||
    fail "missing roadmap plan: $ROADMAP_PLAN"
[[ -f "$AUDIT" ]] ||
    fail "missing appliance platform audit: $AUDIT"
[[ -f "$ROADMAP_PUBLIC" ]] ||
    fail "missing public roadmap: $ROADMAP_PUBLIC"

for section in \
    '## Purpose' \
    '## Current State' \
    '## Scope' \
    '## Architecture Decisions' \
    '## Artifact Contract' \
    '## Metadata Contract' \
    '## Installation Stages' \
    '## Compatibility Rules' \
    '## Rollback Expectations' \
    '## Failure Boundaries' \
    '## Recovery Metadata Ownership' \
    '## Wave H Boundaries' \
    '## Non-Goals' \
    '## Acceptance'; do
    grep -Fq -- "$section" "$DOC" ||
        fail "authoritative document is missing section: $section"
done

grep -Fq -- 'ITEM-329' "$DOC" ||
    fail "authoritative document does not name ITEM-329"
grep -Fq -- 'ITEM-330' "$DOC" ||
    fail "authoritative document does not assign ITEM-330 to manifest validation"
grep -Fq -- 'recovery.metadata' "$DOC" ||
    fail "authoritative document does not state recovery.metadata ownership"

grep -Fq -- '- [x] **ITEM-329**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-329 complete"
grep -Fq -- '- [x] **ITEM-330**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-330 complete"
grep -Fq -- '- [x] **ITEM-331**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-331 complete"
grep -Fq -- '- [x] **ITEM-332**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-332 complete"
grep -Fq -- '- [x] **ITEM-333**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-333 complete"
grep -Fq -- '- [x] **ITEM-334**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-334 complete"
grep -Fq -- '- [x] **ITEM-335**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-335 complete"
grep -Fq -- '- [x] **ITEM-336**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-336 complete"
grep -Fq -- '- [x] **ITEM-337**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-337 complete"

grep -Fq -- 'ITEM-329' "$AUDIT" ||
    fail "appliance platform audit does not name ITEM-329 as completed"
grep -Fq -- 'ITEM-336' "$AUDIT" ||
    fail "appliance platform audit does not record ITEM-336 complete"
grep -Fq -- 'ITEM-337' "$AUDIT" ||
    fail "appliance platform audit does not record ITEM-337 complete"
grep -Fq -- 'ITEM-330' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe ITEM-330 as implemented"
grep -Fq -- 'ITEM-331' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe ITEM-331 as implemented"
grep -Fq -- 'ITEM-332' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe ITEM-332 as implemented"
grep -Fq -- 'ITEM-333 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-333 as implemented"
grep -Fq -- 'appliance factory reset' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe appliance factory reset"
grep -Fq -- 'ITEM-334 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-334 as implemented"
grep -Fq -- 'integrated appliance boot acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe integrated appliance boot acceptance"
grep -Fq -- 'ITEM-335 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-335 as implemented"
grep -Fq -- 'persistent configuration reboot acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe persistent configuration reboot acceptance"
grep -Fq -- 'ITEM-336 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-336 as implemented"
grep -Fq -- 'management plane QEMU acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe management plane QEMU acceptance"
grep -Fq -- 'ITEM-337 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-337 as implemented"
grep -Fq -- 'platform release acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe platform release acceptance"

echo "Item 329 update package architecture test: OK"
