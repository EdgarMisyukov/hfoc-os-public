#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

AUDIT=docs/hfoc-appliance-platform-architecture-audit.md
ROADMAP=docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md

fail() {
    echo "Item 301 audit test: $*" >&2
    exit 1
}

[[ -f "$AUDIT" ]] ||
    fail "missing $AUDIT"
[[ -f "$ROADMAP" ]] ||
    fail "missing $ROADMAP"

for section in \
    '## Audit Scope' \
    '## Boot Sequence' \
    '## Kernel, Memory, Interrupts And Execution' \
    '## Storage And State' \
    '## Console, Serial And Local Control' \
    '## Network, VirtIO And TLS/HTTPS' \
    '## Test, Acceptance And CI' \
    '## Existing Reusable Primitives' \
    '## Missing Appliance Capabilities' \
    '## Corrected Execution Order' \
    '## Risk Register'; do
    grep -Fq -- "$section" "$AUDIT" ||
        fail "audit document is missing section: $section"
done

grep -Fq -- 'no production persistent state loader' "$AUDIT" ||
    fail "audit document does not state the persistent-state loader gap"
grep -Fq -- 'no production network lifecycle' "$AUDIT" ||
    fail "audit document does not state the network lifecycle gap"
grep -Fq -- 'no production HTTPS management service' "$AUDIT" ||
    fail "audit document does not state the HTTPS management service gap"

grep -Fq -- '## Execution Status' "$ROADMAP" ||
    fail "roadmap plan is missing the execution-status table"
grep -Fq -- '- [x] **ITEM-301**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-301 complete"
grep -Fq -- '- [x] **ITEM-302**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-302 complete"
grep -Fq -- '- [x] **ITEM-303**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-303 complete"
grep -Fq -- '- [x] **ITEM-304**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-304 complete"
grep -Fq -- '- [x] **ITEM-305**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-305 complete"
grep -Fq -- '- [x] **ITEM-306**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-306 complete"
grep -Fq -- '- [x] **ITEM-309**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-309 complete"
grep -Fq -- '- [x] **ITEM-310**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-310 complete"
grep -Fq -- '- [x] **ITEM-307**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-307 complete"
grep -Fq -- '- [x] **ITEM-308**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-308 complete"
grep -Fq -- '- [x] **ITEM-311**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-311 complete"
grep -Fq -- '- [x] **ITEM-312**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-312 complete"
grep -Fq -- '- [x] **ITEM-313**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-313 complete"
grep -Fq -- '- [x] **ITEM-314**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-314 complete"
grep -Fq -- '- [x] **ITEM-315**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-315 complete"
grep -Fq -- '- [x] **ITEM-316**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-316 complete"
grep -Fq -- '- [x] **ITEM-317**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-317 complete"
grep -Fq -- '- [x] **ITEM-318**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-318 complete"
grep -Fq -- '- [x] **ITEM-319**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-319 complete"
grep -Fq -- '- [x] **ITEM-320**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-320 complete"
grep -Fq -- '- [x] **ITEM-321**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-321 complete"
grep -Fq -- '- [x] **ITEM-322**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-322 complete"
grep -Fq -- '- [x] **ITEM-323**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-323 complete"
grep -Fq -- '- [x] **ITEM-324**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-324 complete"
grep -Fq -- '- [x] **ITEM-325**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-325 complete"
grep -Fq -- '- [x] **ITEM-326**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-326 complete"
grep -Fq -- '- [x] **ITEM-327**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-327 complete"
grep -Fq -- '- [x] **ITEM-328**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-328 complete"
grep -Fq -- '- [x] **ITEM-329**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-329 complete"
grep -Fq -- '- [x] **ITEM-330**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-330 complete"
grep -Fq -- '- [x] **ITEM-331**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-331 complete"
grep -Fq -- '- [x] **ITEM-332**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-332 complete"
grep -Fq -- '- [x] **ITEM-333**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-333 complete"
grep -Fq -- '- [x] **ITEM-334**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-334 complete"
grep -Fq -- '- [x] **ITEM-335**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-335 complete"
grep -Fq -- '- [x] **ITEM-336**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-336 complete"
grep -Fq -- '- [x] **ITEM-337**' "$ROADMAP" ||
    fail "roadmap plan does not mark ITEM-337 complete"

for item in $(seq 302 337); do
    count=$(grep -Fc -- "**ITEM-$item**" "$ROADMAP" || true)
    [[ $count == 1 ]] ||
        fail "ITEM-$item must appear exactly once in the roadmap status list"
done

echo "Item 301 audit document test: OK"
