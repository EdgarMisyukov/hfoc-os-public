#!/usr/bin/env bash
# ITEM-557: the refusal line `diag save` prints, executed by the real console.
#
# The owner's hardware run ended in `DIAG FAILED` with no line he could read
# whole, and no way to ask again: the next attempt's reset erased the answer.
# This proof boots the shipped console medium as an optical disc - so the boot
# medium cannot carry the journal and `diag save` must refuse with NO_MEDIUM -
# and asserts what the operator now gets:
#
#   DIAG FAILED reason=NO_MEDIUM step=MEDIUM_PROBE     one row, short
#   DIAG DETAIL step=... cause=... ehci=... medium=... journal=...   the facts
#   DIAG LAST ... / DIAG DETAIL ...                    the same, asked again
#
# The proof is not a hardware proof and not an HS-hub proof: it exercises the
# console + diag code path that produces the operator's line, on the guest's
# own serial stream, with the journal deliberately unavailable - the state the
# Toshiba reported.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

VERSION_VALUE=$(cat VERSION)
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$VERSION_VALUE-production-installer-console.iso}
test -f "$IMAGE" || {
    echo "Missing proof medium: $IMAGE (run make production-installer-console-media)" >&2
    exit 1
}
EXPECTED_BUILD_ID=${EXPECTED_BUILD_ID:-}
EXPECTED_KERNEL=${EXPECTED_KERNEL:-}
test -n "$EXPECTED_BUILD_ID" || {
    echo "EXPECTED_BUILD_ID is empty: the proof cannot tell which build it boots" >&2
    exit 1
}
if [[ -n $EXPECTED_KERNEL ]]; then
    test -f "$EXPECTED_KERNEL" || {
        echo "Missing proof kernel: $EXPECTED_KERNEL" >&2
        exit 1
    }
    # One process reads the kernel: the old `strings | grep -q` pipeline
    # returned 141 under pipefail when grep matched and strings was still
    # writing, so a present build id looked absent (ITEM-641).
    bash scripts/hfoc-file-marker.sh "$EXPECTED_KERNEL" "$EXPECTED_BUILD_ID" || {
        echo "the proof kernel $EXPECTED_KERNEL does not carry build id $EXPECTED_BUILD_ID" >&2
        exit 1
    }
fi

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
QEMU_PID=
: >"$SERIAL_LOG"
: >"$QEMU_LOG"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -n "${QEMU_PID:-}" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( status != 0 )); then
        echo "----- serial tail -----" >&2
        tail -c 4000 "$SERIAL_LOG" >&2 || true
        echo "----- qemu log -----" >&2
        tail -c 2000 "$QEMU_LOG" >&2 || true
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# One persistent monitor connection: a connection opened, written and closed at
# once can lose the command before QEMU parses it.
monitor_commands() {
    python3 - "$MONITOR_SOCKET" "$@" <<'PY'
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
buffer = b""
for command in sys.argv[2:]:
    sock.sendall(command.encode("ascii") + b"\n")
    waited = time.monotonic() + 2.0
    while time.monotonic() < waited:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buffer += chunk
        if b"(qemu)" in buffer:
            buffer = b""
            break
    time.sleep(0.05)
sock.close()
PY
}

wait_for() {
    local marker=$1
    local budget=${2:-90}
    python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" "$marker" \
        "$budget" <<'PY'
import importlib.util
import pathlib
import sys
import time

spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
path = pathlib.Path(sys.argv[1])
marker = sys.argv[3]
deadline = time.monotonic() + float(sys.argv[4])
while time.monotonic() < deadline:
    try:
        text = module.normalise(path.read_bytes().decode("ascii",
                                                         errors="replace"))
    except FileNotFoundError:
        text = ""
    if marker in text:
        raise SystemExit(0)
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic before the marker")
    time.sleep(0.25)
sys.stderr.write(
    'HFOC-QEMU-MARKER-TIMEOUT marker="' + marker + '" deadline='
    + str(int(float(sys.argv[4]))) + 's\n')
sys.stderr.flush()
raise SystemExit(f"timed out waiting for {marker!r}")
PY
}

echo "diag-failure proof: booting $IMAGE as an optical disc (no journal medium)"
qemu-system-x86_64 \
    -machine pc -m 512M -smp 1 -display none -no-reboot \
    -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

# The machine has no disk to install on, so it paints its refusal page and waits
# for a key; the console prompt is behind it.
wait_for "Press any key to continue." 180
monitor_commands "sendkey ret"
wait_for "hfoc>" 120

# ITEM-558: the build identity the operator can read without any medium.  The
# kernel now prints it on the existing `version` command; the proof refuses a
# guest that names another revision.
monitor_commands "sendkey v" "sendkey e" "sendkey r" "sendkey s" \
    "sendkey i" "sendkey o" "sendkey n" "sendkey ret"
wait_for "HFOC OS 1.0.0 X86-64 build=${EXPECTED_BUILD_ID}" 60

# The operator's one command, on a machine whose journal medium is a disc.
keys=()
for key in d i a g spc s a v e ret; do
    keys+=("sendkey $key")
done
monitor_commands "${keys[@]}"
wait_for "DIAG FAILED reason=NO_MEDIUM step=MEDIUM_PROBE" 120

# The operator asks again: the refusal has to survive the next command.
monitor_commands "sendkey d" "sendkey i" "sendkey a" "sendkey g" "sendkey ret"
wait_for "DIAG LAST reason=NO_MEDIUM step=MEDIUM_PROBE" 120

monitor_commands quit
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
text = module.normalise(pathlib.Path(sys.argv[1]).read_bytes()
                        .decode("ascii", errors="replace"))
lines = text.splitlines()


def fail(message):
    print("diag-failure proof: " + message, file=sys.stderr)
    for line in lines[-12:]:
        print("  " + line, file=sys.stderr)
    raise SystemExit(1)


version = [line for line in lines
           if line.startswith("HFOC OS 1.0.0 X86-64 ")]
if not version:
    fail("the `version` command printed no build identity")
if " build=" not in version[-1] or " version=" not in version[-1]:
    fail("the version line does not carry the build identity: " + version[-1])

short = [line for line in lines if line.startswith("DIAG FAILED ")]
if not short:
    fail("the machine never printed DIAG FAILED")
short_line = short[-1]
if "reason=NO_MEDIUM" not in short_line or "step=MEDIUM_PROBE" not in short_line:
    fail("the short line does not name the cause and the stage: " + short_line)
# The operator reads this line on one row of an 80-column screen: the line is
# bounded by construction, so a wrap can never hide the stage.
if len(short_line) > 80:
    fail("the short line is longer than one 80-column row: %d" % len(short_line))

details = [line for line in lines if line.startswith("DIAG DETAIL ")]
if not details:
    fail("the machine never printed the detail line")
detail = details[-1]
for field in ("step=MEDIUM_PROBE", "cause=NO_MEDIUM", "ehci=", "medium=",
              "journal="):
    if field not in detail:
        fail("the detail line does not carry %r: %s" % (field, detail))

last = [line for line in lines if line.startswith("DIAG LAST ")]
if not last:
    fail("the `diag` command did not repeat the refusal")
if "reason=NO_MEDIUM" not in last[-1] or "step=MEDIUM_PROBE" not in last[-1]:
    fail("the repeated refusal lost the cause or the stage: " + last[-1])
# The rebuilt refusal must be the same one, detail included.
if details[-1] not in text[text.index(last[-1]):]:
    fail("the detail line after `diag` is not the recorded one")

print("diag-failure proof: the short line, the detail and the repeated answer agree")
PY

echo "DIAG-FAILURE qemu proof: OK"
