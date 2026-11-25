#!/usr/bin/env bash
# Run the existing boot-policy QEMU proof as the release QEMU-liveness check.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MAKE_CMD=${HFOC_OPERATIONAL_QEMU_MAKE_CMD:-${HFOC_MAKE_CMD:-make}}
LOG=$(mktemp)

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -f "$LOG"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

set +e
"$MAKE_CMD" test-boot-policy-regression >"$LOG" 2>&1
status=$?
set -e
cat "$LOG"
if (( status != 0 )); then
    echo "Release operational QEMU liveness failed with status ${status}" >&2
    exit "$status"
fi
grep -Fq 'QEMU boot policy regression test: OK' "$LOG" || {
    echo "Release operational QEMU liveness missing boot-policy OK marker" >&2
    exit 1
}
echo "Release operational QEMU liveness: ALL OK"
