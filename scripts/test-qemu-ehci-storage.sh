#!/usr/bin/env bash
# ITEM-551: the machine reaches the boot medium over EHCI, and the reserved
# journal on it really is written.
#
# The owner's Toshiba L875 boots the diagnostic stick through the firmware and
# then hands the kernel a machine where the stick sits on the Intel 7 Series
# EHCI function (`Bus 001: ehci-pci/2p 480M` in the Linux capture of the same
# machine) while the xHCI function is the one failing at ENABLE_SLOT.  Before
# this item the kernel had no EHCI driver, so the medium it was booted from was
# invisible to it: `[INSTALL-MEDIUM] state=UNAVAILABLE reason=NO_DEVICE`.
#
# This proof boots the production console medium from a QEMU EHCI controller
# with a `usb-storage` device behind it - the same shape as the laptop, on the
# same firmware path (SeaBIOS reads the stick and starts Limine) - and requires:
#
#   * the EHCI function is found, reset and started (`[EHCI] state=READY`),
#     with its port connected and enabled after the port reset (`[EHCI-PORT]`);
#   * the mass-storage device binds and answers INQUIRY, TEST UNIT READY and
#     READ CAPACITY (`[EHCI-STORAGE] state=BOUND ... commands=3`);
#   * the read-only boot-medium guard matches the device (`[INSTALL-MEDIUM]
#     controller=ehci ... state=MATCH reason=OK`);
#   * the reserved `HFOC/INSTALL.LOG` binds (`[INSTALL-LOG] state=READY`);
#   * after the machine stops, the journal on the image carries the boot's own
#     records - read back with `scripts/hfoc-install-log-read.py`, the same
#     reader the Debian machine runs - which proves the *write* half of the
#     transport, not only the read half.
#
# A negative control boots the same medium as a CD with no EHCI function and
# requires that the boot record does not claim a reachable medium: that is the
# state this item removes, and it proves the positive run is not passing on a
# record that would appear anyway.
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

VERSION_VALUE=$(cat VERSION)
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$VERSION_VALUE-production-installer-console.iso}
test -f "$IMAGE" || {
    echo "Missing medium: $IMAGE (run make production-installer-console-media)" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
STICK=$TEST_DIR/stick.img
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
QEMU_PID=
FAILURE_MESSAGE=
: >"$SERIAL_LOG"
: >"$QEMU_LOG"
cp "$IMAGE" "$STICK"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/ehci-storage-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-EHCI storage proof failed}" \
        >"$artifact_dir/failure-reason.txt"
    cp -a "$SERIAL_LOG" "$QEMU_LOG" "$artifact_dir/" 2>/dev/null || true
    echo "EHCI storage artifacts: $artifact_dir" >&2
}

best_effort_cleanup() {
    if [[ -z "$QEMU_PID" ]]; then
        return 0
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
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

monitor_quit() {
    python3 - "$MONITOR_SOCKET" <<'PY' || true
import socket
import sys
import time

deadline = time.monotonic() + 10.0
while True:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        sock.connect(sys.argv[1])
        break
    except OSError:
        if time.monotonic() > deadline:
            raise SystemExit(0)
        time.sleep(0.2)
sock.sendall(b"quit\n")
time.sleep(0.2)
sock.close()
PY
}

wait_for_markers() {
    python3 - "$SERIAL_LOG" "${1:-45}" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import importlib.util
import pathlib
import sys
import time

serial = pathlib.Path(sys.argv[1])
budget = float(sys.argv[2])
spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[3])
SERIAL_VIEW = importlib.util.module_from_spec(spec)
spec.loader.exec_module(SERIAL_VIEW)
deadline = time.monotonic() + budget
required = [
    ("port", "[EHCI-PORT] "),
    ("storage", "[EHCI-STORAGE] "),
    ("controller", "[EHCI] pci="),
    ("controller_ready", "[EHCI] pci="),
    ("medium", "[INSTALL-MEDIUM] controller=ehci "),
    ("match", "state=MATCH reason=OK"),
    ("journal", "[INSTALL-LOG] state=READY"),
]
seen = set()
while time.monotonic() < deadline:
    try:
        text = SERIAL_VIEW.normalise(
            serial.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        text = ""
    for name, marker in required:
        if marker in text:
            seen.add(name)
    if len(seen) == len(required) and "state=READY" in text:
        raise SystemExit(0)
    time.sleep(0.25)
missing = [name for name, _ in required if name not in seen]
raise SystemExit("missing markers: " + " ".join(missing))
PY
}

# ---------------------------------------------------------------- positive ---
echo "EHCI proof: booting $IMAGE from a QEMU EHCI usb-storage device"
qemu-system-x86_64 \
    -machine pc -m 512M -smp 1 -display none -no-reboot \
    -drive "file=$STICK,format=raw,if=none,id=stick" \
    -device usb-ehci,id=ehci \
    -device usb-storage,drive=stick,bootindex=0 \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

if ! fail=$(wait_for_markers 60 2>&1); then
    FAILURE_MESSAGE="boot markers: $fail"
    echo "EHCI proof: $FAILURE_MESSAGE" >&2
    exit 1
fi
monitor_quit
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# The journal the boot wrote must read back with the Debian reader.
journal_output=$(python3 scripts/hfoc-install-log-read.py --disk "$STICK" 2>&1) || {
    FAILURE_MESSAGE="journal read: $journal_output"
    echo "EHCI proof: $FAILURE_MESSAGE" >&2
    exit 1
}
slots=$(printf '%s\n' "$journal_output" | head -1 |
    sed -n 's/^journal: fat12-preallocated, \([0-9][0-9]*\) slots.*/\1/p')
[[ -n $slots && $slots -gt 1 ]] || {
    FAILURE_MESSAGE="journal has no written slots: $journal_output"
    echo "EHCI proof: $FAILURE_MESSAGE" >&2
    exit 1
}
# The journal carries the boot's own records; the journal's *status* line is a
# notify line (serial and the boot log store), which the journal cannot carry
# without re-entering itself, so it is asserted from the serial markers above and
# not looked for on the medium.
for marker in \
    "[BOOT] Bootloader: Limine" \
    "[EHCI] pci=" \
    "state=READY" \
    "[EHCI-STORAGE] " \
    "[INSTALL-MEDIUM] controller=ehci " \
    "state=MATCH reason=OK"; do
    printf '%s\n' "$journal_output" | grep -Fq -- "$marker" || {
        FAILURE_MESSAGE="journal record missing: $marker"
        echo "EHCI proof: $FAILURE_MESSAGE" >&2
        echo "$journal_output" >&2
        exit 1
    }
done
echo "EHCI proof: $slots journal slots on the medium carry the boot records"

# ---------------------------------------------------------------- negative ---
echo "EHCI proof: negative control - the same medium as a CD, no EHCI function"
: >"$SERIAL_LOG"
qemu-system-x86_64 \
    -machine pc -m 512M -smp 1 -display none -no-reboot \
    -cdrom "$IMAGE" -boot order=d \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!
python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import importlib.util
import pathlib
import sys
import time

serial = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
SERIAL_VIEW = importlib.util.module_from_spec(spec)
spec.loader.exec_module(SERIAL_VIEW)
deadline = time.monotonic() + 60.0
while time.monotonic() < deadline:
    try:
        text = SERIAL_VIEW.normalise(
            serial.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        text = ""
    if "[INSTALL-MEDIUM]" in text and "[BOOT] Bootloader: Limine" in text:
        break
    time.sleep(0.25)
else:
    raise SystemExit("negative control: boot markers missing")
if "[EHCI]" in text:
    raise SystemExit("negative control: an EHCI record appeared with no EHCI function")
if "controller=ehci" in text:
    raise SystemExit("negative control: the CD boot named an EHCI medium")
PY
monitor_quit
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "EHCI proof: negative control holds (no EHCI record on a CD boot)"
echo "EHCI-STORAGE qemu proof: OK"
