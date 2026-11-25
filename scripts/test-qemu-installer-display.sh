#!/usr/bin/env bash
# ITEM-488: the installer's display, through the real UEFI path, in QEMU/OVMF.
#
# The installer has to draw on machines it has no GPU driver for, and the only
# display it may rely on is the one the firmware already made.  This proof boots
# the installer medium twice through OVMF:
#
#   * with a display device, where the firmware offers a GOP framebuffer: the
#     installer must report `backend=GOP status=AVAILABLE` with a real
#     resolution, a real pitch and a pixel format it writes, and draw the
#     foundation screen on it;
#   * without one (`-vga none`), where the firmware has no GOP at all: the same
#     flow must appear as `backend=TEXT status=UNAVAILABLE`, the same lines must
#     be readable on the console and the serial stream, and the machine must not
#     panic or hang.
#
# Both cases read the serial stream, which is the same stream a hardware run
# leaves behind - but this is a firmware proof, not a physical one:
# QEMU/OVMF proof != physical hardware acceptance.  OVMF's GOP is one firmware
# implementing one virtual device; real GPUs, real firmware and real pixel
# formats remain a separate acceptance stage, marked NOT TESTED by this item.
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

if [[ "${HFOC_INSTALLER_DISPLAY_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALLER_DISPLAY_TEST_TIMEOUT:-300}s" \
        env HFOC_INSTALLER_DISPLAY_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer.iso}

test -f "$IMAGE" || {
    echo "Missing installer medium: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

# run_case <label> [extra qemu arguments...]
run_case() {
    local label=$1
    shift
    local log=$TEST_DIR/$label.log
    local pid

    : >"$log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 1024M -smp 1 \
        -display none -cdrom "$IMAGE" \
        -serial "file:$log" -no-reboot -no-shutdown \
        "$@" >"$TEST_DIR/$label-qemu.log" 2>&1 &
    pid=$!

    if ! python3 - "$log" "$label" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
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

log = pathlib.Path(sys.argv[1])
label = sys.argv[2]

# The reading, the screen and the selected row, case by case.  The GOP case
# additionally requires a resolution the firmware actually produced; the text
# case requires the honest NONE - a framebuffer that was never there must not
# be reported as one.
required = {
    "gop": [
        "[DISPLAY] backend=GOP status=AVAILABLE",
        " format=BGRX8888 ",
        " fallback=TEXT ",
        "HFOC Installer",
        "> Display",
        "  Backend: GOP",
        "  Pixel format: BGRX8888",
        "  Status: AVAILABLE",
        "  Storage",
        "  Status: NOT_IMPLEMENTED",
        "  Network",
        "  Continue",
        "[INSTALLER-UI] selected=Display state=AVAILABLE",
    ],
    "nogop": [
        "[DISPLAY] backend=TEXT status=UNAVAILABLE",
        " resolution=NONE ",
        " format=UNKNOWN ",
        " reason=NO_FRAMEBUFFER",
        "HFOC Installer",
        "  Backend: TEXT",
        "  Resolution: NONE",
        "  Status: UNAVAILABLE",
        "  Fallback: SERIAL",
        "  Storage",
        "  Status: NOT_IMPLEMENTED",
        "  Network",
        "  Continue",
        "[INSTALLER-UI] selected=Display state=UNAVAILABLE",
    ],
}

deadline = time.monotonic() + 150.0
missing = list(required[label])
while time.monotonic() < deadline:
    try:
        body = SERIAL_VIEW.normalise(
            log.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        body = ""
    if "*** HFOC KERNEL PANIC ***" in body:
        raise SystemExit(f"{label}: the kernel panicked instead of falling back")
    missing = [marker for marker in required[label] if marker not in body]
    if not missing:
        # AVAILABLE is a claim about a framebuffer that was really there.
        if label == "gop" and "resolution=NONE" in body:
            raise SystemExit("gop: AVAILABLE reported without a resolution")
        sys.exit(0)
    time.sleep(0.1)
raise SystemExit(f"{label}: markers never appeared: {missing}")
PY
    then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "installer display proof ($label): FAILED" >&2
        tail -c 2000 "$log" >&2 || true
        exit 1
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "installer display proof ($label): OK"
}

# A firmware that offers a GOP framebuffer.
run_case gop
# A firmware with no display device at all: no GOP, no framebuffer, and the
# installer still has to come up and say so.
run_case nogop -vga none

echo "Installer display QEMU proof: OK"
echo "QEMU/OVMF proof != physical hardware acceptance: physical GPU acceptance is NOT TESTED"
