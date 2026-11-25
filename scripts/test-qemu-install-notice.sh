#!/usr/bin/env bash
# ITEM-417: prove the operator is told on the screen, for as long as promised.
#
# The install flow runs before the kernel's console exists, so every record it
# publishes reaches the serial stream and is held back from the display: the
# machine of the 2026-09-15 acceptance run rebooted with nothing on the screen
# saying the install had finished.  This proof runs the flow in QEMU with a
# monitor attached and reads the guest's own display - the frame the person
# standing at the machine would see - while the machine is still up.
#
# Two things are required of that frame, because two things were promised:
#
#   * the banner is on it, and the countdown in the middle of it changes while
#     the machine waits - a fixed picture that happens to be on the screen is
#     not a countdown;
#   * the wait the record announces is the wait the machine actually takes.
#     The delay behind both was measured at 0.318 s against the 5 s the record
#     announced (2026-09-15): the completion record was written, the screen was
#     painted, and the machine left before the operator could read either.  A
#     proof that only looked at the pixels would have called that a pass.
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

if [[ "${HFOC_INSTALL_NOTICE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALL_NOTICE_TEST_TIMEOUT:-300}s" \
        env HFOC_INSTALL_NOTICE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-whole-disk.iso}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
TARGET_SERIAL=${TARGET_SERIAL:-QM00001}
QEMU_MEMORY=${QEMU_MEMORY:-3072}
ACCEPTANCE_BYTES=${ACCEPTANCE_BYTES:-8}
DISK_BYTES=$((ACCEPTANCE_BYTES * 1024 * 1024 * 1024))
# The record promises whole seconds; a machine that leaves inside this slack of
# the promise is a machine that kept it.
NOTICE_SLACK_SECONDS=${NOTICE_SLACK_SECONDS:-1}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Install-notice test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "install-notice test needs the declared medium: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/target.raw
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
SCREENSHOT=$TEST_DIR/screen.ppm
ARTIFACTS=$ROOT/build/test-artifacts/install-notice-$(date -u +%Y%m%dT%H%M%SZ)-$$
QEMU_PID=
FAILURE_MESSAGE=

# A failing run leaves the frame it judged, the serial stream and the QEMU log
# behind: "the screen was not painted" is only diagnosable with the screen.
keep_artifacts() {
    mkdir -p "$ARTIFACTS"
    for file in "$SCREENSHOT" "$SERIAL_LOG" "$QEMU_LOG"; do
        [[ -f "$file" ]] && cp "$file" "$ARTIFACTS/"
    done
    printf 'install notice test artifacts: %s\n' "$ARTIFACTS" >&2
}

cleanup() {
    local status=$?

    trap - EXIT INT TERM
    set +e
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null
    fi
    if (( status != 0 )); then
        printf 'install notice: %s\n' \
            "${FAILURE_MESSAGE:-the run did not show the banner}" >&2
        keep_artifacts
        [[ -f "$SERIAL_LOG" ]] && tail -n 20 "$SERIAL_LOG" >&2
        [[ -f "$QEMU_LOG" ]] && tail -n 5 "$QEMU_LOG" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

truncate -s "$DISK_BYTES" "$DISK"
cp "$OVMF_VARS" "$TEST_DIR/vars.fd"

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$TEST_DIR/vars.fd" \
    -drive "if=none,id=target,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device "ide-hd,drive=target,bus=ide.0,serial=$TARGET_SERIAL" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -serial "file:$SERIAL_LOG" -no-reboot >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

if ! output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$SCREENSHOT" \
    "$QEMU_PID" "$NOTICE_SLACK_SECONDS" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import hashlib
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
screenshot = pathlib.Path(sys.argv[3])
qemu_pid = int(sys.argv[4])
slack_seconds = float(sys.argv[5])

deadline = time.monotonic() + 240.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

completion = re.compile(
    r"\[PRODUCTION-INSTALL\] complete disk=\d+ identity=\S+ "
    r"medium=REMOVE reset=\S+ in=(\d+)s screen=(\w+)")


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def machine_alive():
    # A reaped or already-dead machine is not a running one: a zombie would
    # keep /proc/<pid> in place until bash collects it.
    stat = pathlib.Path(f"/proc/{qemu_pid}/stat")
    try:
        text = stat.read_text()
    except OSError:
        return False
    fields = text.rsplit(")", 1)[-1].split()
    return bool(fields) and fields[0] != "Z"


match = None
while time.monotonic() < deadline:
    text = serial_text()
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic before the install completed")
    found = completion.search(text)
    if found is not None:
        match = found
        break
    time.sleep(0.02)
else:
    # The install did not finish inside this proof's window: that is a marker
    # timeout - the guest may be healthy and merely starved - so it declares the
    # class the retry rule reads, and never an assertion.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT '
        'marker="install completion record" '
        'deadline=240s\n')
    sys.stderr.flush()
    raise SystemExit(
        "the medium never published a completion record; the log ends at: " +
        serial_text()[-600:])

promised = int(match.group(1))
if match.group(2) != "BANNER":
    raise SystemExit(
        "the completion record does not state that the screen was painted: " +
        match.group(0))
# The promise starts when the operator can start reading it.
promised_at = time.monotonic()

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.02)


def dump():
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(2.0)
        monitor.connect(str(socket_path))
        received = b""
        while b"(qemu)" not in received:
            chunk = monitor.recv(4096)
            if not chunk:
                raise ConnectionError("QEMU monitor closed")
            received += chunk
        monitor.sendall(("screendump " + str(screenshot) + "\n").encode())
        received = b""
        while b"(qemu)" not in received:
            chunk = monitor.recv(4096)
            if not chunk:
                raise ConnectionError("QEMU monitor closed")
            received += chunk


# The kernel writes its 32-bit colour straight into the framebuffer, so the
# byte order the display ends up with is the display's, not the kernel's: both
# orders are the colour this proof is looking for.
def orders(colour):
    red, green, blue = colour
    return (bytes((red, green, blue)), bytes((blue, green, red)))


banner_background = orders((0x10, 0x18, 0x20))
countdown = orders((0xFF, 0xD0, 0x70))


def read_frame():
    data = screenshot.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit("the screendump is not a binary PPM")
    fields = data.split(b"\n", 3)
    if len(fields) < 4:
        raise SystemExit("the screendump header is short")
    width, height = (int(value) for value in fields[1].split())
    pixels = fields[3]
    if width <= 0 or height <= 0 or len(pixels) < width * height * 3:
        raise SystemExit("the screendump has no pixels")
    return width, height, pixels


frames = 0
banner_seen = None
painted = None
countdown_frames = set()
last_reason = "the screen was never read"
# The banner is painted after the record is published, so the frame is read
# until it is there: the proof looks at the display the moment it is drawn
# instead of racing the paint with a single dump.  The window is bounded by the
# promise itself - a machine that never paints leaves this loop alive.
while True:
    if not machine_alive():
        last_reason = ("the machine left "
                       f"{time.monotonic() - promised_at:.3f}s after the "
                       f"record, before a painted frame was read")
        break
    if time.monotonic() - promised_at > promised + 30.0:
        last_reason = "the banner was never painted while the machine ran"
        break
    try:
        dump()
    except (ConnectionError, FileNotFoundError, OSError) as exc:
        last_reason = f"the display could not be read: {exc}"
        break
    frames += 1
    width, height, pixels = read_frame()
    background_pixels = 0
    countdown_pixels = 0
    # Every fourth pixel, read as three strided channel planes: the banner is a
    # full-screen fill and the countdown is drawn at eight times the base glyph
    # size, so a sampling that cannot miss either of them keeps the proof's own
    # read of the frame well inside the second it has to notice the countdown
    # moving.
    reds = pixels[0::12]
    greens = pixels[1::12]
    blues = pixels[2::12]
    sampled = min(len(reds), len(greens), len(blues))
    for index in range(sampled):
        pixel = bytes((reds[index], greens[index], blues[index]))
        if pixel in banner_background:
            background_pixels += 1
        elif pixel in countdown:
            countdown_pixels += 1
    if countdown_pixels != 0:
        # The band the countdown is written in: the middle of the screen, where
        # the number is drawn.  Its content has to change while the machine
        # waits - that is what makes the picture a countdown.
        band = []
        for row in range(height // 2, min(height, height // 2 + 80)):
            start = (row * width + width // 2 - 64) * 3
            band.append(pixels[start:start + 128 * 3])
        countdown_frames.add(hashlib.sha256(b"".join(band)).hexdigest())
    if background_pixels * 2 >= sampled:
        if painted is None:
            painted = time.monotonic() - promised_at
            banner_seen = (width, height, background_pixels, countdown_pixels)
        if len(countdown_frames) >= 2:
            break
    else:
        last_reason = ("the screen was read but is not the banner "
                       f"({background_pixels} of {sampled} sampled pixels)")
    time.sleep(0.1)

if painted is None:
    raise SystemExit(f"the banner was never on the screen: {last_reason}")

width, height, background_pixels, countdown_pixels = banner_seen
if countdown_pixels == 0:
    raise SystemExit("the banner carries no countdown")
if len(countdown_frames) < 2:
    raise SystemExit(
        "the countdown did not change while the machine waited: the screen "
        f"carries one fixed picture ({len(countdown_frames)} frame content, "
        f"{frames} frames read)")
if "[PRODUCTION-INSTALL] notice screen=NONE" in serial_text():
    raise SystemExit("the flow published a notice it could not paint")

while machine_alive() and time.monotonic() - promised_at < promised + 60.0:
    time.sleep(0.05)
if machine_alive():
    raise SystemExit(
        f"the machine is still running {promised + 60}s after the record")
waited = time.monotonic() - promised_at
print(f"install notice: banner on {width}x{height} screen with "
      f"{countdown_pixels} countdown pixels, painted at +{painted:.3f}s, "
      f"{len(countdown_frames)} countdown frames, machine left at "
      f"+{waited:.3f}s")
if waited + slack_seconds < promised:
    raise SystemExit(
        f"the record promised {promised}s before the machine restarts and the "
        f"machine left after {waited:.3f}s: the operator cannot read it")
PY
); then
    FAILURE_MESSAGE="$output"
    printf '%s\n' "$output" >&2
    exit 1
fi
printf '%s\n' "$output"

# The machine resets itself after the countdown (ITEM-411), and QEMU runs with
# -no-reboot, so the run ends by itself.
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "QEMU install notice test: OK"
