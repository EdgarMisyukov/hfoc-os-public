#!/usr/bin/env bash
# ITEM-452: the driver walks every xHCI controller, and a keyboard and a mouse
# on two different controllers work at the same time.
#
# The owner's machine has three xHCI controllers.  The 2026-09-19 hardware run
# published exactly one `[XHCI]` record - the first controller in PCI order,
# the one with the K120 keyboard - because the machine loop left on the first
# bound device (`if (scan.selected) break;`).  The Logitech receiver
# `046d:c53f` on a USB3 port of a *different* controller was never enumerated,
# and `[MOUSE] ... seen=0` was the whole story: plugged in, and invisible.
#
# This proof boots the production image twice on a machine with two xHCI
# controllers at fixed PCI slots (`00:02.0` and `00:02.1`, so the scan order is
# the one the assertions name, exactly as the ITEM-406 phase of
# scripts/test-qemu-production-storage-boot.sh pins it):
#
#   * phase A puts QEMU's usb-kbd on the first controller and usb-mouse on the
#     second.  Both `[XHCI]` records must appear with *different* `pci=`
#     addresses and `taken=YES`, each device must publish its own `[USB ]` line
#     with its port, speed, role and identity - the keyboard BOUND on the
#     controller the pointer is not on - the keyboard must open the console
#     menu while the pointer is bound elsewhere, the monitor's mouse_move /
#     mouse_button must click the Status entry and run it, and the keyboard
#     must still answer afterwards;
#   * QEMU's usb-kbd does not answer the once-a-second idle request by itself,
#     so the proof wakes it with modifier-only `sendkey shift` reports while
#     the boot records are awaited and requires `proven=1 retry=0`: the
#     keyboard must prove itself *inside its walk window*, because that is the
#     path the owner's machine takes - the walk continues with the keyboard
#     already bound and still has to reach the controller the pointer is on.
#     Without the wake the first candidate is always silent and the proof would
#     only ever exercise the release-and-re-bind path;
#   * phase B is the mass-storage proof (ITEM-519 increment 2): the first
#     controller carries the keyboard and the second a `usb-storage` stick.
#     The stick must bind (`role=STORAGE state=BOUND`), publish its identity
#     and the capacity it declared on `[USB-STORAGE]`, and the first sector
#     the bulk data stage reads must be the marker this script wrote into
#     the image before QEMU started - so the bytes, not a claim, are what
#     the proof checks.  Its controller publishes `taken=YES`.  The old
#     negative shape (`REFUSED`) now belongs to phase C's `usb-uas`.
#
# ITEM-453 adds the port evidence and the SuperSpeed case:
#
#   * every port of every controller publishes its own `[XHCI-PORT]` line, the
#     walk names every port index 1..ports exactly once, and the controller's
#     Supported Protocol groups partition exactly those ports - the driver
#     labels each port from them, so a gap or an overlap would mislabel the
#     hardware run this proof is for.  A device this driver binds has to be on
#     the USB2 half of its controller (QEMU's usb-kbd/usb-mouse are high-speed
#     devices), which is also the half the owner's receiver is on;
#   * phase C is the SuperSpeed case: QEMU's `usb-uas` carries a USB 3 device
#     descriptor, and on the first physical port of a controller its model
#     presents it on raw port 1, the USB3 half.  The driver has to address it
#     with the 512-byte control pipe USB 3 defines and publish `speed=SUPER`,
#     and the port's raw PORTSC has to carry the SuperSpeed value in its speed
#     field - a SuperSpeed device is not silently refused as UNSUPPORTED_SPEED.
#
# ITEM-545 adds phase E: a SuperSpeed BOT mass-storage device on the USB3 half
# of the controller - QEMU's `usb-bot` with a SCSI disk behind it, the shape of
# the owner's 24a9:205a stick - must be enumerated, bound and read, and the
# SuperSpeed companion descriptor's bMaxBurst must reach the endpoint context
# the driver programs (`burst_in=`/`burst_out=` in the bound record).  Phase B
# already proves the high-speed BOT bind and its bytes; phase C proves a
# SuperSpeed device is addressed, and refuses UAS by name.  Neither of them
# binds a SuperSpeed *BOT* device, which is the one the blue port carries.
#
# What this cannot prove, and says so instead of faking an assertion: QEMU's
# xHCI model always reports PORTSC.PP set, never leaves RxDetect for an empty
# port and ignores writes to PORTSC.PED (see the ITEM-406 note in
# scripts/test-qemu-production-storage-boot.sh), so no phase here exercises the
# power-on and link-kick writes of the bring-up - those are pinned by the host
# proof (make test-xhci-host) and remain a hardware question: whether the
# owner's `05:00.3` needs power, a link kick or simply more time before its
# ports mean anything is what the next hardware run's `[XHCI-PORT]` lines will
# answer.  The low-speed path, the real receiver's descriptors and whether its
# USB3 port trains as FULL or SUPER also need the hardware run; this proof is a
# QEMU proof and does not claim them.
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

if [[ "${HFOC_USB_TWO_CONTROLLERS_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_USB_TWO_CONTROLLERS_TIMEOUT:-420s}" \
        env HFOC_USB_TWO_CONTROLLERS_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "$QEMU_MACHINE" = q35 || {
    echo "The two-controller proof must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "$QEMU_SMP" = 1 || {
    echo "The two-controller proof must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Missing production image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
QEMU_PID=
# ITEM-511: a failing phase keeps its transcript.  The proof drives the console
# through emulated USB keyboards, so a failure here is diagnosed from the serial
# log, not from a 200-second marker timeout with the log deleted by the trap:
# the failing logs are copied under build/test-artifacts, which the CI
# diagnostics step collects, and their tails are printed into the phase log.
save_failure_artifacts() {
    local timestamp artifact_dir log
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/usb-two-controllers-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-the two-controller proof failed}" \
        >"$artifact_dir/FAILURE"
    for log in "$TEST_DIR"/*.log; do
        [[ -f $log ]] || continue
        cp -a "$log" "$artifact_dir/"
        echo "----- ${log##*/} (tail) -----" >&2
        tail -n 30 "$log" >&2 || true
    done
    echo "Two-controller proof artifacts: $artifact_dir" >&2
}
cleanup() {
    local status=$?
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( status != 0 )); then
        save_failure_artifacts || true
    fi
    rm -rf "$TEST_DIR" || true
    exit "$status"
}
trap cleanup EXIT


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

# ---------------------------------------------------------------- phase A ---
# usb-kbd on 00:02.0 and usb-mouse on 00:02.1: the owner's shape, with the
# keyboard and the pointer on different controllers.
SERIAL_A=$TEST_DIR/two-controllers.log
MONITOR_A=$TEST_DIR/two-controllers.monitor
: >"$SERIAL_A"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10' \
    -device 'qemu-xhci,id=x1,bus=pcie.0,addr=0x11' \
    -device 'usb-kbd,bus=x0.0' \
    -device 'usb-mouse,bus=x1.0' \
    -serial "file:$SERIAL_A" \
    -monitor "unix:$MONITOR_A,server,nowait" \
    -no-reboot -no-shutdown >"$TEST_DIR/qemu-a.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_A" "$MONITOR_A" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import threading
import time

sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
deadline = time.monotonic() + 200.0


def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""


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
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


def records(marker):
    return [line for line in serial_text().splitlines()
            if line.startswith(marker)]


while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
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


def type_command(value, probe, what):
    """Type a console command through the shared echo-confirmed helper.

    ITEM-511: every keystroke travels through the guest's local input path, and
    the kernel's bounded queue drops the newest event while the console has not
    drained it.  `type_text` used to inject four characters and Enter
    back-to-back and then wait 200 seconds for a command the guest may never
    have seen; the shared helper (ITEM-427) types the line, reads the console's
    own `\\rhfoc> ` echo as the delivery witness, retypes a line that was never
    echoed from a cleared line, and repeats Enter while the console has not
    answered at all.  The witness is case-insensitive (`witness=str.lower`),
    because this proof's own wake types modifier-only `sendkey shift` reports
    and a dropped modifier release leaves the guest's Shift set: the console
    then echoes `MENU` while its command vocabulary matches case-insensitively.
    `probe` owns the exactness: it raises SystemExit on a definite wrong answer
    and returns a value only for the answer this command must produce, so a
    retry never weakens an assertion.  A budget that runs out is reported as the
    phase's canonical marker-timeout record, never as a bare traceback.
    """
    try:
        console_command.type_line(send_key, serial_bytes, value, deadline,
                                  witness=str.lower)
        console_command.submit(
            lambda: send_key("ret"), serial_bytes,
            lambda suffix: probe(suffix), deadline)
    except console_command.LostKeystrokes as lost:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
        sys.stderr.flush()
        raise SystemExit("timed out typing %r: %s" % (value, lost)) from lost


def menu_probe(text):
    if "UNKNOWN COMMAND" in text or "CONSOLE TRANSLATION MISSING" in text:
        raise SystemExit("the menu command was refused: " + text)
    return text if "[LOCAL-MENU] open" in text else None


def kbd_probe(text):
    if "UNKNOWN COMMAND" in text:
        raise SystemExit("the kbd command was refused: " + text)
    return text if "LOCAL INPUT: SOURCE=USB_HID USB=READY" in text else None


receive_prompt()


# ITEM-452: QEMU's usb-kbd does not answer the once-a-second idle request on its
# own, so without a hand on it the first keyboard candidate is always silent,
# released and re-bound after the walk - and the proof would never exercise the
# path the owner's machine takes: a keyboard that proves itself *during* the
# walk, after which the walk still has to reach the controller the pointer is
# on.  Shift is a modifier-only boot report: it makes the device answer and
# types nothing into the console.  Only while the readiness records are
# awaited; every real monitor command below runs after this thread stopped.
wake_stop = threading.Event()


def wake_keyboard():
    for _ in range(200):
        if wake_stop.is_set():
            return
        try:
            monitor.sendall(b"sendkey shift\n")
        except OSError:
            return
        time.sleep(0.05)


waker = threading.Thread(target=wake_keyboard, daemon=True)
waker.start()

# Every controller the machine has must name itself, and the keyboard and the
# pointer must both be bound with the devices' own records beside them.
text = wait_for(
    lambda text: "[KBD ] state=READY" in text
    and "[MOUSE] state=READY reason=OK" in text
    and "input_source=USB_HID" in text
    and "hfoc> " in text,
    "the keyboard and the pointer readiness records",
)
wake_stop.set()
waker.join(timeout=2.0)
# Drain the prompts the waker queued and leave the modifier released.
command("sendkey shift")

for marker in (
    "[KBD ] state=READY",
    "[MOUSE] state=READY reason=OK",
    "LOCAL INPUT: ENABLED (USB HID KEYBOARD)",
):
    if marker not in text:
        raise SystemExit("the boot did not publish: " + marker)

# ITEM-521 requirement 3: this phase boots the ISO as an optical disc, which
# none of the three placement candidates can write.  The journal must say so
# itself - exactly once, in the ordinary records - and the run must continue:
# every assertion around this one is proof that it did.
#
# ITEM-542: the refusal page prints a section line with the same prefix and its
# counters appended, so the machine's own record is selected by its shape (no
# `pending=`) and a page-shaped line on this appliance boot is named as the
# failure it would be - this boot paints no refusal page.
journal_lines = [
    line for line in text.splitlines() if line.startswith("[INSTALL-LOG] ")]
readonly_logs = [line for line in journal_lines if " pending=" not in line]
page_logs = [line for line in journal_lines if " pending=" in line]
if readonly_logs != [
        "[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY"]:
    raise SystemExit(
        "the optical boot did not name the unwritable journal exactly once: "
        + repr(readonly_logs))
if page_logs:
    raise SystemExit(
        "a refusal-page section line reached the appliance boot: "
        + repr(page_logs))
print("the optical boot names the unwritable install journal: PASS")

# The keyboard proved itself inside its window: the walk continued with the
# keyboard bound and still reached the controller the pointer is on, which is
# the acceptance host's shape.
proved = re.search(
    r"\[KBD \] state=READY .*candidates=(\d+) silent=(\d+) proven=(\d+) "
    r"retry=(\d+)",
    text,
)
if proved is None:
    raise SystemExit("the [KBD ] record does not name how the walk ended")
candidates, silent, proven, retry = (int(value) for value in proved.groups())
if proven < 1 or retry != 0:
    raise SystemExit(
        "the keyboard did not prove itself during the walk "
        "(candidates=%d silent=%d proven=%d retry=%d); the two-controller "
        "proof must exercise the proven path, not only the re-bind" %
        (candidates, silent, proven, retry))

xhci_records = records("[XHCI] state=")
if len(xhci_records) < 2:
    raise SystemExit(
        "the walk published %d [XHCI] records for two controllers:\n%s" %
        (len(xhci_records), "\n".join(xhci_records)))

controller_addresses = []
records_by_controller = {}
declared_ports = {}
for line in xhci_records:
    match = re.search(r" pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) ", line)
    if match is None:
        raise SystemExit("an [XHCI] record names no pci address: " + line)
    if " controllers=2 " not in line:
        raise SystemExit(
            "an [XHCI] record does not name the machine's controller count: "
            + line)
    ports = re.search(r" ports=(\d+) ", line)
    if ports is None:
        raise SystemExit("an [XHCI] record names no port count: " + line)
    address = match.group(1)
    declared_ports[address] = int(ports.group(1))
    if address not in controller_addresses:
        controller_addresses.append(address)
    records_by_controller.setdefault(address, []).append(line)
if len(controller_addresses) != 2:
    raise SystemExit(
        "the two controllers did not publish two different pci addresses: "
        + ", ".join(controller_addresses))
# The controller's *final* record is the one that says whether the machine kept
# it.  A controller whose only keyboard candidate was silent publishes its visit
# as taken=NO and the re-bind publishes taken=YES for the same controller
# afterwards; both records name it, which is the point of the item.
for address, lines in records_by_controller.items():
    if " taken=YES" not in lines[-1]:
        raise SystemExit(
            "controller %s ended without a bound role:\n%s" %
            (address, "\n".join(lines)))

usb_records = records("[USB ] ")
if not usb_records:
    raise SystemExit("no device published a [USB ] record")
bound_devices = {}
for line in usb_records:
    match = re.search(
        r"\[USB \] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) port=(\d+) "
        r"speed=(\w+) role=(\w+) state=BOUND", line)
    if match is None:
        continue
    pci, port, speed, role = match.groups()
    if "vid=0x" not in line or "pid=0x" not in line or "ifaces=" not in line:
        raise SystemExit(
            "a bound device did not publish its identity: " + line)
    if "detail=" in line:
        raise SystemExit(
            "a bound device published a refusal detail beside reason=OK: "
            + line)
    bound_devices[role] = (pci, port, speed, line)
if "KEYBOARD" not in bound_devices or "MOUSE" not in bound_devices:
    raise SystemExit(
        "both roles must be bound and published; the machine published:\n"
        + "\n".join(usb_records))
if bound_devices["KEYBOARD"][0] == bound_devices["MOUSE"][0]:
    raise SystemExit(
        "the keyboard and the pointer were bound on one controller, not on "
        "the two the machine has: " + bound_devices["KEYBOARD"][3])

# ITEM-453: every port of every controller names itself.  The 2026-09-19
# hardware run published two PORTSC values out of the six ports of the
# controller the owner's receiver is attached to, and no record said whether
# the other four had been read at all.  The count, the indices and the raw
# values below are what turn "the driver looked at every port" into a fact of
# the record.
ports_by_controller = {}
for line in records("[XHCI-PORT] "):
    match = re.search(
        r"\[XHCI-PORT\] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) port=(\d+) "
        r"group=(\w+) was=(0x[0-9A-F]{8}) now=(0x[0-9A-F]{8}) pp=(\d) "
        r"ccs=(\d) pls=(\d+) pon=(\d) kick=(\d) wait=(\d+)", line)
    if match is None:
        raise SystemExit("an [XHCI-PORT] record is malformed: " + line)
    pci, port, group, _was, now, pp, ccs, pls, _pon, _kick, _wait = match.groups()
    ports_by_controller.setdefault(pci, {})[int(port)] = (group, now, int(ccs))
for address, declared in declared_ports.items():
    published = ports_by_controller.get(address, {})
    if sorted(published) != list(range(1, declared + 1)):
        raise SystemExit(
            "controller %s declares %d ports but named %s of them; every port "
            "has to publish its own [XHCI-PORT] line" %
            (address, declared, sorted(published)))

# ITEM-453: the controller's Supported Protocol groups must partition its
# ports exactly - the driver labels every port from them, so a gap or an
# overlap would silently mislabel a port on the hardware run this proof is
# for.  QEMU's model declares a USB3 group over the first raw ports and a USB2
# group over the ones after it (hw/usb/hcd-xhci.c), and its usb-kbd/usb-mouse
# are high-speed devices, so a bound device has to be on the USB2 half of the
# controller it is on.
groups_by_controller = {}
declared_groups = {}
for line in records("[XHCI-PROTO] "):
    match = re.search(
        r"\[XHCI-PROTO\] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) index=(\d+) "
        r"groups=(\d+) major=(\d+) minor=(\d+) name=(\S*) off=(\d+) count=(\d+) "
        r"ports=(\d+)-(\d+) psiv=(\S+) d0=(0x[0-9A-F]{8}) "
        r"d1=(0x[0-9A-F]{8}) d2=(0x[0-9A-F]{8})", line)
    if match is None:
        raise SystemExit("an [XHCI-PROTO] record is malformed: " + line)
    pci = match.group(1)
    major, port_offset, count = int(match.group(4)), int(match.group(7)), int(match.group(8))
    first, last = int(match.group(9)), int(match.group(10))
    if first != port_offset or last != port_offset + count - 1:
        raise SystemExit(
            "the protocol record's range does not match its raw offset and "
            "count: " + line)
    declared_groups[pci] = int(match.group(3))
    groups_by_controller.setdefault(pci, []).append((first, last, major))
for address, declared in declared_ports.items():
    declared_count = declared_groups.get(address, 0)
    if declared_count != len(groups_by_controller.get(address, [])):
        raise SystemExit(
            "controller %s declares %d protocol groups but published %d "
            "records" % (address, declared_count,
                         len(groups_by_controller.get(address, []))))
    covered = []
    for first, last, _major in groups_by_controller.get(address, []):
        covered.extend(range(first, last + 1))
    if sorted(covered) != list(range(1, declared + 1)):
        raise SystemExit(
            "the protocol groups of controller %s do not partition its ports "
            "1..%d: %s" % (address, declared, sorted(covered)))
    halves = {major for _first, _last, major in
              groups_by_controller.get(address, [])}
    if 2 not in halves or 3 not in halves:
        raise SystemExit(
            "controller %s did not declare both protocol halves: %s" %
            (address, groups_by_controller.get(address)))
for role, (pci, port, speed, line) in bound_devices.items():
    group, _now, ccs = ports_by_controller[pci][int(port)]
    if ccs != 1:
        raise SystemExit(
            "the port a %s was bound on did not report a connect: %s" %
            (role, line))
    if group != "USB2":
        raise SystemExit(
            "the %s is a %s-speed device and was labelled %s on port %s" %
            (role, speed, group, port))

# The keyboard opens the menu while the pointer is bound on the *other*
# controller: this is the ITEM-452 contract, and the menu record names the
# console path both devices feed.
type_command("menu", menu_probe, "the menu to open")
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
    multiplied by the console scale alone, so small device steps land on the
    requested pixel.  Large accelerated flicks would overshoot the first menu
    row on the 3x 1080p cell."""
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
# The corner is (0, 0); the remaining move is a sequence of acceleration-free
# device steps, each worth `pointer_scale` pixels.
walk_slow(entry_x // pointer_scale, entry_y // pointer_scale)

def last_click():
    clicks = re.findall(r"\[POINTER\] click ([^\r\n]*)", serial_text())
    return clicks[-1] if clicks else ""

def click_count():
    return len(re.findall(r"\[POINTER\] click ", serial_text()))

hit = ""
for _ in range(6):
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

# The keyboard must still answer after the pointer's traffic, which the
# pointer generated on a different controller.
send_key("q")
wait_for(
    lambda text: "[LOCAL-MENU] closed" in text,
    "the menu to close before the keyboard proof",
)
type_command("kbd", kbd_probe, "the keyboard's own counters after the click")
wait_for(
    lambda text: "LOCAL INPUT: SOURCE=USB_HID USB=READY" in text
    and re.search(r"KEYS=[1-9][0-9]*", text) is not None,
    "the keyboard's own counters after the click",
)
print("two controllers: keyboard on %s and pointer on %s, click: %s" % (
    bound_devices["KEYBOARD"][0], bound_devices["MOUSE"][0], hit))
PY

if ! python3 - "$MONITOR_A" <<'PY'
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
    echo "usb_two_controllers: QEMU monitor quit failed" >&2
    exit 1
fi
wait "$QEMU_PID"
QEMU_PID=

if grep -Fq "*** HFOC KERNEL PANIC ***" "$(serial_view "$SERIAL_A")"; then
    echo "usb_two_controllers: the boot panicked" >&2
    exit 1
fi
if grep -Fq "LOCAL INPUT: DISABLED" "$(serial_view "$SERIAL_A")"; then
    echo "usb_two_controllers: the console still reports disabled local input" >&2
    exit 1
fi

echo "Two xHCI controllers: the keyboard and the pointer on different ones: PASS"
grep -F '[XHCI] state=' "$(serial_view "$SERIAL_A")" | tail -2
grep -F '[USB ] ' "$(serial_view "$SERIAL_A")" | grep -F 'state=BOUND' || true

# ---------------------------------------------------------------- phase B ---
# ITEM-519 increment 2: the positive mass-storage proof.  A controller carries
# a device that is not HID at all - QEMU's `usb-storage` stick - and the walk
# must bind it, name it on its own `[USB ]` line and publish its identity, the
# capacity it declared and the first sector the bulk data stage really read on
# `[USB-STORAGE]`.  The device's identifiers are read from the record, never
# hardcoded: QEMU's model ids change between releases.
#
# ITEM-519 increment 3 adds the write half: the console command
# `storage write-proof` is typed into the guest, drives WRITE(10) over the
# bulk OUT path on the stick's last sector, reads the pattern back, restores
# the original sector and reads that back.  The proof requires the
# `state=WRITE_PROOF reason=OK write=OK verify=OK restore=OK` record, and then
# checks the image on the host: the last sector must still be the zeros the
# proof wrote into it before QEMU started, so the restore is witnessed outside
# the kernel that claims it.
#
# ITEM-519 increment 4 requires the stick's block_device view first
# (`state=BLOCK_BOUND` with the geometry the block layer enforces) and drives
# the write proof through that view, so the record the journal will own is the
# same handle whose bytes the proof wrote and restored.
SERIAL_B=$TEST_DIR/non-hid.log
MONITOR_B=$TEST_DIR/non-hid.monitor
# ITEM-519 increment 2: the first sector carries a marker before the guest
# starts, so the record's sector0 field is the proof that the bulk data
# stage read the medium and not the driver's zeroed bounce page.
python3 - "$TEST_DIR/usb-storage.img" <<'PY'
import pathlib
import sys

marker = b"HFOC-USB-STORAGE"
image = bytearray(1 << 20)
image[0:len(marker)] = marker
pathlib.Path(sys.argv[1]).write_bytes(image)
PY
: >"$SERIAL_B"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10' \
    -device 'qemu-xhci,id=x1,bus=pcie.0,addr=0x11' \
    -device 'usb-kbd,bus=x0.0' \
    -drive "if=none,id=usbstick,file=$TEST_DIR/usb-storage.img,format=raw,cache=writeback,file.locking=off" \
    -device 'usb-storage,drive=usbstick,bus=x1.0' \
    -serial "file:$SERIAL_B" \
    -monitor "unix:$MONITOR_B,server,nowait" \
    -no-reboot -no-shutdown >"$TEST_DIR/qemu-b.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_B" "$MONITOR_B" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
deadline = time.monotonic() + 200.0


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
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


text = wait_for(
    lambda text: "[KBD ] state=READY" in text and "hfoc> " in text,
    "the keyboard on the first controller and the prompt",
)

usb_records = [line for line in text.splitlines() if line.startswith("[USB ] ")]
if not usb_records:
    raise SystemExit(
        "the mass-storage device disappeared: no [USB ] line was published")

# ITEM-519 increment 2: the stick on the second controller is bound, and its
# identity, the capacity it declared and the first sector the bulk path really
# read are published.  The sector's first four bytes are the marker the proof
# wrote into the image before QEMU started, so the data stage is shown to have
# carried the medium's bytes.
storage = [line for line in usb_records
           if " role=STORAGE " in line and " state=BOUND" in line]
if not storage:
    raise SystemExit(
        "the mass-storage device was not bound:\n" + "\n".join(usb_records))
line = storage[-1]
match = re.search(
    r"\[USB \] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) port=(\d+) "
    r"speed=(\w+) role=STORAGE state=BOUND reason=(\w+)", line)
if match is None:
    raise SystemExit("the bound storage record is malformed: " + line)
pci = match.group(1)

storage_records = [line for line in text.splitlines()
                   if line.startswith("[USB-STORAGE] ")]
bound = [line for line in storage_records
         if " state=BOUND" in line and " pci=%s " % pci in line]
if not bound:
    raise SystemExit(
        "no [USB-STORAGE] BOUND record:\n" + "\n".join(storage_records))
line = bound[-1]
if not re.search(r" vid=0x[0-9A-F]+ pid=0x[0-9A-F]+", line):
    raise SystemExit("the storage record names no identity: " + line)
if " blocks=2048" not in line or " block=512" not in line:
    raise SystemExit("the storage record carries no capacity: " + line)
if " sector0=434F4648" not in line:
    raise SystemExit(
        "the storage record does not carry the first sector's bytes: " + line)

# ITEM-519 increment 4: the same stick got a block_device view, with the
# geometry the block layer will enforce.  The write proof below drives the
# medium through exactly this view, so the record is the handle the journal
# will own and the proof is the data that the handle works.
block_records = [line for line in storage_records
                 if " state=BLOCK_BOUND" in line and " pci=%s " % pci in line]
if not block_records:
    raise SystemExit(
        "no [USB-STORAGE] BLOCK_BOUND record:\n"
        + "\n".join(storage_records))
line = block_records[-1]
if " block=512" not in line or " blocks=2048" not in line:
    raise SystemExit("the block view carries no capacity: " + line)
print("mass storage block view: " + line)

xhci_records = [line for line in text.splitlines()
                if line.startswith("[XHCI] state=")]
controller = [line for line in xhci_records if " pci=%s " % pci in line]
if not controller:
    raise SystemExit(
        "the storage controller published no [XHCI] record: " + pci)
if " taken=YES" not in controller[-1]:
    raise SystemExit(
        "a controller with a bound storage device did not publish taken=YES: "
        + controller[-1])
print("mass storage bound: " + line)

# ITEM-519 increment 3: the write half, typed into the guest's own console.
# The line travels the same local-input path every operator keystroke takes;
# the echo-confirmed helper retypes a line the guest never saw, so a starved
# guest cannot turn a lost keystroke into a failed assertion.
while not monitor_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
monitor.settimeout(2.0)
monitor.connect(str(monitor_path))


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
    # The console command this phase types carries a space and a hyphen, and
    # HMP `sendkey` names those keys `spc` and `minus` rather than spelling the
    # character; a key sent by the wrong name never reaches the guest.
    key_names = {" ": "spc", "-": "minus"}
    command("sendkey " + key_names.get(key, key))


def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""


def proof_probe(text):
    records = [line for line in text.splitlines()
               if line.startswith("[USB-STORAGE] ")
               and " state=WRITE_PROOF " in line]
    for record in records:
        if " reason=OK " in record and \
                " write=OK verify=OK restore=OK" in record:
            return record
        raise SystemExit("the write proof refused: " + record)
    return None


try:
    console_command.type_line(send_key, serial_bytes, "storage write-proof",
                              deadline, witness=str.lower)
    proof = console_command.submit(
        lambda: send_key("ret"), serial_bytes, proof_probe, deadline)
except console_command.LostKeystrokes as lost:
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="the storage write proof" '
        'deadline=200s\n')
    sys.stderr.flush()
    raise SystemExit("timed out typing the storage write proof: %s" % lost)
print("mass storage write proof: " + proof)

# ITEM-519 increment 5: the boot-medium guard must never call this attached
# stick the boot medium - the machine booted from the CD, and the stick is a
# plain zeroed image with a marker.  The record still has to be published and
# name the fact that decided it; a silent probe, or one that says MATCH, is
# exactly the failure the guard exists to prevent.  ITEM-542: the machine's
# record is the one that carries the probe's own fields (`pci=`), so a refusal
# page's bare verdict line can never be mistaken for it.
medium = [line for line in serial_text().splitlines()
          if line.startswith("[INSTALL-MEDIUM] ") and " pci=" in line]
if not medium:
    raise SystemExit("no [INSTALL-MEDIUM] record was published")
medium_line = medium[-1]
if " state=MATCH" in medium_line:
    raise SystemExit(
        "the attached stick was claimed as the boot medium: " + medium_line)
if " reason=" not in medium_line:
    raise SystemExit("the medium record names no reason: " + medium_line)
print("attached stick is not the boot medium: " + medium_line)
PY

if ! python3 - "$SERIAL_B" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import sys


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
text = SERIAL_VIEW.normalise(
    pathlib.Path(sys.argv[1]).read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("the boot panicked")
PY
then
    echo "usb_two_controllers: the mass-storage phase panicked" >&2
    exit 1
fi

kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# ITEM-519 increment 3: the host's own view of the restore.  The image was
# created as zeros with the marker in sector 0, so after the write proof its
# last sector (LBA 2047 of 2048) must still be zeros: the pattern the kernel
# wrote over it and read back is not on the medium the host can see, and the
# marker sector is untouched.
python3 - "$TEST_DIR/usb-storage.img" <<'PY'
import pathlib
import sys

image = pathlib.Path(sys.argv[1]).read_bytes()
if len(image) != 1 << 20:
    raise SystemExit("the stick image changed size: %d" % len(image))
if image[0:16] != b"HFOC-USB-STORAGE":
    raise SystemExit("the marker sector was overwritten: " + repr(image[0:16]))
if image[-512:] != bytes(512):
    raise SystemExit("the write proof did not restore the last sector")
print("the host sees the last sector restored: "
      + image[-512:][0:8].hex())
PY

if grep -Fq "LOCAL INPUT: DISABLED" "$(serial_view "$SERIAL_B")"; then
    echo "usb_two_controllers: the console still reports disabled local input" >&2
    exit 1
fi

echo "USB mass storage binds on the second controller, publishes its block view, reads sector 0 and restores a written sector through it: PASS"
grep -F '[USB ] ' "$(serial_view "$SERIAL_B")" | grep -F 'role=STORAGE' | tail -1 || true
grep -F '[USB-STORAGE] ' "$(serial_view "$SERIAL_B")" | tail -3 || true
grep -F '[XHCI] state=' "$(serial_view "$SERIAL_B")" | tail -2 || true

# ---------------------------------------------------------------- phase C ---
# ITEM-453: a SuperSpeed device on a USB3 port.  QEMU's xHCI bus names physical
# ports, and the device's speed decides which logical half of the controller it
# appears on: a SUPER device on the first physical port lands on raw port 1, the
# USB3 half.  `usb-uas` is QEMU's USB 3 device model (dev-uas.c carries the
# SuperSpeed device descriptor), and it is not HID - so both roles refuse it,
# and the proof is that the refusal *names it*: the speed the port reported, the
# port's own raw PORTSC, and the identity the driver read over the 512-byte
# control pipe USB 3 defines.
SERIAL_C=$TEST_DIR/superspeed.log
: >"$SERIAL_C"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10' \
    -device 'usb-uas,bus=x0.0' \
    -serial "file:$SERIAL_C" \
    -no-reboot -no-shutdown >"$TEST_DIR/qemu-c.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_C" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
deadline = time.monotonic() + 200.0

XHCI_SPEED_SUPER = 4


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
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


def records(marker):
    return [line for line in serial_text().splitlines()
            if line.startswith(marker)]


text = wait_for(
    lambda text: "[XHCI-PORT] " in text and "hfoc> " in text,
    "the port records and the prompt",
)

usb_records = records("[USB ] ")
if not usb_records:
    raise SystemExit(
        "the SuperSpeed device disappeared: no [USB ] line was published")

# The SuperSpeed device must be published as SUPER.  A device the driver could
# not address at all is exactly the failure ITEM-438 removed, and this is the
# QEMU case that keeps it removed.
super_records = [line for line in usb_records if " speed=SUPER " in line]
if not super_records:
    raise SystemExit(
        "no device was published at SuperSpeed:\n" + "\n".join(usb_records))
line = super_records[-1]
match = re.search(
    r"\[USB \] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) port=(\d+) "
    r"speed=SUPER role=(\w+) state=(\w+) reason=(\w+)", line)
if match is None:
    raise SystemExit("the SuperSpeed record is malformed: " + line)
if "vid=0x" not in line or "pid=0x" not in line or "ifaces=" not in line:
    raise SystemExit(
        "the SuperSpeed device published no identity over its control pipe: "
        + line)
pci, port = match.group(1), int(match.group(2))
# ITEM-519 increment 2: the stick on phase B binds now, so this is the proof's
# negative shape - a mass-storage device this driver cannot drive (UAS) must
# still be published as REFUSED with its identity, not silently skipped.
if " state=REFUSED " not in line:
    raise SystemExit(
        "the SuperSpeed non-storage device was not published as REFUSED: "
        + line)

# Its port's own record has to agree: the raw PORTSC the port answered must
# carry the SuperSpeed value in bits 13:10 (xHCI 1.1 table 5-27), and the port
# must be on the controller's USB3 half - which is what the protocol groups
# below declare it to be.
port_record = None
for candidate in records("[XHCI-PORT] "):
    if " pci=%s port=%d " % (pci, port) in candidate:
        port_record = candidate
if port_record is None:
    raise SystemExit("the SuperSpeed port published no [XHCI-PORT] line")
match = re.search(
    r"\[XHCI-PORT\] pci=\S+ port=(\d+) group=(\w+) was=(0x[0-9A-F]{8}) "
    r"now=(0x[0-9A-F]{8}) pp=(\d) ccs=(\d) pls=(\d+) pon=(\d) kick=(\d) "
    r"wait=(\d+)", port_record)
if match is None:
    raise SystemExit("the SuperSpeed port record is malformed: " + port_record)
if match.group(2) != "USB3":
    raise SystemExit(
        "the SuperSpeed device was labelled %s, not USB3: %s" %
        (match.group(2), port_record))
now = int(match.group(4), 16)
if ((now >> 10) & 0xF) != XHCI_SPEED_SUPER:
    raise SystemExit(
        "the port's raw PORTSC does not carry the SuperSpeed speed value: "
        + port_record)
if match.group(6) != "1":
    raise SystemExit(
        "the SuperSpeed port did not report a connect: " + port_record)

groups = []
for candidate in records("[XHCI-PROTO] "):
    match = re.search(
        r"\[XHCI-PROTO\] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) index=(\d+) "
        r"groups=(\d+) major=(\d+) minor=\d+ name=(\S*) off=(\d+) count=(\d+) "
        r"ports=(\d+)-(\d+) ", candidate)
    if match is None:
        raise SystemExit("an [XHCI-PROTO] record is malformed: " + candidate)
    if match.group(1) == pci:
        # Groups: 1 pci, 2 index, 3 groups, 4 major, 5 name, 6 offset,
        # 7 count, 8 first port, 9 last port.
        groups.append((int(match.group(6)), int(match.group(7)),
                       int(match.group(8))))
covered = []
for port_offset, count, first in groups:
    covered.extend(range(first, first + count))
if sorted(covered) != list(range(1, 9)):
    raise SystemExit(
        "the protocol groups do not partition the controller's ports: %s" %
        sorted(covered))
if 1 not in [first for _offset, _count, first in groups]:
    raise SystemExit(
        "no protocol group starts at raw port 1, so the SuperSpeed port is "
        "not in a declared USB3 range")
print("SuperSpeed device published: " + line)
PY

if ! python3 - "$SERIAL_C" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import sys


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
text = SERIAL_VIEW.normalise(
    pathlib.Path(sys.argv[1]).read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("the boot panicked")
PY
then
    echo "usb_two_controllers: the SuperSpeed phase panicked" >&2
    exit 1
fi

kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "A SuperSpeed device on a USB3 port: the port and the device name it: PASS"
grep -F '[XHCI-PORT] ' "$(serial_view "$SERIAL_C")" | head -1 || true
grep -F ' speed=SUPER ' "$(serial_view "$SERIAL_C")" | tail -1 || true

# ---------------------------------------------------------------- phase D ---
# ITEM-519 increment 5: the positive half of the boot-medium guard.  The
# machine boots the hybrid ISO from the USB stick itself, so the loader's disk
# id and the bound stick's own MBR id are the same medium: the guard must say
# MATCH, name the whole device as the scope, and publish the loader's id and
# the device's id as equal.  This is the precondition the install journal
# (ITEM-521) will read before it may write to a medium at all.
SERIAL_D=$TEST_DIR/boot-medium.log
: >"$SERIAL_D"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=c,menu=off \
    -device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10' \
    -device 'qemu-xhci,id=x1,bus=pcie.0,addr=0x11' \
    -drive "if=none,id=bootmed,file=$IMAGE,format=raw,cache=writeback,file.locking=off" \
    -device 'usb-storage,drive=bootmed,bus=x1.0' \
    -serial "file:$SERIAL_D" \
    -no-reboot -no-shutdown >"$TEST_DIR/qemu-d.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_D" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
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
deadline = time.monotonic() + 200.0


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
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


text = wait_for(
    lambda text: "[INSTALL-MEDIUM] " in text and "hfoc> " in text,
    "the boot-medium record and the prompt",
)

medium = [line for line in text.splitlines()
          if line.startswith("[INSTALL-MEDIUM] ") and " pci=" in line]
match_record = None
for record in medium:
    if " state=MATCH " in record:
        match_record = record
if match_record is None:
    raise SystemExit(
        "the boot stick was not identified as the medium:\n"
        + "\n".join(medium))
if " reason=OK " not in match_record or " scope=WHOLE " not in match_record:
    raise SystemExit(
        "the match does not name OK and the whole device: " + match_record)
loader = re.search(r" loader_id=([0-9A-F]+)", match_record)
disk = re.search(r" disk_id=([0-9A-F]+)", match_record)
if loader is None or disk is None or loader.group(1) != disk.group(1):
    raise SystemExit(
        "the match does not compare the loader's id with the device's: "
        + match_record)
if " first_lba=0 " not in match_record:
    raise SystemExit(
        "the whole-device match does not start at zero: " + match_record)
if not [line for line in text.splitlines()
        if line.startswith("[USB-STORAGE] ") and " state=BOUND" in line]:
    raise SystemExit(
        "the boot medium bound no storage device to identify")
print("boot medium identified: " + match_record)
PY

if ! python3 - "$SERIAL_D" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import sys


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
text = SERIAL_VIEW.normalise(
    pathlib.Path(sys.argv[1]).read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("the boot panicked")
PY
then
    echo "usb_two_controllers: the boot-medium phase panicked" >&2
    exit 1
fi

kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "The boot medium names itself: the USB stick that booted the machine matches: PASS"
grep -F '[INSTALL-MEDIUM] ' "$(serial_view "$SERIAL_D")" | tail -1 || true
grep -F '[USB-STORAGE] ' "$(serial_view "$SERIAL_D")" | tail -1 || true

# ---------------------------------------------------------------- phase E ---
# ITEM-545: the SuperSpeed mass-storage bind.  Phase C proves a SUPERS device
# is addressed over the 512-byte control pipe and refused by name when it is
# not BOT; phase B proves a high-speed BOT stick binds and its bytes are read.
# Neither exercises the half the owner's blue port needs: a SuperSpeed BOT
# device - QEMU's `usb-bot` with a SCSI disk behind it, the same shape as the
# owner's 24a9:205a stick - on the USB3 half of the controller, enumerated,
# bound, and its sector 0 read through the bulk-IN path.
#
# `port=1` is not decoration: the same device also supports USB 2, and on a
# USB 2 port it would bind as a HIGH-speed stick and prove nothing about the
# USB3 half.  The proof therefore requires the port it bound on to be the one
# the controller's own protocol group calls USB3.
#
# What phase E adds over "it binds": the device's SuperSpeed companion
# descriptor declares a burst (bMaxBurst, USB 3.0 section 9.6.7), and the
# endpoint context the driver programs has to carry it in Max Burst Size
# (xHCI 1.1 section 6.2.3.5).  `burst_in=`/`burst_out=` in the bound record
# name what the context carried; zero would mean the companion was stepped
# over, which is the state this item found the parser in.
SERIAL_E=$TEST_DIR/superspeed-storage.log
python3 - "$TEST_DIR/usb3-storage.img" <<'PY'
import pathlib
import sys

marker = b"HFOC-USB3-BOT"
image = bytearray(1 << 20)
image[0:len(marker)] = marker
pathlib.Path(sys.argv[1]).write_bytes(image)
PY
: >"$SERIAL_E"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE,i8042=off,usb=off" -m 512M -smp "$QEMU_SMP" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10' \
    -drive "if=none,id=ssstick,file=$TEST_DIR/usb3-storage.img,format=raw,cache=writeback,file.locking=off" \
    -device 'usb-bot,id=ssbot,bus=x0.0,port=1' \
    -device 'scsi-hd,bus=ssbot.0,drive=ssstick' \
    -serial "file:$SERIAL_E" \
    -no-reboot -no-shutdown >"$TEST_DIR/qemu-e.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_E" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
deadline = time.monotonic() + 200.0
marker = b"HFOC-USB3-BOT"


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
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s\n' % what)
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


text = wait_for(
    lambda text: "[USB-STORAGE] " in text and " state=BOUND " in text
    and "hfoc> " in text,
    "the SuperSpeed storage bind and the prompt",
)

usb_records = [line for line in text.splitlines()
               if line.startswith("[USB ] ")]
super_records = [line for line in usb_records
                 if " speed=SUPER " in line and " role=STORAGE " in line]
if not super_records:
    raise SystemExit(
        "no SuperSpeed storage device was bound:\n" + "\n".join(usb_records))
line = super_records[-1]
match = re.search(
    r"\[USB \] pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F]) port=(\d+) "
    r"speed=SUPER role=STORAGE state=BOUND reason=OK", line)
if match is None:
    raise SystemExit("the SuperSpeed storage record is not a binding: " + line)
pci, port = match.group(1), int(match.group(2))

# The port the device bound on has to be the controller's USB3 half: the whole
# point of the phase is that the blue-port shape works, not that the device
# fell back to USB 2.
port_record = None
for candidate in text.splitlines():
    if " pci=%s port=%d " % (pci, port) in candidate and \
            candidate.startswith("[XHCI-PORT] "):
        port_record = candidate
if port_record is None:
    raise SystemExit("the SuperSpeed port published no [XHCI-PORT] line")
match = re.search(r"group=(\w+) ", port_record)
if match is None or match.group(1) != "USB3":
    raise SystemExit(
        "the SuperSpeed stick did not land on the USB3 half: " + port_record)

bound = [line for line in text.splitlines()
         if line.startswith("[USB-STORAGE] ") and " state=BOUND " in line]
if not bound:
    raise SystemExit("the bound device published no [USB-STORAGE] record")
record = bound[-1]
if " block=512 " not in record or " blocks=2048 " not in record:
    raise SystemExit(
        "the bound record does not carry the 1 MiB geometry: " + record)
# sector0 is the first four bytes of the sector the bulk-IN path read, as the
# little-endian word the record prints; the host wrote the marker there.
expected = "%02X%02X%02X%02X" % (marker[3], marker[2], marker[1], marker[0])
if " sector0=%s" % expected not in record:
    raise SystemExit(
        "the bound record did not read the marked sector 0 (want %s): %s"
        % (expected, record))
# ITEM-545: the companion descriptor's bMaxBurst reached the endpoint context.
burst = re.search(r" burst_in=(\d+) burst_out=(\d+)", record)
if burst is None:
    raise SystemExit(
        "the bound record names no endpoint burst: " + record)
in_burst, out_burst = int(burst.group(1)), int(burst.group(2))
if in_burst == 0 or out_burst == 0:
    raise SystemExit(
        "the SuperSpeed companion's burst was not programmed: " + record)
print("SuperSpeed BOT stick bound on the USB3 half: " + line)
print("its bursts and its marked sector: " + record)
PY

if ! python3 - "$SERIAL_E" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import sys


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
text = SERIAL_VIEW.normalise(
    pathlib.Path(sys.argv[1]).read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("the boot panicked")
PY
then
    echo "usb_two_controllers: the SuperSpeed storage phase panicked" >&2
    exit 1
fi

kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# The host's own view: the probe reads sector 0 and never writes, so the marker
# the host put there is still the first thing in the image.
python3 - "$TEST_DIR/usb3-storage.img" <<'PY'
import pathlib
import sys

marker = b"HFOC-USB3-BOT"
image = pathlib.Path(sys.argv[1]).read_bytes()
if len(image) != 1 << 20:
    raise SystemExit("the SuperSpeed stick image changed size: %d" % len(image))
if image[0:len(marker)] != marker:
    raise SystemExit("the marker sector was overwritten: " + repr(image[0:16]))
print("the host's marked sector is untouched")
PY

echo "A SuperSpeed BOT stick on the USB3 half binds, carries its companion burst and reads its marked sector: PASS"
grep -F '[USB ] ' "$(serial_view "$SERIAL_E")" | grep -F 'speed=SUPER' | tail -1 || true
grep -F '[USB-STORAGE] ' "$(serial_view "$SERIAL_E")" | grep -F 'state=BOUND' | tail -1 || true
