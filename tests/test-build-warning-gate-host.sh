#!/usr/bin/env bash
# ITEM-432: host proof for the build-warning gate.
#
# The gate is the only thing that reads the build the acceptance phase
# produces, so its own reading is driven here on captured logs: a `kernel/`
# warning fails it (and every shape GCC emits for our sources is counted), a
# third-party warning does not, and a log that cannot be read is a failure of
# the gate - never a clean verdict.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
GATE="$ROOT/scripts/hfoc-build-warning-gate.sh"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-build-warning-test.XXXXXX")
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

checks=0
failures=0
fail() {
    printf 'build warning gate host test: FAIL: %s\n' "$*" >&2
    exit 1
}
check() {
    local condition=$1
    local message=$2

    checks=$((checks + 1))
    if [[ $condition != 1 ]]; then
        failures=$((failures + 1))
        printf 'build warning gate host test: FAIL: %s\n' "$message" >&2
    fi
}

run_case() {
    local label=$1
    local expected_status=$2
    local expected_marker=$3

    shift 3
    out=$("$GATE" "$@" 2>&1) && status=0 || status=$?
    check "$([[ $status -eq $expected_status ]] && echo 1 || echo 0)" \
        "$label: exit $status, expected $expected_status ($out)"
    check "$([[ $out == *"$expected_marker"* ]] && echo 1 || echo 0)" \
        "$label: the marker does not say '$expected_marker': $out"
}

log=$WORK/build.log

# A clean build: the gate is silent about everything that is not ours.
printf 'cc kernel/src/main.c\nld -o build/hfoc-kernel.elf\n' >"$log"
run_case "clean build" 0 "HFOC-BUILD-WARNINGS state=CLEAN kernel=0" "$log"

# One kernel warning, in each shape the toolchain emits for our sources.
printf 'kernel/src/platform_reset.c:385:44: warning: unused variable\n' >"$log"
run_case "a kernel warning" 1 "HFOC-BUILD-WARNINGS state=FAIL kernel=1" "$log"

printf 'kernel/src/boot.S:12: Warning: end of file not at end of a line\n' >"$log"
run_case "an assembler warning" 1 "HFOC-BUILD-WARNINGS state=FAIL kernel=1" "$log"

{
    printf 'kernel/src/a.c:1:1: warning: first\n'
    printf 'kernel/include/b.h:2:3: warning: second\n'
} >"$log"
run_case "two kernel warnings" 1 "HFOC-BUILD-WARNINGS state=FAIL kernel=2" "$log"

# Warnings that are not ours must not block a phase.
{
    printf 'third_party/lwip/src/core/ip.c:7:1: warning: not ours\n'
    printf 'build/host-tool.c:9:1: warning: also not ours\n'
} >"$log"
run_case "third-party warnings only" 0 \
    "HFOC-BUILD-WARNINGS state=CLEAN kernel=0" "$log"

# A log that cannot be read is a failure of the gate, never a clean verdict:
# missing, unreadable (the branch a bare `grep` would swallow), or empty (a
# build whose output never arrived).
run_case "missing log" 1 "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE" \
    "$WORK/absent.log"
: >"$log"
run_case "empty log" 1 "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_EMPTY" "$log"
printf 'kernel/src/main.c:1:1: warning: hidden\n' >"$WORK/unreadable.log"
chmod 000 "$WORK/unreadable.log"
run_case "unreadable log" 1 \
    "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE" "$WORK/unreadable.log"
chmod 600 "$WORK/unreadable.log"

# Usage errors are their own code and print the contract.
out=$("$GATE" 2>&1) && status=0 || status=$?
check "$([[ $status -eq 64 ]] && echo 1 || echo 0)" \
    "a missing log argument is a usage error (exit $status)"
check "$([[ $out == *'HFOC-BUILD-WARNINGS'* ]] && echo 1 || echo 0)" \
    "the usage text states the marker the gate prints"

if [[ $failures -ne 0 ]]; then
    printf 'build warning gate host test: %d checks, %d failures\n' \
        "$checks" "$failures"
    exit 1
fi
printf 'build warning gate host test: OK (%d checks)\n' "$checks"
