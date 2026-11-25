#!/usr/bin/env bash
# Macro-gated QEMU proof for the ITEM-357 physical SATA acceptance path.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PHYSICAL_SATA_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PHYSICAL_SATA_TEST_TIMEOUT:-120}s" \
        env HFOC_PHYSICAL_SATA_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
QEMU_LABEL=${QEMU_LABEL:-baseline}
QEMU_MACHINE_OPTIONS=${QEMU_MACHINE_OPTIONS:-}
QEMU_PCIE_ROOT_PORT=${QEMU_PCIE_ROOT_PORT:-0}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Physical SATA acceptance test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Physical SATA acceptance test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Physical SATA acceptance test must not set QEMU_CPU" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/physical-sata.raw
SERIAL_1=$TEST_DIR/com1-boot1.log
SERIAL_2=$TEST_DIR/com1-boot2.log
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=
FAILURE_MESSAGE=

truncate -s 1G "$DISK"
: >"$QEMU_LOG"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    set +e
    if (( rc != 0 )); then
        artifact_dir="build/test-artifacts/physical-sata-acceptance-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-Physical SATA acceptance QEMU proof failed}" \
            >"$FAILURE_REASON"
        cp -a "$SERIAL_1" "$SERIAL_2" "$DISK" \
            "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
        echo "Physical SATA acceptance artifacts: $artifact_dir" >&2
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
    exit "$rc"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

run_boot() {
    local phase=$1
    local expected_marker=$2
    local serial_log=$3
    local output
    local -a qemu_args
    local machine

    : >"$serial_log"
    machine=$QEMU_MACHINE
    if [[ -n "$QEMU_MACHINE_OPTIONS" ]]; then
        machine="$QEMU_MACHINE,$QEMU_MACHINE_OPTIONS"
    fi
    qemu_args=(
        -machine "$machine"
        -m 512M
        -smp "$QEMU_SMP"
        -display none
        -boot "order=d,menu=off"
        -cdrom "$IMAGE"
        -drive "if=none,id=sata-disk,file=$DISK,format=raw,cache=writeback,file.locking=off"
        -device "ide-hd,drive=sata-disk,bus=ide.0"
    )
    if [[ "$QEMU_PCIE_ROOT_PORT" == 1 ]]; then
        qemu_args+=(-device "pcie-root-port,id=root0,chassis=1,slot=1")
    fi
    qemu_args+=(-serial "file:$serial_log" -no-reboot -no-shutdown)
    qemu-system-x86_64 "${qemu_args[@]}" >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    output=$(python3 - "$serial_log" "$QEMU_PID" \
        "$phase" "$expected_marker" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
phase = sys.argv[3]
expected = sys.argv[4]
deadline = time.monotonic() + 70.0

def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

while time.monotonic() < deadline:
    text = serial_text()
    if expected in text and "[AHCI] physical evidence proof OK" in text:
        break
    if "[AHCI] failed:" in text or "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit(f"{phase} reported kernel failure")
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {phase} physical SATA marker") + '" '
        'deadline=70s\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {phase} physical SATA marker")

text = serial_text()
if expected not in text:
    raise SystemExit(f"missing {expected} in {phase}")
if "[AHCI] physical evidence proof OK" not in text:
    raise SystemExit("missing physical evidence proof OK marker")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("kernel panic detected")

try:
    os.kill(qemu_pid, 0)
except ProcessLookupError:
    pass
else:
    os.kill(qemu_pid, 15)
    exit_deadline = time.monotonic() + 5.0
    while time.monotonic() < exit_deadline:
        try:
            os.kill(qemu_pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)

print(f"{phase} physical SATA marker: found")
print(f"{phase} physical evidence proof: OK")
PY
)
    if [[ -z "$output" ]]; then
        FAILURE_MESSAGE="QEMU phase $phase did not complete"
        printf '%s\n' "$FAILURE_MESSAGE" >&2
        return 1
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
    printf '%s\n' "$output"
}

phase1_output=$(run_boot "boot-1" "[AHCI] physical boot 1 marker written" "$SERIAL_1")
if ! python3 - "$DISK" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
with path.open("rb") as handle:
    handle.seek(1024 * 512)
    sector = handle.read(512)
if sector[:16] != b"HFOCPHYS1P1MARK\n":
    raise SystemExit("boot 1 did not persist phase 1 evidence at LBA 1024")
print("boot 1 evidence persisted: phase 1")
PY
then
    FAILURE_MESSAGE="boot 1 evidence did not persist"
    exit 1
fi

phase2_output=$(run_boot "boot-2" "[AHCI] physical boot 2 marker written" "$SERIAL_2")
if ! python3 - "$DISK" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
with path.open("rb") as handle:
    handle.seek(1024 * 512)
    sector = handle.read(512)
if sector[:16] != b"HFOCPHYS2P2MARK\n":
    raise SystemExit("boot 2 did not persist phase 2 evidence at LBA 1024")
print("boot 2 evidence persisted: phase 2")
PY
then
    FAILURE_MESSAGE="boot 2 evidence did not persist"
    exit 1
fi

printf '%s\n' "$phase1_output"
printf '%s\n' "$phase2_output"
echo "QEMU physical SATA acceptance test ($QEMU_LABEL): OK"
