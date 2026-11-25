#!/usr/bin/env bash
# ITEM-448: the mouse works next to the keyboard, and a click runs the menu.
#
# The owner's contract has two halves that a host proof cannot carry: the two
# HID devices must be bound on one controller at the same time - the second no
# longer displacing the first, which is what the 2026-09-17 hardware run showed
# when the Logitech receiver took the keyboard's place - and a click on a menu
# entry has to run that entry through the same path the keyboard's ENTER uses.
#
# This proof boots the production image on a machine with no PS/2 controller
# and no built-in USB, puts QEMU's usb-kbd and usb-mouse on the *same* emulated
# xHCI controller, and then drives both:
#
#   * the keyboard types `menu`, which opens the machine's own menu and proves
#     the keyboard still works while the pointer is bound;
#   * the monitor's mouse_move/mouse_button commands move the pointer and click
#     the Status entry - the topmost one, so stepping down from above can only
#     reach it first - and the console must publish `[POINTER] click
#     entry=status action=RUN` and then `[LOCAL-MENU] run action=status`;
#   * the keyboard types `help` afterwards, so the keyboard is proven to have
#     survived the pointer's traffic.
#
# What this cannot prove: the real receiver's own report descriptor, a
# low-speed mouse, and the acceptance host's AMD controller semantics. Those
# need the hardware run; this proof is a QEMU proof and says so.
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

if [[ "${HFOC_USB_MOUSE_MENU_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_USB_MOUSE_MENU_TIMEOUT:-180s}" \
        env HFOC_USB_MOUSE_MENU_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "$QEMU_MACHINE" = q35 || {
    echo "The USB mouse menu proof must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "$QEMU_SMP" = 1 || {
    echo "The USB mouse menu proof must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Missing production image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)

# ITEM-527: the console prints one character at a time and an interrupt-context
# diagnostic can land inside a record it is printing, so a shell assertion that
# greps the raw serial file can miss a record the proof's python half saw.  The
# assertions grep the diagnostics-stripped stream, rendered into a file so a
# reader that stops early cannot SIGPIPE the renderer (ITEM-437).  The rule
# lives in scripts/hfoc-serial-view.py - the same rule the embedded python
# helpers carry - so the two halves of the proof cannot disagree.
serial_view() {
    local view=$1.view

    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"
    printf '%s\n' "$view"
}

trap 'rm -rf "$TEST_DIR"' EXIT
SERIAL_LOG=$TEST_DIR/usb-mouse-menu.log
MONITOR_SOCKET=$TEST_DIR/usb-mouse-menu.monitor
QEMU_LOG=$TEST_DIR/qemu.log
QEMU_PID=

cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap 'cleanup; rm -rf "$TEST_DIR"' EXIT

: >"$SERIAL_LOG"
: >"$QEMU_LOG"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -device 'qemu-xhci,id=xhci' \
    -device 'usb-kbd,bus=xhci.0' \
    -device 'usb-mouse,bus=xhci.0' \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
deadline = time.monotonic() + 120.0


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


def wait_for(predicate, what):
    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("kernel panic while waiting for " + what)
        if predicate(text):
            return text
        time.sleep(0.05)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=120s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


# Both HID roles must be bound on the one controller, each with its own record.
wait_for(
    lambda text: "[MOUSE] state=READY reason=OK" in text
    and "[KBD ] state=READY" in text
    and "input_source=USB_HID" in text
    and "hfoc> " in text,
    "the keyboard and the mouse readiness records",
)
text = serial_text()
for marker in (
    "[KBD ] state=READY",
    "[MOUSE] state=READY reason=OK",
    "hid=BOOT_KEYBOARD",
    "hid=REPORT_MOUSE",
    "mode=REPORT",
    "LOCAL INPUT: ENABLED (USB HID KEYBOARD)",
):
    if marker not in text:
        raise SystemExit("the boot did not publish: " + marker)
# ITEM-639: QEMU's usb-mouse declares a wheel, so the driver must bind it in
# report protocol.  A boot record here would mean the optional wheel probe was
# skipped; the click below still proves the pointer reaches the same menu path.
if "hid=BOOT_MOUSE" in text:
    raise SystemExit(
        "the wheel-capable pointer stayed in boot protocol: hid=BOOT_MOUSE")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    monitor.connect(str(socket_path))

    def receive_prompt():
        chunks = []
        while True:
            chunk = monitor.recv(4096)
            if not chunk:
                raise SystemExit("QEMU monitor closed unexpectedly")
            chunks.append(chunk)
            if b"(qemu)" in b"".join(chunks):
                return

    def command(line):
        monitor.sendall((line + "\n").encode("ascii"))
        receive_prompt()
        time.sleep(0.15)

    def send_key(key):
        command("sendkey " + key)

    def type_text(value):
        for character in value:
            send_key(character)
        send_key("ret")

    receive_prompt()

    # The keyboard opens the menu while the pointer is bound next to it.
    type_text("menu")
    wait_for(lambda text: "[LOCAL-MENU] open" in text, "the menu to open")
    geometry = wait_for(
        lambda text: "[POINTER] menu targets=" in text,
        "the pointer's menu geometry record",
    )

    match = re.search(
        r"\[POINTER\] menu targets=(\d+) x=(\d+) y=(\d+) line=(\d+) "
        r"width=(\d+)",
        geometry,
    )
    if match is None:
        raise SystemExit("the pointer geometry record is malformed")
    targets, entry_x, entry_y, line_height, entry_width = (
        int(value) for value in match.groups()
    )
    if targets != 6 or entry_width <= 0 or line_height <= 0:
        raise SystemExit("the pointer geometry record is not usable")

    def latest_geometry():
        """The newest geometry the console published: the rows move up with
        every record the boot writes, and each move is republished."""
        found = re.findall(
            r"\[POINTER\] menu targets=(\d+) x=(\d+) y=(\d+) line=(\d+) "
            r"width=(\d+)",
            serial_text(),
        )
        if not found:
            return None
        return tuple(int(value) for value in found[-1])

    def move(dx, dy):
        if dx or dy:
            command("mouse_move %d %d" % (dx, dy))

    def walk(dx, dy):
        """The emulated mouse is a relative device whose report carries one
        signed byte per axis, so a movement is made of steps that arrive
        whole."""
        while dx or dy:
            step_x = max(-127, min(127, dx))
            step_y = max(-127, min(127, dy))
            move(step_x, step_y)
            dx -= step_x
            dy -= step_y

    def walk_slow(dx, dy):
        """Move without acceleration: a delta below the acceleration floor is
        multiplied by the console scale alone, so small device steps land on
        the requested pixel.  Large accelerated flicks would overshoot the
        first menu row on the 3x 1080p cell."""
        while dx or dy:
            step_x = max(-7, min(7, dx))
            step_y = max(-7, min(7, dy))
            move(step_x, step_y)
            dx -= step_x
            dy -= step_y

    def aim(geometry):
        """Put the pointer in the middle of the first entry's own row."""
        targets, x, y, line, width = geometry
        return x + width // 2, y + line // 2

    # The pointer starts in the middle of the screen.  It is walked into the
    # top left corner first - every step of that walk is clamped there, so the
    # corner is a known place even without a reading - and then stepped to the
    # first entry's row, which is the topmost one a click can reach.
    for _ in range(12):
        move(-127, -127)
    geometry = latest_geometry()
    if geometry is None or geometry[3] < 9:
        raise SystemExit("the pointer geometry has no usable line height")
    pointer_scale = geometry[3] // 8
    entry_x, entry_y = aim(geometry)
    # The corner is (0, 0); the remaining move is a sequence of acceleration-
    # free device steps, each worth `pointer_scale` pixels.
    walk_slow(entry_x // pointer_scale, entry_y // pointer_scale)

    def last_click():
        clicks = re.findall(r"\[POINTER\] click ([^\r\n]*)", serial_text())
        return clicks[-1] if clicks else ""

    def click_count():
        return len(re.findall(r"\[POINTER\] click ", serial_text()))

    hit = ""
    for _ in range(6):
        # The click's own record scrolls the log on, and the console republishes
        # the moved geometry after it, so the wait counts click records instead
        # of reading the tail of the log.
        before = click_count()
        command("mouse_button 1")
        command("mouse_button 0")
        wait_for(
            lambda text: click_count() > before,
            "the click record",
        )
        recorded = last_click()
        if "entry=status action=RUN" in recorded:
            hit = recorded
            break
        # A miss is published with the position it landed at, so the aim is
        # corrected against the console's own reading instead of guessed - and
        # the geometry is re-read, because the rows move up with the records.
        position = re.search(r" x=(\d+) y=(\d+)", recorded)
        if position is None:
            raise SystemExit("the click record carries no position: " + recorded)
        if "entry=NONE action=NONE" not in recorded:
            raise SystemExit(
                "the click landed on an entry that is not Status: " + recorded)
        target_x, target_y = aim(latest_geometry())
        walk(target_x - int(position.group(1)),
             target_y - int(position.group(2)))
    else:
        raise SystemExit("no click reached the menu's Status entry")

    wait_for(
        lambda text: "[LOCAL-MENU] run action=status" in text,
        "the clicked entry to run",
    )
    if "moves=" not in hit or "moves=0" in hit:
        raise SystemExit("the click record does not name a movement: " + hit)

    # The keyboard must still answer after the pointer's traffic.  The menu is
    # still open - running Status does not leave it, exactly as ENTER does not
    # - so it is left with `q` first; typing into an open menu would run its
    # letter shortcuts instead of reaching the line editor (`p` is Shut down).
    send_key("q")
    wait_for(
        lambda text: "[LOCAL-MENU] closed" in text,
        "the menu to close before the keyboard proof",
    )
    type_text("help")
    wait_for(
        lambda text: "COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD" in text,
        "the command typed through the keyboard after the click",
    )
    type_text("kbd")
    wait_for(
        lambda text: "LOCAL INPUT: SOURCE=USB_HID USB=READY" in text
        and re.search(r"KEYS=[1-9][0-9]*", text) is not None,
        "the keyboard's own counters after the click",
    )
    print("USB mouse menu click: " + hit)
PY

if ! python3 - "$MONITOR_SOCKET" <<'PY'
import socket
import sys
import time

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    monitor.connect(sys.argv[1])
    deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise SystemExit("QEMU monitor prompt timeout")
        monitor.settimeout(remaining)
        try:
            prompt += monitor.recv(4096)
        except socket.timeout:
            continue
    monitor.sendall(b"quit\n")
finally:
    monitor.close()
PY
then
    echo "usb_mouse_menu: QEMU monitor quit failed" >&2
    exit 1
fi
wait "$QEMU_PID"
QEMU_PID=

if grep -Fq "*** HFOC KERNEL PANIC ***" "$(serial_view "$SERIAL_LOG")"; then
    echo "usb_mouse_menu: the boot panicked" >&2
    exit 1
fi
if grep -Fq "LOCAL INPUT: DISABLED" "$(serial_view "$SERIAL_LOG")"; then
    echo "usb_mouse_menu: the console still reports disabled local input" >&2
    exit 1
fi

echo "USB keyboard and mouse on one xHCI controller, click runs the menu: PASS"
grep -F "[MOUSE] state=READY reason=OK" "$(serial_view "$SERIAL_LOG")" | tail -1
grep -F "[LOCAL-MENU] run action=status" "$(serial_view "$SERIAL_LOG")" | tail -1
