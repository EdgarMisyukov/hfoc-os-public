#!/usr/bin/env bash
# Prove the first embedded user init process in QEMU: a parent process waits
# and reaps a user child; the child runs Ring 3 code, validates write, yields,
# exits, and the parent restores the syscall ABI MSRs.
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

if [[ "${HFOC_USER_INIT_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_USER_INIT_TEST_TIMEOUT:-60}s" \
        env HFOC_USER_INIT_TEST_INNER=1 "$0" "$@"
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
USE_MONITOR=1
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"

if ! python3 - "$MONITOR_SOCKET"   <<'PY'
import os
import socket
import sys

path = sys.argv[1]
try:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(path)
    sock.close()
    os.unlink(path)
except OSError:
    raise SystemExit(1)
PY
then
    USE_MONITOR=0
    MONITOR_SOCKET=
fi

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/user-init-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-User init test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "User init test artifacts: $artifact_dir" >&2
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

QEMU_ARGS=(
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG"
)
if [[ "$USE_MONITOR" == 1 ]]; then
    QEMU_ARGS+=(-monitor "unix:$MONITOR_SOCKET,server,nowait")
else
    QEMU_ARGS+=(-monitor none)
fi
QEMU_ARGS+=(-d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown)
qemu-system-x86_64 "${QEMU_ARGS[@]}" >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QEMU_PID" "$USE_MONITOR" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import signal
import sys
import time
import os

serial_path = pathlib.Path(sys.argv[1])
monitor_socket = pathlib.Path(sys.argv[2]) if sys.argv[2] else None
qemu_pid = int(sys.argv[3])
use_monitor = sys.argv[4] == "1"
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
            serial_path.read_bytes().decode("utf-8", errors="replace"))
    except FileNotFoundError:
        return ""

while time.monotonic() < deadline:
    text = serial_text()
    if "[USER-INIT] proof OK" in text:
        break
    if "[USER-INIT] failed" in text:
        raise SystemExit("User init proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before user init proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="user init proof markers" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for user init proof markers")

text = serial_text()
#
# ITEM-626: the native utility phase runs after the exact-count ITEM-625
# matrix.  Its programs write their own bytes and exit, so the 625 counts are
# read from the segment before the boundary while the utility assertions read
# their own markers.  Neither set is relaxed or merged.
boundary = text.find("[USER-UTIL] start")
if boundary < 0:
    raise SystemExit("Native utility phase did not start")
legacy_text = text[:boundary]
util_text = text[boundary:]
required = [
    "[USER-INIT] proof init",
    "[IMAGE-INTEGRITY] mismatch rejected before init launch",
    "[IMAGE-INTEGRITY] verified initramfs before init launch",
    "[IDENTITY] identity v=1 class=hfoc-appliance product=HFOC-APPLIANCE-V1 arch=x86_64 platform=q35 hostname=hfoc-appliance instance=default",
    "[SECURITY] capture OK pid=2 domain=1 generation=0",
    "[SECURITY] cross-domain rejected pid=2",
    "[SECURITY] stale-origin rejected pid=3",
    "[CAPABILITY] child syscall capabilities OK pid=2",
    "[CAPABILITY] write denied pid=4",
    "[CAPABILITY] denied state unchanged pid=4",
    "[USER-INIT] ring3 marker OK",
    "[USER-INIT] write ok len=6",
    "[USER-EXEC] spawn pid=",
    "[USER-EXEC] spawned",
    "[USER-EXEC] reaped pid=",
    "[USER-EXEC] crash spawn pid=",
    "[USER-EXEC] crash reaped pid=",
    "[USER-EXEC] exec spawn pid=",
    "[USER-EXEC] exec reaped pid=",
    "[USER-EXEC] exec pid=",
    "[USER-EXEC] exec failed USER_PROCESS_NOT_FOUND",
    "[USER-EXEC] missing spawn pid=",
    "[USER-EXEC] missing reaped pid=",
    "[USER-EXEC] pair spawn pid=",
    "[USER-EXEC] pair-a reaped pid=",
    "[USER-EXEC] pair-b reaped pid=",
    "[USER-EXEC] cancel spawn pid=",
    "[USER-EXEC] cancel pid=",
    "[USER-EXEC] cancel reaped pid=",
    "[USER-EXEC] leak check free=",
    "[USER-FAULT] vector=14 ",
    "[AUDIT-OBS] consumed index=3 ",
    "[AUDIT-OBS] consumed index=4 ",
    "[USER-INIT] yield ok",
    "[USER-FS] op=4 result=3",
    "[USER-FS] op=14 result=5",
    "[USER-FS] op=8 result=0",
    "[USER-FS] op=3 result=5",
    "[USER-FS] op=5 result=0",
    "[USER-INIT] parent reaped all",
    "[AUDIT-OBS] published operation=0 result=6",
    "[AUDIT-OBS] published operation=1 result=0",
    "[AUDIT-OBS] consumer start",
    "[AUDIT-OBS] consumed index=0 sequence=0 type=2 operation=0 result=6 pid=2 domain=1 generation=0",
    "[AUDIT-OBS] consumed index=1 sequence=1 type=2 operation=1 result=0 pid=2 domain=1 generation=0",
    "[AUDIT-OBS] consumed index=2 sequence=2 type=2 operation=2 result=0 pid=2 domain=1 generation=0",
    "[AUDIT-OBS] empty after exact consume",
    "[AUDIT-OBS] parent consumed exactly once",
]
for marker in required:
    if legacy_text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {legacy_text.count(marker)}: {marker}")
if text.count("[USER-INIT] proof OK") != 1:
    raise SystemExit(
        f"Expected 1 final proof marker, got {text.count('[USER-INIT] proof OK')}")
if legacy_text.count("[USER-INIT] write ok len=11") != 4:
    raise SystemExit(
        "Expected 4 hello writes, got "
        f"{legacy_text.count('[USER-INIT] write ok len=11')}")
if legacy_text.count("[USER-INIT] write ok len=10") != 1:
    raise SystemExit(
        "Expected 1 kept-image write, got "
        f"{legacy_text.count('[USER-INIT] write ok len=10')}")
if legacy_text.count("[USER-INIT] write ok len=9") != 1:
    raise SystemExit(
        "Expected 1 spin write, got "
        f"{legacy_text.count('[USER-INIT] write ok len=9')}")
if legacy_text.count("[AUDIT-OBS] published operation=2 result=0") != 6:
    raise SystemExit(
        "Expected 6 exit publications, got "
        f"{text.count('[AUDIT-OBS] published operation=2 result=0')}")
if legacy_text.count("[USER-INIT] exit syscall") != 6:
    raise SystemExit(
        "Expected 6 user exit records, got "
        f"{text.count('[USER-INIT] exit syscall')}")
if legacy_text.count("[SECURITY] syscall origin validated pid=2") != 20:
    raise SystemExit(
        "Expected 20 user syscall origin validations, got "
        f"{text.count('[SECURITY] syscall origin validated pid=2')}")
util_required = [
    "[USER-UTIL] spawn ls-long",
    "[USER-UTIL] spawn mkdir ",
    "[USER-UTIL] spawn cp ",
    "[USER-UTIL] spawn cat ",
    "[USER-UTIL] spawn wc ",
    "[USER-UTIL] spawn grep ",
    "[USER-UTIL] spawn mv ",
    "[USER-UTIL] spawn cp-second",
    "[USER-UTIL] spawn rm-renamed",
    "[USER-UTIL] roundtrip OK bytes=8",
    "[USER-UTIL] spawn cp-readonly",
    "[USER-UTIL] spawn cat-missing",
    "[USER-UTIL] spawn echo",
    "[USER-UTIL] spawn true",
    "[USER-UTIL] spawn false",
    "[USER-UTIL] spawn pwd",
    "[USER-UTIL] spawn ls ",
    "[USER-UTIL] spawn hfoc ",
    "[USER-UTIL] spawn hfoc-env",
    "[USER-PIPE] start",
    "[USER-PIPE] stream OK bytes=8192",
    "[USER-PIPE] dup OK bytes=1024",
    "[USER-PIPE] broken-pipe status=42",
    "[USER-PIPE] cancel OK status=130",
    "[USER-PIPE] proof OK",
    "[USER-UTIL] spawn hfoc-exec ",
    "[USER-UTIL] spawn hfoc-exec-missing",
    "[USER-UTIL] spawn hfoc-exec-bad",
    "[USER-UTIL] spawn mkdir-unicode",
    "[USER-UTIL] spawn cp-unicode",
    "[USER-UTIL] spawn ls-unicode",
    "[USER-UTIL] spawn cat-unicode",
    "[USER-UTIL] unicode OK",
    "[USER-UTIL] spawn cp-enospc",
    "[USER-UTIL] enospc refused",
    "[USER-UTIL] spawn rm-big",
    "[USER-UTIL] proof OK",
    "[USER-SH] start",
    "[USER-SH] script OK",
    "[USER-SH] inline OK",
    "[USER-SH] scripts OK",
    "[USER-SH] syntax OK",
    "[USER-SH] proof OK",
    "[USER-UTIL] leak check free=",
]
for marker in util_required:
    if util_text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 utility marker, got {util_text.count(marker)}: {marker}")
# The programs' own output proves the bytes reached stdout, not just that a
# process exited.  Kernel-side file contents were checked before the marker.
for output in [
    "hello-native",
    "HFOC OS",
    "1 2 8 /tmp/utildir/motd",
    "hfoc-exec reaped",
    "status=127",
    "status=126",
    "status=1",
    "HOME=/tmp",
    "HFOC native user runtime",
    "replaced-ok",
    "файл",
]:
    if output not in util_text:
        raise SystemExit(f"Native utility output missing: {output}")
if "[USER-UTIL] failed" in text:
    raise SystemExit("Native utility phase reported failure")
if "[USER-SH] failed" in text:
    raise SystemExit("Native shell phase reported failure")
if "[USER-INIT] failed" in text:
    raise SystemExit("User init proof reported failure")
if "[SECURITY] failed" in text:
    raise SystemExit("Security origin proof reported failure")
if "[CAPABILITY] failed" in text:
    raise SystemExit("Capability gate proof reported failure")
if "[AUDIT-OBS] failed" in text:
    raise SystemExit("Audit observability proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

if use_monitor:
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
else:
    try:
        os.kill(qemu_pid, signal.SIGTERM)
    except ProcessLookupError:
        raise SystemExit("QEMU exited before test completion")
    time.sleep(0.2)

print("User init proof init: found")
print("User init proof OK: found")
print("Kernel panic: none")
if use_monitor:
    print("QEMU alive before quit: yes")
else:
    print("QEMU stopped after proof: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU user init test: OK"
