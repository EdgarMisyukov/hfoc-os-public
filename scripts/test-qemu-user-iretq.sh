#!/usr/bin/env bash
# Prove the first macro-gated Ring 3 transition: kernel iretq enters user
# code once, user code records its Ring 3 selector, iretq returns to Ring 0,
# and the kernel validates the recorded user marker before clean shutdown.
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

if [[ "${HFOC_USER_IRETQ_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_USER_IRETQ_TEST_TIMEOUT:-60}s" \
        env HFOC_USER_IRETQ_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "APIC baseline must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "APIC baseline must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "APIC baseline must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU APIC baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/user-iretq-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-User iretq test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "User iretq test artifacts: $artifact_dir" >&2
}

best_effort_cleanup() {
    if [[ -z "$QEMU_PID" ]]; then
        return 0
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=
        return 0
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    local deadline=$((SECONDS + 2))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        save_failure_artifacts
    fi
    best_effort_cleanup
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int -D "$INTERRUPTS_LOG" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QEMU_PID" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import os
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_socket = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
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
    if "[USER] returned" in text and "[USER] proof OK" in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before user iretq proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="user iretq proof markers" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for user iretq proof markers")

text = serial_text()
if text.count("[USER] proof init") != 1:
    raise SystemExit(
        f"Expected 1 proof init marker, got {text.count('[USER] proof init')}")
if text.count("[USER] returned") != 1:
    raise SystemExit(
        f"Expected 1 returned marker, got {text.count('[USER] returned')}")
if text.count("[USER] proof OK") != 1:
    raise SystemExit(
        f"Expected 1 proof OK marker, got {text.count('[USER] proof OK')}")
if "[USER] failed" in text:
    raise SystemExit("User iretq proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

if not monitor_socket.exists():
    raise SystemExit("timed out waiting for QEMU monitor socket")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    try:
        monitor.connect(str(monitor_socket))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

    chunks = []
    prompt_deadline = time.monotonic() + 5.0
    while b"(qemu)" not in b"".join(chunks):
        remaining = prompt_deadline - time.monotonic()
        if remaining <= 0.0:
            raise SystemExit("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        chunks.append(chunk)
    monitor.sendall(b"quit\n")

try:
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit("QEMU exited before monitor quit")

print("User iretq proof init: found")
print("User iretq returned: found")
print("User iretq proof OK: found")
print("Kernel panic: none")
print("QEMU alive before quit: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU user iretq test: OK"
