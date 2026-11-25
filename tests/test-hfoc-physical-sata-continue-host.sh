#!/usr/bin/env bash
# Regression test for host-side physical SATA marker LF normalization.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

source scripts/hfoc-physical-sata-continue.sh

fail() {
    echo "$*" >&2
    exit 1
}

assert_normalized() {
    local expected=$1
    local raw=$2
    local label=$3
    local normalized

    normalized=$(hfoc_physical_sata_normalize_marker "$raw")
    [[ "$normalized" == "$expected" ]] ||
        fail "$label: expected '$expected', got '$normalized'"
}

phase1=$HFOC_PHYSICAL_SATA_PHASE1_MARKER
phase2=$HFOC_PHYSICAL_SATA_PHASE2_MARKER

assert_normalized "$phase1" "$phase1" "phase1 marker without trailing LF"
assert_normalized "$phase1" "${phase1}"$'\n' "phase1 marker with trailing LF"
assert_normalized "$phase2" "$phase2" "phase2 marker without trailing LF"
assert_normalized "$phase2" "${phase2}"$'\n' "phase2 marker with trailing LF"

# Reproduce the previous host failure: command substitution removes LF while
# the old comparison still expected the literal LF byte.
marker_from_command_substitution=$(printf '%s\n' "$phase1")
if [[ "$marker_from_command_substitution" == "${phase1}"$'\n' ]]; then
    fail "old newline comparison unexpectedly matched after command substitution"
fi
normalized=$(hfoc_physical_sata_normalize_marker "$marker_from_command_substitution")
[[ "$normalized" == "$phase1" ]] ||
    fail "normalized marker after command substitution mismatch"

# A different marker must not be accepted as phase1.
normalized=$(hfoc_physical_sata_normalize_marker "${phase2}"$'\n')
[[ "$normalized" == "$phase2" ]] ||
    fail "phase2 marker was not preserved"
[[ "$normalized" != "$phase1" ]] ||
    fail "phase2 marker was misclassified as phase1"

echo 'HFOC physical SATA continuation marker normalization host test: OK'
