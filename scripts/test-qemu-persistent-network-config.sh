#!/usr/bin/env bash
# Prove item-309 persistent network configuration in a macro-gated QEMU boot:
# safe DHCP default, DHCP/STATIC commit and fresh-load persistence, invalid
# rejection, schema-invalid recovery, and clean shutdown.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PERSISTENT_NETWORK_CONFIG_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PERSISTENT_NETWORK_CONFIG_TEST_TIMEOUT:-60}s" \
        env HFOC_PERSISTENT_NETWORK_CONFIG_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Persistent network config test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Persistent network config test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Persistent network config test must not set QEMU_CPU" >&2
    exit 1
}

echo "QEMU persistent network config baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/persistent-network-config-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Persistent network config test failed}" \
        >"$artifact_dir/failure-reason.txt"
    cp -a "$SERIAL_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" "$artifact_dir/"
    echo "Persistent network config artifacts: $artifact_dir" >&2
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        save_failure_artifacts
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        local deadline=$((SECONDS + 3))
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            if (( SECONDS >= deadline )); then
                kill -KILL "$QEMU_PID" 2>/dev/null || true
                break
            fi
            sleep 0.1
        done
    fi
    if [[ -n "$QEMU_PID" ]]; then
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" -serial "file:$SERIAL_LOG" \
    -monitor none -d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import re
import signal
import sys
import time
import os

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
deadline = time.monotonic() + 45.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

while time.monotonic() < deadline:
    text = serial_text()
    if "[NETWORK-PERSISTENT] proof OK" in text:
        break
    if "[NETWORK-PERSISTENT] failed" in text:
        raise SystemExit("Persistent network config proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before persistent network config proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="persistent network config proof markers" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for persistent network config proof markers")

text = serial_text()
required = [
    "[NETWORK-PERSISTENT] proof init",
    "[NETWORK-PERSISTENT] default DHCP config loaded",
    "[NETWORK-PERSISTENT] DHCP config committed",
    "[NETWORK-PERSISTENT] fresh DHCP config read persisted",
    "[NETWORK-PERSISTENT] STATIC config committed",
    "[NETWORK-PERSISTENT] fresh STATIC config read persisted",
    "[NETWORK-PERSISTENT] invalid STATIC config rejected",
    "[NETWORK-PERSISTENT] invalid record recovered to default",
    "[NETWORK-PERSISTENT] post-recovery persistence proven",
    "[NETWORK-PERSISTENT] clean shutdown",
    "[NETWORK-PERSISTENT] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[NETWORK-PERSISTENT] failed" in text:
    raise SystemExit("Persistent network config proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before test completion")
time.sleep(0.3)

print("Persistent network config proof init: found")
print("Persistent network config proof OK: found")
print("Kernel panic: none")
print("QEMU stopped after proof: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU persistent network config test: OK"
