#!/usr/bin/env bash
# QEMU no-PS/2 proof that physical SATA acceptance phase 1 warm-resets to
# phase 2 on the same boot.
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

if [[ "${HFOC_PHYSICAL_SATA_WARM_RESET_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PHYSICAL_SATA_WARM_RESET_TIMEOUT:-90}s" \
        env HFOC_PHYSICAL_SATA_WARM_RESET_INNER=1 "$0" "$@"
fi

IMAGE=${IMAGE:-build/physical-sata-acceptance/hfoc-os-$(<VERSION)-physical-sata-acceptance.iso}
TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/physical-sata.raw
SERIAL_LOG=$TEST_DIR/serial.log
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    set +e
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( rc != 0 )); then
        artifact_dir="build/test-artifacts/physical-sata-warm-reset-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_REASON:-Physical SATA warm reset proof failed}" \
            >"$artifact_dir/failure-reason.txt"
        cp -a "$SERIAL_LOG" "$QEMU_LOG" "$DISK" "$artifact_dir/" 2>/dev/null || true
        echo "Physical SATA warm reset artifacts: $artifact_dir" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$rc"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

truncate -s 1G "$DISK"
: >"$SERIAL_LOG"

qemu-system-x86_64 \
    -machine q35,i8042=off \
    -m 512M \
    -smp 1 \
    -display none \
    -boot order=d,menu=off \
    -cdrom "$IMAGE" \
    -drive "if=none,id=sata-disk,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device "ide-hd,drive=sata-disk,bus=ide.0" \
    -serial "file:$SERIAL_LOG" >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

if ! python3 - "$SERIAL_LOG" "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
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
deadline = time.monotonic() + 80.0

while time.monotonic() < deadline:
    try:
        text = SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        text = ""
    if "[AHCI] physical boot 1 marker written" not in text:
        if "[AHCI] failed:" in text or "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("physical SATA warm reset proof reported kernel failure")
    if "[AHCI] physical boot 2 marker written" in text:
        break
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        raise SystemExit("QEMU exited before phase 2 marker")
    time.sleep(0.05)
else:
    raise SystemExit("timed out waiting for phase 2 after phase 1 warm reset")

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

print("physical SATA phase 1 warm reset to phase 2: found")
PY
then
    FAILURE_REASON="Physical SATA warm reset serial proof failed"
    exit 1
fi

wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

if ! python3 - "$DISK" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
with path.open("rb") as handle:
    handle.seek(1024 * 512)
    sector = handle.read(512)
if sector[:16] != b"HFOCPHYS2P2MARK\n":
    raise SystemExit("phase 2 marker was not persisted")
print("physical SATA phase 2 marker persisted")
PY
then
    FAILURE_REASON="Physical SATA warm reset phase 2 marker check failed"
    exit 1
fi

echo "Physical SATA warm reset QEMU proof: OK"
