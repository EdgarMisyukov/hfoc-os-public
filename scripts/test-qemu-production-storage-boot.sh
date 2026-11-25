#!/usr/bin/env bash
# Prove the ordinary production ISO boot coordinator against real QEMU AHCI
# and NVMe disks: committed state reload, no-backend degradation, and
# ambiguity.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in cmp python3 qemu-system-x86_64 sha256sum timeout mktemp truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PRODUCTION_STORAGE_BOOT_INNER:-0}" != 1 ]]; then
    # ITEM-458: the USB HID phase now carries the keyboard loss burst as well -
    # a queued run of long reports plus a 300-character stream typed through the
    # same keyboard - so the wrapper rises with the budget it protects by the
    # ITEM-427 rule: a give-up writes its HFOC-QEMU-MARKER-TIMEOUT record
    # instead of being killed at exit 124 with no class to retry.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_STORAGE_BOOT_TIMEOUT:-300}s" \
        env HFOC_PRODUCTION_STORAGE_BOOT_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "$QEMU_MACHINE" = q35 || {
    echo "Production storage boot must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "$QEMU_SMP" = 1 || {
    echo "Production storage boot must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Production storage boot must not set QEMU_CPU" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Missing production image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
DISK_A=$TEST_DIR/hfoc-a.raw
DISK_B=$TEST_DIR/hfoc-b.raw
DISK_BLANK=$TEST_DIR/hfoc-blank.raw
DISK_A_BEFORE=$TEST_DIR/hfoc-a.before
# ITEM-392: a private copy for the boot without a PS/2 controller, so the
# "only the update record changed" assertion below stays exact.
DISK_KBD=$TEST_DIR/hfoc-kbd.raw
# ITEM-395: the same, for the boot whose console is driven by the emulated USB
# HID keyboard behind an emulated xHCI controller.
DISK_USB=$TEST_DIR/hfoc-usb.raw
# ITEM-406: and a third for the boot whose keyboard sits on the *second* xHCI
# controller, with a device that is not a keyboard on the first one.
DISK_USB_SECOND=$TEST_DIR/hfoc-usb-second.raw
# ITEM-407: and a fourth for the boot whose xHCI controller has no device at
# all, which is the shape the acceptance host published NO_DEVICE for.
DISK_USB_EMPTY=$TEST_DIR/hfoc-usb-empty.raw
QEMU_PID=
QEMU_LOG=$TEST_DIR/qemu.log
# ITEM-467: the socket-netdev peer that keeps the receive path busy while the
# operator types; the port is chosen once and its listener is QEMU's.
NETLOAD_PORT=
NETLOAD_PEER_PID=

# ITEM-406 follow-up: the phases whose logs are post-checked publish the records
# those checks read *after* the phase's own marker, and `run_boot_machine` used
# to quit the guest the moment the first marker appeared - so the post-checks
# raced the boot, and under load (the second-controller phase made the suite
# longer) they could read a log the guest was killed before writing.  A phase
# must therefore publish every marker it is going to be checked for before the
# guest is quit.  RUN_BOOT_ALSO carries them; run_boot resets it per phase.
RUN_BOOT_ALSO=()
UPDATE_LIFECYCLE_RECORD='[UPDATE-ATOMICITY] update_lifecycle v=1 state=IDLE outcome=NONE image=1 slot=1 attempts=0 pending=NO rollback=NONE'
AUDIT_CHAIN_RECORD='[AUDIT-INTEGRITY] audit_chain v=1 state=READY verify=OK tamper=0 skipped=0'
AUDIT_EXPORT_RECORD='audit_export v=1 state=COMPLETE'

cleanup() {
    local result=$?
    trap - EXIT INT TERM
    if [[ -n "$NETLOAD_PEER_PID" ]] &&
        kill -0 "$NETLOAD_PEER_PID" 2>/dev/null; then
        kill -TERM "$NETLOAD_PEER_PID" 2>/dev/null || true
        wait "$NETLOAD_PEER_PID" 2>/dev/null || true
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( result != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/production-storage-boot-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        cp -a "$TEST_DIR"/. "$artifact_dir"/ 2>/dev/null || true
        echo "Production storage boot artifacts: $artifact_dir" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$result"
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

trap 'exit 124' TERM
trap 'exit 130' INT

create_hfoc_disk() {
    python3 - "$1" <<'PY'
import hashlib
import pathlib
import struct
import sys
import zlib

path = pathlib.Path(sys.argv[1])
block_size = 512
block_count = (16 * 1024 * 1024) // block_size
first_lba = 34
partition_blocks = 512
disk = bytearray(block_size * block_count)

def store32(offset, value):
    disk[offset:offset + 4] = struct.pack("<I", value)

def store64(offset, value):
    disk[offset:offset + 8] = struct.pack("<Q", value)

header = block_size
disk[header:header + 8] = b"EFI PART"
store32(header + 8, 0x00010000)
store32(header + 12, 92)
store64(header + 24, 1)
store64(header + 32, block_count - 1)
store64(header + 40, first_lba)
store64(header + 48, block_count - 1)
store64(header + 72, 2)
store32(header + 80, 4)
store32(header + 84, 128)

entry = 2 * block_size + 128
disk[entry:entry + 16] = bytes([
    0xE1, 0x44, 0x01, 0x4C, 0x0D, 0x6E, 0x7A, 0x4B,
    0xB4, 0x9D, 0x22, 0x10, 0x33, 0x3D, 0x41, 0x31,
])
store64(entry + 16, 2)
store64(entry + 32, first_lba)
store64(entry + 40, first_lba + partition_blocks - 1)
store32(header + 88, zlib.crc32(disk[2 * block_size:3 * block_size]))
store32(header + 16, 0)
store32(header + 16, zlib.crc32(disk[header:header + 92]))

for store_offset in (0, 64, 128, 256):
    offset = (first_lba + store_offset) * block_size
    disk[offset:offset + 8] = b"HFOCDATA"
    disk[offset + 8] = 1
    store64(offset + 16, 1)
    store64(offset + 24, 16)

record = b"v1:system.hostname=hfoc-production\n"
offset = (first_lba + 1) * block_size
disk[offset:offset + 8] = b"HFOCJRNL"
disk[offset + 8] = 2
store64(offset + 16, 1)
store32(offset + 24, len(record))
disk[offset + 64:offset + 64 + len(record)] = record
disk[offset + 448:offset + 456] = b"HFOCCOMM"
store64(offset + 456, 1)
disk[offset + 32:offset + 64] = hashlib.sha256(
    disk[offset:offset + 32] +
    disk[offset + 64:offset + 64 + len(record)] +
    disk[offset + 448:offset + 512]
).digest()
path.write_bytes(disk)
PY
}

send_monitor_quit() {
    python3 - "$1" <<'PY'
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
}

run_boot() {
    local label=$1
    local expected=$2
    shift 2
    run_boot_machine "$label" "$QEMU_MACHINE" "$expected" "$@"
}

run_boot_machine() {
    local label=$1
    local machine=$2
    local expected=$3
    shift 3
    local serial_log=$TEST_DIR/$label.log
    local monitor_socket=$TEST_DIR/$label.monitor
    local deadline

    : >"$serial_log"
    : >"$QEMU_LOG"
    qemu-system-x86_64 \
        -machine "$machine" -m 512M -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$IMAGE" \
        "$@" -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    for marker in "$expected" ${RUN_BOOT_ALSO[@]+"${RUN_BOOT_ALSO[@]}"}; do
        deadline=$((SECONDS + 45))
        while (( SECONDS < deadline )); do
            if grep -Fq "$marker" "$(serial_view "$serial_log")"; then
                break
            fi
            if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$serial_log")"; then
                echo "$label: kernel panic" >&2
                return 1
            fi
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then
                echo "$label: QEMU exited before expected marker" >&2
                return 1
            fi
            sleep 0.05
        done
        if ! grep -Fq "$marker" "$(serial_view "$serial_log")"; then
            echo "$label: marker timeout: $marker" >&2
            return 1
        fi
    done
    RUN_BOOT_ALSO=()
    if [[ "$(grep -Fc "$expected" "$(serial_view "$serial_log")")" != 1 ]]; then
        echo "$label: expected marker count is not one" >&2
        return 1
    fi
    if ! send_monitor_quit "$monitor_socket"; then
        echo "$label: QEMU monitor quit failed" >&2
        return 1
    fi
    wait "$QEMU_PID"
    QEMU_PID=
    echo "$label: PASS"
}

# ITEM-395 + ITEM-406: boot a machine with no PS/2 controller but with a HID
# boot-protocol keyboard on an emulated xHCI controller, type console commands
# through that keyboard and require the console to answer them.
#
# The keyboard is pinned to an xHCI bus (qemu-xhci,id=xhci plus
# usb-kbd,bus=xhci.0) and the machine's built-in USB is disabled, because a bare
# "-device usb-kbd" binds to whichever USB bus QEMU finds first: on q35 that is
# the ICH9 UHCI/EHCI controller, which would exercise a path this kernel does
# not implement instead of the xHCI path under proof.
#
# The device list and the disk are parameters, so the ITEM-406 phase can put a
# device that is not a keyboard on the *first* controller and the keyboard on
# the second one, which is the acceptance host's layout.
#
# "hfoc> " repeats on every line redraw, so this phase waits for the unique
# readiness record before it types and for the command's own output marker
# after it, instead of run_boot_machine's single-occurrence marker check.
run_usb_hid_phase() {
    local label=$1
    local machine=$2
    local disk=$3
    # ITEM-458: the fourth argument turns on the loss burst; the phase is run
    # twice (one controller, then two) and the burst belongs to the first run.
    local loss_phase=$4
    shift 4
    local serial_log=$TEST_DIR/$label.log
    local monitor_socket=$TEST_DIR/$label.monitor
    local wait_seconds=60
    local devices=("$@")

    : >"$serial_log"
    : >"$QEMU_LOG"
    qemu-system-x86_64 \
        -machine "$machine" -m 512M -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$IMAGE" \
        ${devices[@]+"${devices[@]}"} \
        -drive "if=none,id=hfoc-usb,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device 'nvme,drive=hfoc-usb,serial=HFOC-USB' \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    if ! python3 - "$serial_log" "$monitor_socket" "$wait_seconds" \
        "$loss_phase" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
deadline = time.monotonic() + float(sys.argv[3])
loss_phase = sys.argv[4]
ready_marker = "local_input=AVAILABLE input_source=USB_HID"
prompt_marker = "hfoc> "
keyboard_marker = "LOCAL INPUT: SOURCE=USB_HID"
command_marker = "COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD"

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def serial_text():
    try:
        text = SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""
    # ITEM-463: a `[CONSOLE] scroll` record is written from inside the paint of
    # the line it follows, so it can split the echo this proof waits for.  The
    # shared renderer removes the periodic IRQ diagnostics; this record is the
    # proof's own extra, and the scroll record keeps being removed here.
    return re.sub(r"\[CONSOLE\] scroll[^\[\r\n]*\r?\n", "", text)

while time.monotonic() < deadline:
    text = serial_text()
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic before the USB keyboard proof")
    if ready_marker in text and prompt_marker in text:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="USB HID readiness record" '
        'deadline=5s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for the USB HID readiness record")

# ITEM-478 (round 16): the boot screen carries only the product banner.  The
# console publishes `[CONSOLE] boot paint lines=<n> service_lines=<m>` once, at
# the first prompt, so this is the boot screen's own count before any typed
# character can add to it: one painted line (the banner, "HFOC OS 1.0.0
# INITIALISED") and at least one service line that reached the serial stream
# and the durable journal instead of the screen.  A service record that painted
# would have pushed `lines` past the banner's one.
boot_match = re.search(
    r"\[CONSOLE\] boot paint lines=(\d+) service_lines=(\d+)",
    serial_text(),
)
if boot_match is None:
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="[CONSOLE] boot paint lines" '
        'deadline=5s\n')
    sys.stderr.flush()
    raise SystemExit("the boot screen record was never published")
boot_paint_lines = int(boot_match.group(1))
boot_service_lines = int(boot_match.group(2))
if boot_paint_lines > 1:
    raise SystemExit(
        "a service line was painted during boot: paint lines="
        f"{boot_paint_lines}")
if boot_service_lines < 1:
    raise SystemExit(
        "the boot published no service line to the serial stream and journal")
print(f"Boot screen paint lines: {boot_paint_lines} "
      f"(service lines kept off the screen: {boot_service_lines})")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
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

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        receive_prompt()
        time.sleep(0.2)

    receive_prompt()
    # "kbd" makes the console print the live input path and the USB report
    # count, so the proof shows the keyboard is not only enumerated but being
    # serviced; "help" then proves a whole command line was typed through it.
    for character in "kbd":
        send_key(character)
    send_key("ret")
    for character in "help":
        send_key(character)
    send_key("ret")

    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("kernel panic after the USB keyboard input")
        if keyboard_marker in text and command_marker in text:
            break
        time.sleep(0.05)
    else:
        raise SystemExit(
            "the console did not answer the command typed through the USB "
            "keyboard")

    # ITEM-477 (round 15): the periodic records are still in the durable
    # journal; the `bootlog` dump is read at the end of this phase, after every
    # exact echo check, and its check is described there.

    if loss_phase == "1":
        # ITEM-458: the loss proof.
        #
        # The 2026-09-19 hardware run lost characters while the screen scrolled.
        # The two numbers that turn that into evidence are the bounded console
        # queue's refusals and the worst wait between two reports the service
        # pass delivered, and this phase drives both:
        #
        #   * a run of long reports is queued first, so the console is busy
        #     printing while the stream arrives;
        #   * 300+ characters are typed through the same USB keyboard in three
        #     lines (the console's line editor holds 127), and every character
        #     is required to appear in the line the console repaints before the
        #     next one is sent - the echo is the end-to-end evidence that no
        #     character was lost between the device and the screen.  Nothing is
        #     ever retyped: a character that does not arrive is the failure this
        #     proof exists for;
        #   * the `kbd` command then publishes DROPPED= and GAP_MS_MAX=, which
        #     the caller requires to be zero and inside the budget.
        #
        # Each key is held for 1 ms and the next is sent only after the echo of
        # the previous one.  Both halves were measured on 2026-09-19: HMP's
        # `sendkey` holds a key for 100 ms by default, so a stream sent at the
        # contract's 10 ms pace leaves several keys down at once, QEMU reports
        # that key state in one boot-protocol report, and the report's slot
        # order is not the typing order - the burst echoed as
        # `epaipaicgjnswidz...` instead of the typed line.  A guest that is
        # still catching up (this one mirrors every console character to a
        # 115200-baud serial port and publishes an [IOAPIC] record per
        # interrupt) lets QEMU's own USB HID queue overflow, which drops keys
        # in the emulator - not this proof's subject.  Waiting for the echo
        # keeps one key in flight at a time and measures what the machine lost.
        def send_key_paced(key):
            monitor.sendall(("sendkey " + key + " 1\n").encode("ascii"))
            receive_prompt()
            time.sleep(0.01)

        # ITEM-467: the pointer is driven through the same QEMU monitor while
        # the keyboard stream and the ARP flood run, so its interrupt transfer
        # has to complete and be re-armed under the same load.  A movement is
        # one relative report; the click makes a press edge.
        def move_mouse(dx, dy):
            monitor.sendall(("mouse_move %d %d\n" % (dx, dy)).encode("ascii"))
            receive_prompt()
            time.sleep(0.01)

        def click_mouse():
            monitor.sendall(b"mouse_button 1\n")
            receive_prompt()
            time.sleep(0.05)
            monitor.sendall(b"mouse_button 0\n")
            receive_prompt()
            time.sleep(0.05)

        def type_line_exact(line, offset, key_names=None):
            for index in range(1, len(line) + 1):
                character = line[index - 1]
                # ITEM-459: punctuation has no one-letter HMP key name, so the
                # caller may pass the name QEMU knows the key by.
                send_key_paced(key_names[character]
                               if key_names is not None else character)
                prefix = line[:index]
                echo_deadline = time.monotonic() + 5.0
                while time.monotonic() < echo_deadline:
                    text = serial_text()
                    if "*** HFOC KERNEL PANIC ***" in text:
                        raise SystemExit(
                            "kernel panic during the keyboard loss stream")
                    # The console repaints the line it is editing, so the
                    # echo to look for is the prompt plus the line so far - and
                    # the prompt is what makes a one-character prefix a real
                    # acknowledgement instead of a letter of the report that is
                    # still printing.  A few characters of slack cover the
                    # normaliser dropping whole IRQ lines at the boundary.
                    # ITEM-482: normal typing takes the one-cell fast path, so
                    # the prompt is not re-emitted; the typed characters are the
                    # witness.  Whole `[NAME] ...` records are stripped (one may
                    # land between two characters) and the view must end with the
                    # exact prefix, which a lost or duplicated character breaks.
                    if ("hfoc> " + prefix) in text[max(0, offset - 64):]:
                        break
                    if re.sub(r"\[[A-Z][A-Z0-9 _-]*\][^\r\n]*\r?\n", "",
                              text).endswith(prefix):
                        break
                    time.sleep(0.01)
                else:
                    sys.stderr.write(
                        'HFOC-QEMU-MARKER-TIMEOUT marker="USB keyboard echo of '
                        + line[:index] + '" deadline=5s\n')
                    sys.stderr.flush()
                    raise SystemExit(
                        "character " + str(index) + " of a line typed through "
                        "the USB keyboard was never echoed")

        stream_lines = (
            "abcdefghijklmnopqrstuvwxyz" * 4,
            "0123456789" * 10,
            "qwertyuiopasdfghjklzxcvbnm" * 4,
        )
        # The queued reports: the machine prints them while the stream below
        # arrives.  Their own keystrokes go through the same exact-echo path.
        for _ in range(8):
            queued_offset = len(serial_text())
            type_line_exact("appliance", queued_offset)
            send_key("ret")
        typed_characters = 0
        for line in stream_lines:
            offset = len(serial_text())
            type_line_exact(line, offset)
            typed_characters += len(line)
            send_key("ret")
            # ITEM-467: the pointer moves between the typed lines, so the
            # keyboard and the mouse share the loop under the same flood.
            move_mouse(24, 17)
        click_mouse()
        move_mouse(-11, -7)

        # ITEM-480 (round 18): a dense burst of relative reports, sent without
        # the per-move prompt wait, so the driver's queue holds several reports
        # in one loop turn and the batch path sums them into fewer drawings than
        # reports.  The burst is sent without a settling sleep: the sign stream
        # that follows keeps the keyboard reporting, so the keyboard's
        # inter-report gap - which the load proof bounds at 500 ms - stays
        # inside its budget while the pointer's queue drains.  The host proof
        # pins the coalescing arithmetic; this drives the path under load.
        for _ in range(24):
            monitor.sendall(b"mouse_move 2 1\n")

        # ITEM-459: every printable sign the USB path can carry, typed through
        # the same keyboard with the same per-character echo.  The characters
        # are the ones HMP can name; the key the owner pressed (usage 0x64,
        # `Keyboard Non-US \ and |`) has no QEMU key name of its own, so the
        # table itself is the host proof's subject and this line is the
        # end-to-end half - `|`, `\`, `{`, `}`, `<`, `>` and the rest all have
        # to reach the screen.
        signs = (
            ("\\", "backslash"), ("|", "shift-backslash"),
            ("{", "shift-bracket_left"), ("}", "shift-bracket_right"),
            ("[", "bracket_left"), ("]", "bracket_right"),
            ("~", "shift-grave_accent"), ("`", "grave_accent"),
            (":", "shift-semicolon"), (";", "semicolon"),
            ('"', "shift-apostrophe"), ("'", "apostrophe"),
            ("<", "shift-comma"), (">", "shift-dot"),
            (",", "comma"), (".", "dot"),
            ("/", "slash"), ("?", "shift-slash"),
            ("_", "shift-minus"), ("-", "minus"),
            ("+", "shift-equal"), ("=", "equal"),
            ("*", "shift-8"), ("(", "shift-9"), (")", "shift-0"),
        )
        signs_line = "".join(pair[0] for pair in signs)
        signs_keys = {pair[0]: pair[1] for pair in signs}
        signs_offset = len(serial_text())
        type_line_exact(signs_line, signs_offset, signs_keys)
        # The whole line, not only its prefixes: with the ITEM-482 one-cell fast
        # path the console serializes each character as it is typed instead of
        # repainting the line, so the completed line's own characters in the
        # record-stripped view are the witness, and a prefix-only arrival is not
        # enough.
        signs_view = re.sub(r"\[[A-Z][A-Z0-9 _-]*\][^\r\n]*\r?\n", "",
                            serial_text())
        if ("hfoc> " + signs_line) not in serial_text() and \
                not signs_view.endswith(signs_line):
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="complete USB keyboard signs '
                'line" deadline=5s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the punctuation line was never repainted whole")
        send_key("ret")
        print("USB keyboard signs line: " + signs_line)

        # A key the machine has no name for is published, not silent.  F13 is
        # the key QEMU can send whose HID usage (0x68) is past the character
        # tables, so one press is one `[KBD ] unmapped` record.
        monitor.sendall(b"sendkey f13 1\n")
        receive_prompt()
        unmapped_deadline = time.monotonic() + 15.0
        while time.monotonic() < unmapped_deadline:
            if "[KBD ] unmapped usage=" in serial_text():
                break
            time.sleep(0.05)
        else:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[KBD ] unmapped usage" '
                'deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "an unmapped key did not name itself on the console")

        # Exactly one finding for one press: a held or repeated key must not
        # repeat the record.
        if serial_text().count("[KBD ] unmapped usage=") != 1:
            raise SystemExit(
                "the unmapped-usage record is missing or not exactly once")

        for character in "kbd":
            send_key(character)
        send_key("ret")
        counters_deadline = time.monotonic() + 30.0
        while time.monotonic() < counters_deadline:
            if "GAP_MS_MAX=" in serial_text():
                break
            time.sleep(0.05)
        else:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="kbd loss counters" '
                'deadline=30s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the console did not publish the keyboard loss counters")

        # ITEM-467: the machine's own short input record - published on the
        # heartbeat cadence, so the journal's line bound cannot cut it off - is
        # the durable evidence.  Wait for one whose keyboard report count covers
        # the typed stream, then require the pointer moved and clicked under the
        # same flood, nothing was dropped, and the loop's worst turn is inside
        # the budget.
        input_deadline = time.monotonic() + 60.0
        input_fields = None
        input_text = ""
        marker = re.compile(r"\[PRODUCTION-INPUT\]([^\r\n]*)")
        input_required = (
            "kbd_reports", "kbd_dropped", "kbd_gap_ms_max", "iter_max_us",
            "mouse_reports", "mouse_moves", "mouse_presses",
            "kbd_armed", "kbd_mse", "mouse_armed", "mouse_mse",
            "mouse_no_trb", "loops",
        )
        while time.monotonic() < input_deadline:
            found = marker.findall(serial_text())
            if found:
                fields = dict(re.findall(r"(\w+)=(\S+)", found[-1]))
                # ITEM-470: the record is a once-a-second snapshot, so a
                # qualifying one must already carry the pointer's evidence too;
                # otherwise a slow guest is read as "the pointer never reported"
                # instead of "the snapshot was taken before the moves landed".
                if all(key in fields for key in input_required) and \
                        int(fields["kbd_reports"]) >= typed_characters and \
                        int(fields["mouse_reports"]) > 0 and \
                        int(fields["mouse_moves"]) > 0 and \
                        int(fields["mouse_presses"]) > 0:
                    input_fields = fields
                    input_text = found[-1].strip()
                    break
            time.sleep(0.1)
        if input_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[PRODUCTION-INPUT] with '
                'kbd_reports>=%d and the pointer" deadline=60s\n'
                % typed_characters)
            sys.stderr.flush()
            raise SystemExit(
                "the input record never carried the typed stream and the "
                "pointer")
        if int(input_fields["kbd_dropped"]) != 0:
            raise SystemExit("the input record dropped keyboard reports")
        if int(input_fields["kbd_gap_ms_max"]) > 500:
            raise SystemExit("the keyboard service gap is outside the budget")
        if int(input_fields["iter_max_us"]) > 500000:
            raise SystemExit("the loop turn is outside the input budget")
        # ITEM-469: the fix is a queue, not a single transfer - a READY
        # keyboard must have at least two TRBs armed so a completion is
        # followed by a fresh one inside the same handler - and the missing
        # service error the single-TRB window produced must be zero.
        if int(input_fields["kbd_armed"]) < 2:
            raise SystemExit(
                "the keyboard did not hold a queue of armed transfers")
        if int(input_fields["kbd_mse"]) != 0:
            raise SystemExit(
                "the keyboard saw a missed-service completion")
        # ITEM-470: the pointer's 1 ms endpoint needs a whole loop turn of
        # armed transfers (sixteen), no missed service, and no completion that
        # left the endpoint with no TRB - the silent NAK loss `mouse_mse`
        # cannot name.  `loops` is the literal loop-turn counter that settles
        # how `passes` and `iter_us` relate.
        if int(input_fields["mouse_armed"]) < 16:
            raise SystemExit(
                "the pointer did not hold a full turn of armed transfers")
        if int(input_fields["mouse_mse"]) != 0:
            raise SystemExit(
                "the pointer saw a missed-service completion")
        if int(input_fields["mouse_no_trb"]) != 0:
            raise SystemExit(
                "a pointer completion left the endpoint without a TRB")
        if int(input_fields["loops"]) <= 0:
            raise SystemExit("the loop published no turns")
        if int(input_fields["mouse_reports"]) <= 0 or \
                int(input_fields["mouse_moves"]) <= 0 or \
                int(input_fields["mouse_presses"]) <= 0:
            raise SystemExit(
                "the pointer did not report, move and click under the load")
        # ITEM-480 (round 18): the pointer's turn budget and coalescing.  The
        # record names the named budget, how many decoded reports the window
        # consumed and how many drawings it took; the proof requires the budget
        # constant, at least one drawing, and strictly fewer drawings than
        # reports - the coalesced run and the un-drawn button edges both make
        # that strict under the flood.  The host proof in
        # tests/local-input-host.c pins the arithmetic exactly; this proves the
        # batch path runs end to end under load and the loop turn stays bounded.
        batch_marker = re.compile(r"\[POINTER-BATCH\]([^\r\n]*)")
        batch_deadline = time.monotonic() + 15.0
        batch_fields = None
        batch_text = ""
        while time.monotonic() < batch_deadline:
            records = batch_marker.findall(serial_text())
            if records:
                fields = dict(re.findall(r"(\w+)=(\S+)", records[-1]))
                if all(key in fields for key in
                       ("draws", "events", "budget", "draw_us", "sync_us")):
                    batch_fields = fields
                    batch_text = records[-1].strip()
                    break
            time.sleep(0.1)
        if batch_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[POINTER-BATCH] with the '
                'budget fields" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the pointer never published its batch record")
        if int(batch_fields["budget"]) != 3:
            raise SystemExit(
                "the pointer's turn budget is not the named three batches")
        if int(batch_fields["draws"]) <= 0:
            raise SystemExit("the pointer's batch path never drew anything")
        if int(batch_fields["events"]) <= int(batch_fields["draws"]):
            raise SystemExit(
                "the pointer drew as many batches as reports it consumed")
        print("Pointer batch budget: " + batch_text)
        # ITEM-486 (round 24): the mouse event path names the furthest stage its
        # last window reached.  Under this flood the pointer is reporting and
        # drawing, so the name must be one of the classifier's outcomes - the
        # record's presence and vocabulary are what the next hardware run reads
        # to tell NO_COMPLETION from NO_REPORT from WATCHDOG_GAVE_UP.
        path_marker = re.compile(r"\[PRODUCTION-INPUT-WINDOW\]([^\r\n]*)")
        path_deadline = time.monotonic() + 15.0
        path_fields = None
        while time.monotonic() < path_deadline:
            records = path_marker.findall(serial_text())
            if records:
                fields = dict(re.findall(r"(\w+)=(\S+)", records[-1]))
                if "mouse_path" in fields:
                    path_fields = fields
                    break
            time.sleep(0.1)
        if path_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[PRODUCTION-INPUT-WINDOW] '
                'with mouse_path" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the input window never named the mouse event path")
        if path_fields["mouse_path"] not in (
                "OK", "NO_COMPLETION", "NO_REPORT", "NO_LOCAL_INPUT",
                "QUEUE_BACKLOG", "WATCHDOG_REARM", "WATCHDOG_GAVE_UP",
                "POINTER_PROCESSING", "FRAMEBUFFER_RENDER"):
            raise SystemExit(
                "the mouse event path named an unknown outcome "
                f"(mouse_path={path_fields['mouse_path']})")
        print("Mouse event path: " + path_fields["mouse_path"])
        # ITEM-473: the sixteen-deep pointer queue must not fill the session's
        # event ring.  The ring now holds its whole 4 KiB page (256 TRBs);
        # `ringfull=0` is the proof the controller never raised Event Ring Full
        # (21), and `mouse_arm_depth=16` names the depth that would have filled
        # the old sixteen-TRB ring.  Both fields ride the short input tail.
        tail_marker = re.compile(r"\[PRODUCTION-INPUT-TAIL\]([^\r\n]*)")
        tail_deadline = time.monotonic() + 15.0
        tail_fields = None
        while time.monotonic() < tail_deadline:
            tails = tail_marker.findall(serial_text())
            if tails:
                fields = dict(re.findall(r"(\w+)=(\S+)", tails[-1]))
                if "mouse_arm_depth" in fields and "ringfull" in fields and \
                        "kbd_watchdog" in fields and \
                        "mouse_watchdog" in fields:
                    tail_fields = fields
                    break
            time.sleep(0.1)
        if tail_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[PRODUCTION-INPUT-TAIL] '
                'with the ITEM-473/ITEM-479 fields" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the input tail never carried the arm depth and ringfull")
        if int(tail_fields["mouse_arm_depth"]) < 16:
            raise SystemExit(
                "the pointer's arm depth is below a full loop turn")
        if int(tail_fields["ringfull"]) != 0:
            raise SystemExit(
                "the event ring reported full at the sixteen-deep queue")
        if int(tail_fields["kbd_watchdog"]) < 0 or \
                int(tail_fields["mouse_watchdog"]) < 0:
            raise SystemExit("the input tail carried a negative watchdog count")
        # ITEM-479 (round 17): the stalled-endpoint watchdog.  The pointer is
        # bound and armed at boot and is not moved until the load loop below,
        # so a role that holds armed transfers past the one-second bound must
        # publish its own record and re-arm.  The record names the role, the
        # slot, the endpoint, the depth, the wait and the action; the proof
        # requires one with a wait past the bound, because a watchdog that
        # fired inside its own bound would be the false positive this item
        # exists to avoid.
        watchdog_marker = re.compile(r"\[INPUT-WATCHDOG\]([^\r\n]*)")
        watchdog_deadline = time.monotonic() + 15.0
        watchdog_fields = None
        while time.monotonic() < watchdog_deadline:
            records = watchdog_marker.findall(serial_text())
            if records:
                fields = dict(re.findall(r"(\w+)=(\S+)", records[-1]))
                if fields.get("action") == "REARM" and \
                        "role" in fields and "silent_ms" in fields:
                    watchdog_fields = fields
                    break
            time.sleep(0.1)
        if watchdog_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[INPUT-WATCHDOG] with '
                'action=REARM" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the stalled-endpoint watchdog never published its record")
        if watchdog_fields["role"] not in ("keyboard", "mouse"):
            raise SystemExit(
                "the watchdog named an unknown role "
                f"(role={watchdog_fields['role']})")
        if int(watchdog_fields["armed"]) < 1:
            raise SystemExit("the watchdog fired with nothing armed")
        if int(watchdog_fields["silent_ms"]) <= 1000:
            raise SystemExit(
                "the watchdog fired inside the silence bound "
                f"(silent_ms={watchdog_fields['silent_ms']})")
        if int(watchdog_fields["count"]) < 1:
            raise SystemExit("the watchdog did not count its recovery")
        print("Stalled-endpoint watchdog: " + records[-1].strip())
        # ITEM-474 (round 13): the loop's phase budget.  The record must carry
        # every field, the console's own cost must be subtracted from the
        # measured spans (so the parts do not exceed the work), the work and
        # the halt must cover the turn, and the average turn must be inside the
        # same half-second budget the input record pins.  `turns>=1` is the
        # proof the window held at least one turn, so the averages describe a
        # turn rather than an empty window.
        #
        # ITEM-475 (round 14): the production build must tick on PIT, and the
        # loop must actually take those wakeups.  The owner's machine lived at
        # ~2.4 wakeups/s with the Local APIC timer as the sole source, so this
        # proof requires `tick_source=pit`, a measured `wake_hz` in the PIT
        # band, and enough `turns` in the window that the 100 Hz source is
        # what drives the loop rather than a starved halted turn.
        phases_marker = re.compile(r"\[LOOP-PHASES\]([^\r\n]*)")
        phases_required = (
            "turns", "console_us", "xhci_us", "input_us", "net_us",
            "other_us", "work_us", "hlt_us", "turn_us",
            "tick_source", "wake_hz",
            # ITEM-479 (round 17): the input path's three sub-spans, the
            # window's worst turn and the span it spent, and how many turns ran
            # past the 50 ms stall bound.
            "in_xhci_us", "in_kbd_us", "in_ptr_us",
            "worst_us", "worst_phase", "over50",
            # ITEM-482 (round 20): the input budgets' evidence.  `kbd_events`
            # and `ptr_draws` are the window's worst turn, so the budget
            # comparison below says no turn served more than the bound.
            "kbd_events", "kbd_budget", "kbd_draw_us", "kbd_sync_us",
            "ptr_events", "ptr_draws", "ptr_budget",
            # ITEM-486 (round 23): the former `other_us` remainder, split into
            # its named parts, so a freeze names the subsystem that spent it.
            "svc_us", "journal_us", "store_us",
            # ITEM-486 (round 24): the framebuffer's split cost and the
            # network pass's bounded budget, so the next run proves the shadow
            # draw, the batched flush and the network cap are live.
            "fb_shadow_draw_us", "fb_dirty_flush_us", "fb_dirty_bytes",
            "fb_dirty_rects", "fb_flush_count",
            "net_budget_us", "net_budget_hits",
            # ITEM-486 (round 24): the loop's own counters beside the window
            # they are read against, so "iterations per second", "ticks per
            # second" and "turns per window" are checked in one record instead
            # of being compared across two and misread as a factor of eight.
            "loops", "ticks", "real_seconds",
        )
        phases_phases = (
            "console", "xhci", "input", "net",
            "in_xhci", "in_kbd", "in_ptr", "other",
            # ITEM-486 (round 23): the named parts of the remainder.
            "svc", "journal", "store",
        )
        phases_deadline = time.monotonic() + 15.0
        phases_fields = None
        while time.monotonic() < phases_deadline:
            phase_lines = phases_marker.findall(serial_text())
            if phase_lines:
                fields = dict(re.findall(r"(\w+)=(\S+)", phase_lines[-1]))
                # The first record of the boot is published with an empty
                # window (`turns=0`) and before the cycle clock has measured a
                # tick rate (`wake_hz=0`); a qualifying one has already held a
                # whole turn against the PIT source, so the averages describe a
                # live loop rather than an empty window.
                if all(key in fields for key in phases_required) and \
                        int(fields["turns"]) >= 20 and \
                        int(fields["wake_hz"]) >= 50:
                    phases_fields = fields
                    break
            time.sleep(0.1)
        if phases_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[LOOP-PHASES] with the '
                'ITEM-474/ITEM-475 fields" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the loop-phase record never carried every field")
        # ITEM-476: every qualifying record must keep its parts inside the
        # turn's work, not only the one this proof happens to read.  A record
        # published in the middle of a turn used to mix a completed turn's work
        # with an unfinished turn's spans, and whether that showed depended on
        # where the second boundary fell - the check below pins the accounting,
        # not the timing luck.
        for phase_line in phase_lines:
            candidate = dict(re.findall(r"(\w+)=(\S+)", phase_line))
            if not all(key in candidate for key in phases_required):
                continue
            if int(candidate["turns"]) < 20 or int(candidate["wake_hz"]) < 50:
                continue
            candidate_sum = sum(int(candidate[key]) for key in (
                "console_us", "xhci_us", "input_us", "net_us",
                "svc_us", "journal_us", "store_us", "other_us"))
            if candidate_sum > int(candidate["work_us"]):
                raise SystemExit(
                    "the loop's phases exceed the turn's work in a record: "
                    + phase_line.strip())
        if int(phases_fields["turns"]) < 1:
            raise SystemExit(
                "the loop-phase record described an empty window")
        if phases_fields["tick_source"] != "pit":
            raise SystemExit(
                "the production loop did not name PIT as its tick source "
                f"(tick_source={phases_fields['tick_source']})")
        if int(phases_fields["wake_hz"]) < 50:
            raise SystemExit(
                "the production loop woke fewer than 50 times a second "
                f"(wake_hz={phases_fields['wake_hz']})")
        if int(phases_fields["turns"]) < 20:
            raise SystemExit(
                "the production loop took fewer than 20 turns in the "
                f"window (turns={phases_fields['turns']})")
        accounted = sum(int(phases_fields[key]) for key in (
            "console_us", "xhci_us", "input_us", "net_us",
            "svc_us", "journal_us", "store_us", "other_us"))
        if accounted > int(phases_fields["work_us"]):
            raise SystemExit(
                "the loop's phases exceed the turn's work")
        # ITEM-486 (round 24): the framebuffer's split cost and the network
        # pass's bounded budget.  The budget must be the named two milliseconds;
        # the flush counters are telemetry whose presence the required-field
        # check above already proves, and the exact flush accounting is pinned
        # by the host proof (the QEMU TCG clock is not a hardware measurement).
        if int(phases_fields["net_budget_us"]) != 2000:
            raise SystemExit(
                "the network pass did not publish its bounded 2000 us budget")
        if int(phases_fields["fb_dirty_bytes"]) < 0 or \
                int(phases_fields["fb_flush_count"]) < 0:
            raise SystemExit("the framebuffer published a negative counter")
        if int(phases_fields["turn_us"]) > 500000:
            raise SystemExit(
                "the loop turn is outside the phase budget")
        # ITEM-486 (round 24): the two clocks of the loop, in the record that
        # names the window they disagreed about.  `loops` is cumulative and
        # `turns` is the window's, so the counter can never be below the window;
        # the PIT drives at HFOC_TIMER_HZ, so a record that has held a live
        # window has also taken at least one tick per real second.  These are
        # the invariants that make the old ~1.55x reading decidable from one
        # record instead of guessed from two.
        if int(phases_fields["loops"]) < int(phases_fields["turns"]):
            raise SystemExit(
                "the loop's cumulative counter is below its own window's turns "
                f"(loops={phases_fields['loops']} "
                f"turns={phases_fields['turns']})")
        if int(phases_fields["real_seconds"]) < 1:
            raise SystemExit(
                "a live window was published before one real second elapsed")
        if int(phases_fields["ticks"]) < int(phases_fields["real_seconds"]):
            raise SystemExit(
                "the PIT took fewer ticks than the seconds it counts "
                f"(ticks={phases_fields['ticks']} "
                f"real_seconds={phases_fields['real_seconds']})")
        # ITEM-479 (round 17): the input path's sub-spans are measured inside
        # the spans that already existed, so each stays inside its parent; the
        # worst turn of the window is at least the window's average and its
        # named phase is one this build publishes; and the stall count can
        # never exceed the turns the window held.
        if int(phases_fields["in_xhci_us"]) > int(phases_fields["xhci_us"]):
            raise SystemExit(
                "the driver service sub-span exceeds the driver span")
        for sub_phase in ("in_kbd_us", "in_ptr_us"):
            if int(phases_fields[sub_phase]) > int(phases_fields["input_us"]):
                raise SystemExit(
                    f"the {sub_phase} sub-span exceeds the input span")
        if int(phases_fields["worst_us"]) < int(phases_fields["turn_us"]):
            raise SystemExit(
                "the window's worst turn is below its own average")
        if phases_fields["worst_phase"] not in phases_phases:
            raise SystemExit(
                "the worst turn named an unknown phase "
                f"(worst_phase={phases_fields['worst_phase']})")
        if int(phases_fields["over50"]) > int(phases_fields["turns"]):
            raise SystemExit(
                "more stall turns than turns in the window")
        # ITEM-482 (round 20): neither input source may make one turn
        # unbounded.  `kbd_events`/`ptr_draws` are the window's worst turn, so
        # each must stay at or below the budget the record publishes; the
        # keyboard's own echo must never cost more than the whole input span it
        # is measured inside.
        if int(phases_fields["kbd_events"]) > int(phases_fields["kbd_budget"]):
            raise SystemExit(
                "a turn served more keyboard events than the keyboard budget")
        if int(phases_fields["ptr_draws"]) > int(phases_fields["ptr_budget"]):
            raise SystemExit(
                "a turn drew the pointer more times than the pointer budget")
        print("Loop phases: " + phase_lines[-1].strip())
        if "[PRODUCTION-FRAME] class=ARP_REQUEST" not in serial_text():
            raise SystemExit(
                "the network flood never reached the machine")

        # ITEM-477 (round 15): the periodic records reach the journal and not
        # the screen.  The phase record now carries the console's painted-line
        # count, so consecutive seconds show the heartbeat still publishing
        # (a live `turns`/`wake_hz` window arrives every second) while the
        # screen stays put: a periodic record that painted would add at least
        # its own line every second.  One interval may exceed the bound - a
        # one-time state change such as the local menu's idle offer is exactly
        # what the screen is for - but the periodic stream must not, so more
        # than one such interval is the regression this item exists to prevent.
        # The samples start after the last typed key, so the stream's own echo
        # is not measured here; `turns` is a per-second window, not a counter,
        # so only the painted-line totals are compared.
        # ITEM-523: the samples are awaited by progress, not by a fixed window.
        # The record is published once a second of *guest* time, so a fixed wall
        # clock asks the guest for time the host can spend before the guest has
        # it: the DHCP/DNS artifact in docs/ci-self-hosted-runner.md measured a
        # guest clock lagging wall time by roughly 7x under the acceptance
        # phase's parallel load.  Run 38264136090 lost this proof that way on
        # 2026-10-12 - the retry passed on the same build - so the give-up now
        # names the record that stopped arriving, not the wall clock that ran
        # out: the wait ends when six samples have arrived, gives up when no
        # qualifying record has arrived for `paint_stall` seconds (a heartbeat
        # that really stopped publishing is still a named failure), and stays
        # bounded by `paint_cap` whatever the guest does.  The assertion below
        # - three samples at least, one painted-line jump above two at most -
        # is unchanged.
        paint_next = len(phases_marker.findall(serial_text()))
        paint_samples = []
        paint_stall = 20.0
        paint_cap = 60.0
        paint_started = time.monotonic()
        paint_last = paint_started
        paint_expired = "stall"
        while len(paint_samples) < 6:
            paint_lines = phases_marker.findall(serial_text())
            for phase_line in paint_lines[paint_next:]:
                candidate = dict(re.findall(r"(\w+)=(\S+)", phase_line))
                if not all(key in candidate for key in phases_required) or \
                        "paint_lines" not in candidate:
                    continue
                if int(candidate["turns"]) < 5 or \
                        int(candidate["wake_hz"]) < 50:
                    continue
                paint_samples.append((int(candidate["paint_lines"]),
                                      int(candidate["turns"])))
                paint_last = time.monotonic()
                if len(paint_samples) >= 6:
                    break
            paint_next = len(paint_lines)
            paint_now = time.monotonic()
            if paint_now - paint_last >= paint_stall:
                paint_expired = "stall"
                break
            if paint_now - paint_started >= paint_cap:
                paint_expired = "cap"
                break
            time.sleep(0.2)
        if len(paint_samples) < 3:
            paint_deadline = paint_stall if paint_expired == "stall" \
                else paint_cap
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[LOOP-PHASES] with '
                'paint_lines" deadline=%ds\n' % int(paint_deadline))
            sys.stderr.flush()
            raise SystemExit(
                "the phase record never published the paint samples "
                f"({len(paint_samples)} qualifying records in "
                f"{paint_cap:.0f}s)")
        paint_jumps = 0
        for before, after in zip(paint_samples, paint_samples[1:]):
            if after[0] - before[0] > 2:
                paint_jumps += 1
        if paint_jumps > 1:
            raise SystemExit(
                "the periodic records keep painting the screen "
                f"({paint_jumps} of {len(paint_samples) - 1} seconds grew "
                "by more than two lines)")

        # ITEM-486 (round 24): the run's surviving state.  The durable journal
        # is a 511-record ring - about seventy-five seconds at these cadences -
        # so the motivating 4.7-minute run had already lost the record that
        # named its pointer's death by the time the log was read.  Every field
        # of `[LOOP-SUMMARY]` is cumulative, so the newest one describes the
        # whole run whatever the ring has reclaimed; this proof requires the
        # record to appear with its fields and to keep the invariants that make
        # it readable after the fact.
        summary_marker = re.compile(r"\[LOOP-SUMMARY\]([^\r\n]*)")
        summary_required = (
            "loops", "iter_max_us", "ticks", "real_seconds",
            "mouse_path", "mouse_completed", "mouse_reports", "mouse_armed",
            "mouse_watchdog", "mouse_silent_max_ms", "mouse_gave_up",
            "fb_shadow_draw_us", "fb_flush_count",
            "net_budget_us", "net_budget_hits",
        )
        summary_deadline = time.monotonic() + 15.0
        summary_fields = None
        while time.monotonic() < summary_deadline:
            summary_lines = summary_marker.findall(serial_text())
            if summary_lines:
                fields = dict(re.findall(r"(\w+)=(\S+)", summary_lines[-1]))
                if all(key in fields for key in summary_required):
                    summary_fields = fields
                    break
            time.sleep(0.1)
        if summary_fields is None:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="[LOOP-SUMMARY] with the '
                'ITEM-486 fields" deadline=15s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the run summary never carried every field")
        if summary_fields["mouse_path"] not in (
                "OK", "NO_COMPLETION", "NO_REPORT", "NO_LOCAL_INPUT",
                "QUEUE_BACKLOG", "WATCHDOG_REARM", "WATCHDOG_GAVE_UP",
                "POINTER_PROCESSING", "FRAMEBUFFER_RENDER"):
            raise SystemExit(
                "the run summary named an unknown mouse path "
                f"(mouse_path={summary_fields['mouse_path']})")
        if int(summary_fields["mouse_gave_up"]) not in (0, 1):
            raise SystemExit(
                "the run summary published a non-boolean gave-up flag")
        # The worst turn of the whole boot cannot be below the worst turn of any
        # one window: `iter_max_us` is cumulative and `worst_us` is the window's.
        if int(summary_fields["iter_max_us"]) < int(phases_fields["worst_us"]):
            raise SystemExit(
                "the run's worst turn is below a window's worst turn "
                f"(iter_max_us={summary_fields['iter_max_us']} "
                f"worst_us={phases_fields['worst_us']})")
        if int(summary_fields["loops"]) < int(phases_fields["loops"]):
            raise SystemExit(
                "the run summary's iteration count went backwards")
        if int(summary_fields["net_budget_us"]) != 2000:
            raise SystemExit(
                "the run summary did not publish the bounded 2000 us budget")
        if int(summary_fields["mouse_completed"]) < 0 or \
                int(summary_fields["mouse_reports"]) < 0 or \
                int(summary_fields["fb_flush_count"]) < 0:
            raise SystemExit("the run summary published a negative counter")
        print("Run summary: " + summary_lines[-1].strip())
        print("Periodic screen paint lines: "
              + ",".join(str(sample[0]) for sample in paint_samples))
        print("USB input under network load: " + input_text)
        print(f"USB keyboard loss stream: {typed_characters} characters "
              f"echoed whole and in order")

        # ITEM-477 (round 15): the periodic records are still in the durable
        # journal.  `bootlog` dumps the log's own bounded RAM ring, where every
        # line is prefixed `#<sequence> `, so a `#` line carrying a periodic
        # record is the journal's copy and not the live serial stream's.  The
        # records moved to the journal-only funnel so they stop painting the
        # screen; this is the evidence that "off the screen" did not become
        # "out of the journal".  It is typed only now, after every exact echo
        # check, because the dump keeps the console busy for a moment and a
        # keystroke sent into it would read as a lost character.
        #
        # ITEM-518: the dump is one finite reading, and this proof now says so.
        # It waits for `BOOTLOG: shown=N of=M done` before it reads anything,
        # and every progress line must stay inside the window it names - the
        # old dump compared its counter with a `next_sequence` its own output
        # moved, so it never returned and printed `progress=14656 of=64`.  The
        # input diagnostics record is short enough never to need a `-TAIL`, so
        # the bounded-tail half is asserted on the families that do split
        # (`[LOOP-PHASES-TAIL]`, `[PRODUCTION-INPUT-TAIL]`), not on a tail the
        # record does not have.
        for character in "bootlog":
            send_key(character)
        send_key("ret")
        journal_deadline = time.monotonic() + 30.0
        journal_ok = False
        while time.monotonic() < journal_deadline:
            text = serial_text()
            if "*** HFOC KERNEL PANIC ***" in text:
                raise SystemExit(
                    "kernel panic while reading the durable journal")
            if re.search(r"#\d+ \[LOOP-PHASES\]", text) and \
                    re.search(r"#\d+ \[LOOP-PHASES-TAIL\]", text) and \
                    re.search(r"#\d+ \[PRODUCTION-INPUT\]", text) and \
                    re.search(r"#\d+ \[INPUT-DIAG\]", text) and \
                    re.search(r"BOOTLOG: shown=\d+ of=\d+ done", text):
                journal_ok = True
                break
            time.sleep(0.1)
        if not journal_ok:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="boot log dump with the '
                'periodic records and its bounded window" deadline=30s\n')
            sys.stderr.flush()
            raise SystemExit(
                "the durable journal did not carry the periodic records")
        print("Durable journal carries the periodic records")
        # ITEM-518: one dump is a bounded reading.  Every progress line stays
        # inside the window the reading names, and the final line is the one
        # that says the command returned; a dump that chases its own output
        # fails here with the numbers that made it visible.
        for progress_shown, progress_total in re.findall(
                r"BOOTLOG: progress=(\d+) of=(\d+)", serial_text()):
            if int(progress_shown) > int(progress_total):
                raise SystemExit(
                    "the boot log dump ran past its window "
                    f"({progress_shown} of {progress_total})")
        shown = re.findall(r"BOOTLOG: shown=(\d+) of=(\d+) done",
                           serial_text())
        if not shown:
            raise SystemExit("the boot log dump never said it was done")
        if int(shown[-1][0]) > 64 or int(shown[-1][1]) > 64:
            raise SystemExit(
                "the boot log dump left its 64-record staging window "
                f"({shown[-1][0]} of {shown[-1][1]})")
        # ITEM-481 (round 19): every durable record is inside the console
        # mirror's 320-byte bound, the over-bound ones are split into a named
        # head and `[<name>-TAIL]` continuations published in the same tick,
        # and the by-construction guard never had to fire.  The guard's own
        # `[CONSOLE-BOUND]` record is the one thing this proof must never see:
        # its absence is what "no field can be silently cut" means here.
        journal_records = re.findall(r"^#\d+ (.*)$", serial_text(), re.M)
        if any("[CONSOLE-BOUND]" in line for line in journal_records):
            raise SystemExit(
                "the durable journal published a bound violation")
        if any(len(line) > 320 for line in journal_records):
            raise SystemExit(
                "a durable journal record exceeds the 320-byte bound")
        journal_names = set(
            re.match(r"\[[A-Z0-9 _-]+\]", line).group(0)
            for line in journal_records
            if re.match(r"\[[A-Z0-9 _-]+\]", line))
        for part in ("[LOOP-PHASES]", "[LOOP-PHASES-TAIL]",
                     "[PRODUCTION-INPUT]", "[PRODUCTION-INPUT-TAIL]",
                     "[INPUT-DIAG]", "[LOOP-DIAG]"):
            if part not in journal_names:
                raise SystemExit(
                    "the durable journal is missing the record part " + part)
        print("Durable journal records are bounded and split into tails")
print("USB HID keyboard sendkey sequence: kbd ret help ret")
PY
    then
        return 1
    fi

    if ! send_monitor_quit "$monitor_socket"; then
        echo "$label: QEMU monitor quit failed" >&2
        return 1
    fi
    wait "$QEMU_PID"
    QEMU_PID=
    return 0
}

create_hfoc_disk "$DISK_A"
cp "$DISK_A" "$DISK_B"
cp "$DISK_A" "$DISK_A_BEFORE"
truncate -s 16M "$DISK_BLANK"

RUN_BOOT_ALSO=("$UPDATE_LIFECYCLE_RECORD" "$AUDIT_CHAIN_RECORD" "$AUDIT_EXPORT_RECORD")
run_boot nvme_single \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK discovered=1 valid=1 selected=NVME storage=READY mode=NORMAL state=APPLIANCE_STATE_LOADER_OK' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'nvme,drive=hfoc-a,serial=HFOC-A'

run_boot nvme_reload \
    '[APPLIANCE-BOOT] runtime=READY identity=OK config=COMMITTED local_management=READY storage=READY' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'nvme,drive=hfoc-a,serial=HFOC-A'

RUN_BOOT_ALSO=("$UPDATE_LIFECYCLE_RECORD" "$AUDIT_CHAIN_RECORD" "$AUDIT_EXPORT_RECORD")
run_boot single \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK discovered=1 valid=1 selected=AHCI storage=READY mode=NORMAL state=APPLIANCE_STATE_LOADER_OK' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc-a,bus=ide.0'

RUN_BOOT_ALSO=("$UPDATE_LIFECYCLE_RECORD" "$AUDIT_CHAIN_RECORD" "$AUDIT_EXPORT_RECORD")
run_boot reload \
    '[APPLIANCE-BOOT] runtime=READY identity=OK config=COMMITTED local_management=READY storage=READY' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc-a,bus=ide.0'

# ------------------------------------------------------------- ITEM-392 ---
# A machine with no i8042 must boot instead of halting. The production kernel
# used to panic with "PS/2 keyboard initialisation failed" here, which is what
# the 2026-09-12 hardware acceptance run hit on a host with no usable PS/2
# path: every QEMU proof until now emulated a keyboard, so CI could not see it.
# The controller is removed with QEMU's q35 machine option i8042=off (verified
# against the installed QEMU: "i8042=<bool> - Enable/disable Intel 8042 PS/2
# controller emulation"), so the ports 0x60/0x64 read all ones.
#
# The boot must reach the ordinary appliance readiness record with local input
# published as unavailable, and the console and management plane must come up:
# the whole point of ITEM-392 is that losing the local keyboard leaves the
# appliance manageable.
KEYBOARD_ABSENT_MACHINE="$QEMU_MACHINE,i8042=off"
cp "$DISK_A" "$DISK_KBD"
readiness_record='[APPLIANCE-BOOT] runtime=READY identity=OK config=COMMITTED local_management=READY storage=READY local_input=UNAVAILABLE'
keyboard_record='[KBD ] state=UNAVAILABLE reason=CONTROLLER_ABSENT port='
# ITEM-430: the offer this machine makes is the only way its choices exist,
# and it is published after the readiness record the boot is quit on - so the
# markers it is checked for ride along in RUN_BOOT_ALSO and the guest is not
# killed before it has painted them.
RUN_BOOT_ALSO=(
    '[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6'
    'LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING'
    '[1] Status'
    '[6] Recovery'
)
run_boot_machine keyboard_absent "$KEYBOARD_ABSENT_MACHINE" \
    "$readiness_record" \
    -drive "if=none,id=hfoc-kbd,file=$DISK_KBD,format=raw,cache=writeback,file.locking=off" \
    -device 'nvme,drive=hfoc-kbd,serial=HFOC-KBD'
if [[ "$(grep -Fc "$keyboard_record" "$TEST_DIR/keyboard_absent.log")" != 1 ]]; then
    echo "keyboard_absent: the keyboard-unavailable record is missing or not exactly once" >&2
    exit 1
fi
if [[ "$(grep -Fc "$readiness_record" "$TEST_DIR/keyboard_absent.log")" != 1 ]]; then
    echo "keyboard_absent: the appliance readiness record does not publish unavailable local input exactly once" >&2
    exit 1
fi
if ! grep -Eq \
    '\[KBD \] state=UNAVAILABLE reason=(CONTROLLER_ABSENT|CONTROLLER_STUCK|CONFIG_REJECTED|KEYBOARD_ABSENT) port=0x[0-9A-F]+ config=0x[0-9A-F]+' \
    "$TEST_DIR/keyboard_absent.log"; then
    echo "keyboard_absent: the keyboard record does not name a classified reason" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$TEST_DIR/keyboard_absent.log"; then
    echo "keyboard_absent: the boot panicked without a PS/2 controller" >&2
    exit 1
fi
for marker in \
    '[PRODUCTION-MGMT-BIND] scope=' \
    'HFOC OS 1.0.0 INITIALISED' \
    'LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)' \
    '[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6' \
    'LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING' \
    '[1] Status' \
    '[6] Recovery' \
    'hfoc> '; do
    if ! grep -Fq "$marker" "$TEST_DIR/keyboard_absent.log"; then
        echo "keyboard_absent: the boot without a PS/2 controller did not publish: $marker" >&2
        exit 1
    fi
done
# ITEM-430: the machine that cannot type is offered its own menu, at once and
# read-only.  The offer is the only way the choices exist on this machine, so
# it is required here rather than left to the decision's host test.
if grep -Fq '[LOCAL-MENU] offer cancelled' "$TEST_DIR/keyboard_absent.log"; then
    echo "keyboard_absent: the read-only offer was cancelled by an input that does not exist" >&2
    exit 1
fi
# ITEM-395: with neither PS/2 nor a USB HID keyboard the record must name the
# absent path, so an operator reads NONE instead of guessing.
if ! grep -Fq '[KBD ] state=UNAVAILABLE reason=CONTROLLER_ABSENT port=' \
        "$TEST_DIR/keyboard_absent.log" ||
    ! grep -Fq ' source=NONE ps2=CONTROLLER_ABSENT usb=CONTROLLER_ABSENT' \
        "$TEST_DIR/keyboard_absent.log"; then
    echo "keyboard_absent: the record does not publish source=NONE" >&2
    exit 1
fi
echo "Production boot without a PS/2 controller: PASS"
grep -F "$readiness_record" "$TEST_DIR/keyboard_absent.log"
grep -F '[KBD ] state=UNAVAILABLE' "$TEST_DIR/keyboard_absent.log"

# ------------------------------------------------------------- ITEM-395 ---
# The local console is the documented break-glass path (ITEM-385) and an
# interactive install confirmation (ITEM-394) depends on it, so a machine with
# no PS/2 controller must be drivable locally. The kernel brings up the xHCI
# controller, finds the HID boot-protocol keyboard behind it and feeds the
# console the same events the PS/2 driver produces. This phase boots the
# production ISO with i8042=off and the keyboard on an emulated xHCI bus, types
# "kbd" and "help" through it and requires the console to answer both.
cp "$DISK_A" "$DISK_USB"
usb_readiness_record='[APPLIANCE-BOOT] runtime=READY identity=OK config=COMMITTED local_management=READY storage=READY local_input=AVAILABLE input_source=USB_HID'
usb_keyboard_record='[KBD ] state=READY reason=USB_HID'
usb_controller_record='[XHCI] state=READY reason=OK'
# ITEM-467: the same phase now carries the owner's combined report under load -
# a stream of ARP requests floods the production receive path while 300+
# characters are typed through the USB keyboard and the pointer is moved and
# clicked on the same controller.  The peer keeps the receive ring busy; the
# machine's own `[PRODUCTION-INPUT]` record has to show no dropped report, a
# bounded service gap, a bounded loop turn and a pointer that moved and
# clicked.  The port is QEMU's listener, chosen here so the peer can reach it.
NETLOAD_PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
python3 "$ROOT/scripts/hfoc-qemu-netload-peer.py" \
    --port "$NETLOAD_PORT" --duration 240 --interval-ms 4 \
    >"$TEST_DIR/netload-peer.log" 2>&1 &
NETLOAD_PEER_PID=$!
# The production virtio-net driver negotiates a fixed feature profile, so the
# emulated device must offer exactly the profile the production-network proof
# uses (and the fixed MAC the peer's DHCP lease names).
NETLOAD_VIRTIO='virtio-net-pci-non-transitional,netdev=netload,ioeventfd=off,'
NETLOAD_VIRTIO+='mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,'
NETLOAD_VIRTIO+='mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,'
NETLOAD_VIRTIO+='ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,'
NETLOAD_VIRTIO+='guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,'
NETLOAD_VIRTIO+='guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,'
NETLOAD_VIRTIO+='host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,'
NETLOAD_VIRTIO+='guest_uso6=off,guest_announce=off,hash=off,rss=off,'
NETLOAD_VIRTIO+='guest_rsc_ext=off,vectors=0,x-disable-pcie=on'
if ! run_usb_hid_phase usb_hid_keyboard "$QEMU_MACHINE,i8042=off,usb=off" \
    "$DISK_USB" 1 \
    -device 'qemu-xhci,id=xhci' \
    -device 'usb-kbd,bus=xhci.0' \
    -device 'usb-mouse,bus=xhci.0' \
    -netdev "socket,id=netload,listen=127.0.0.1:$NETLOAD_PORT" \
    -device "$NETLOAD_VIRTIO"; then
    if [[ -n "$NETLOAD_PEER_PID" ]]; then
        kill -TERM "$NETLOAD_PEER_PID" 2>/dev/null || true
        wait "$NETLOAD_PEER_PID" 2>/dev/null || true
        NETLOAD_PEER_PID=
    fi
    echo "usb_hid_keyboard: the console did not answer the USB HID keyboard" >&2
    exit 1
fi
if [[ -n "$NETLOAD_PEER_PID" ]]; then
    kill -TERM "$NETLOAD_PEER_PID" 2>/dev/null || true
    wait "$NETLOAD_PEER_PID" 2>/dev/null || true
    NETLOAD_PEER_PID=
fi
for record in "$usb_readiness_record" "$usb_keyboard_record" \
    "$usb_controller_record"; do
    if [[ "$(grep -Fc "$record" "$TEST_DIR/usb_hid_keyboard.log")" != 1 ]]; then
        echo "usb_hid_keyboard: the record is missing or not exactly once: $record" >&2
        exit 1
    fi
done
# ITEM-458: the loss burst's own evidence.  The exact echo of the three typed
# lines was required inside the phase; here the counters it left are required
# to agree: nothing was refused, the worst wait between two reports is inside
# the budget, and the queued reports really were printed (so the burst's keys
# reached the console, not only the echo).
usb_loss_log=$TEST_DIR/usb_hid_keyboard.log
usb_gap_ms_max=$(grep -o 'GAP_MS_MAX=[0-9]*' "$(serial_view "$usb_loss_log")" | tail -1 | cut -d= -f2)
if [[ -z $usb_gap_ms_max ]]; then
    echo "usb_hid_keyboard: the kbd command published no GAP_MS_MAX" >&2
    exit 1
fi
if ! grep -Eq 'KEYS=[0-9]+ DROPPED=0 GAP_MS_MAX=[0-9]+' "$(serial_view "$usb_loss_log")"; then
    echo "usb_hid_keyboard: a report was refused by the console queue, or the loss counters are missing" >&2
    grep -F 'USB HID: ' "$usb_loss_log" | tail -3 >&2 || true
    exit 1
fi
if (( usb_gap_ms_max > 500 )); then
    echo "usb_hid_keyboard: the worst service gap ${usb_gap_ms_max} ms is outside the 500 ms budget" >&2
    exit 1
fi
# ITEM-459: the exact echo of the punctuation line and the single
# unmapped-usage record are required inside the phase, against the same
# IRQ-stripped stream the other markers are read from; here the log is only
# required to carry the record's marker for a reader.
if ! grep -Fq '[KBD ] unmapped usage=' "$(serial_view "$usb_loss_log")"; then
    echo "usb_hid_keyboard: the unmapped-usage record never reached the log" >&2
    exit 1
fi
usb_reports_printed=$(grep -Fc 'appliance v=' "$(serial_view "$usb_loss_log")" || true)
if (( usb_reports_printed < 6 )); then
    echo "usb_hid_keyboard: only ${usb_reports_printed} of the queued appliance reports were printed" >&2
    exit 1
fi
echo "USB HID keyboard loss: 308 characters echoed whole, dropped=0, gap_ms_max=${usb_gap_ms_max}ms"

for marker in \
    'hid=BOOT_KEYBOARD' \
    'source=USB_HID ps2=CONTROLLER_ABSENT' \
    'LOCAL INPUT: ENABLED (USB HID KEYBOARD)' \
    'LOCAL INPUT: SOURCE=USB_HID USB=READY SPEED=HIGH VID=0x627 PID=0x1' \
    'USB HID: REPORTS=' \
    'COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP ABOUT' \
    'hfoc> '; do
    if ! grep -Fq "$marker" "$TEST_DIR/usb_hid_keyboard.log"; then
        echo "usb_hid_keyboard: the boot did not publish: $marker" >&2
        exit 1
    fi
done
if grep -Fq '*** HFOC KERNEL PANIC ***' "$TEST_DIR/usb_hid_keyboard.log"; then
    echo "usb_hid_keyboard: the boot panicked" >&2
    exit 1
fi
if grep -Fq 'LOCAL INPUT: DISABLED' "$TEST_DIR/usb_hid_keyboard.log"; then
    echo "usb_hid_keyboard: the console still reports disabled local input" >&2
    exit 1
fi
# The keyboard must not only be enumerated: the report counter has to move,
# which is what proves the four keystrokes of "kbd" and "help" were decoded
# from the controller's interrupt IN transfers rather than echoed by something
# else.
#
# ITEM-436: the boot record names the interface the driver bound, not only the
# device it found. The 2026-09-17 hardware run's keyboard is a Logitech
# wireless receiver that answers with more than one HID interface, and
# `max_packet=12` did not say which of them was selected; the record now
# carries the interface number, its descriptor protocol, the endpoint address
# and the interval and packet size the endpoint context was built from.
#
# ITEM-438: the DCI the reports arrive on and the descriptor's own bInterval
# join them, because for a low- or full-speed endpoint the Interval field is
# the *exponent* of the period the device asked for (xHCI 1.2 section 6.2.3.6)
# and a record that prints only one of the two numbers cannot be read.
if ! grep -Eq '\[KBD \] state=READY reason=USB_HID .*source=USB_HID .*iface=[0-9]+ protocol=1 ep=0x[0-9A-F]{2} dci=[0-9]+ interval=[0-9]+ binterval=[0-9]+ max_packet=8' \
        "$TEST_DIR/usb_hid_keyboard.log"; then
    echo "usb_hid_keyboard: the boot record does not name the keyboard binding" >&2
    grep -F '[KBD ] state=READY' "$TEST_DIR/usb_hid_keyboard.log" >&2 || true
    exit 1
fi
# ITEM-436: the live counters name the other half - a keyboard that is bound
# but not answering. Every completion must have carried a whole 8-byte boot
# report (REFUSED=0) and LAST is the length of the most recent one, so a
# device whose reports the decoder refuses is a published number rather than
# the same three characters as a silent one.
#
# ITEM-438: and KEYS is what those reports carried. `REPORTS>0 KEYS=0` is a
# keyboard that answered every poll with an idle report; KEYS>0 is a keyboard
# that typed, which is what the four keystrokes of "kbd" have to produce.
if ! grep -Eq 'USB HID: REPORTS=[1-9][0-9]* TRANSFERS=[1-9][0-9]* ERRORS=0 REFUSED=0 LAST=8 KEYS=[1-9][0-9]*' \
        "$TEST_DIR/usb_hid_keyboard.log"; then
    echo "usb_hid_keyboard: no USB HID boot report was decoded, or a completion carried a length other than the boot report's, or the reports carried no key" >&2
    grep -F 'USB HID: ' "$TEST_DIR/usb_hid_keyboard.log" >&2 || true
    exit 1
fi
echo "Production boot with a USB HID keyboard on xHCI: PASS"
grep -F "$usb_readiness_record" "$TEST_DIR/usb_hid_keyboard.log"
grep -F "$usb_keyboard_record" "$TEST_DIR/usb_hid_keyboard.log"
grep -F 'LOCAL INPUT: SOURCE=USB_HID' "$TEST_DIR/usb_hid_keyboard.log" | tail -1


# ------------------------------------------------------------- ITEM-406 ---
# The acceptance host has three xHCI controllers: the first one in PCI order
# carries a Logitech wireless receiver and no keyboard, the K120 keyboard is on
# the *second* controller, root port 2, as a low-speed USB 1.1 device, and a
# third controller carries nothing of interest. The first version of this driver
# brought up controllers[0] only, so a machine with the keyboard on the second
# controller published UNAVAILABLE - the hardware run that opened this ITEM.
#
# The phase puts two controllers at fixed PCI slots (0:16.0 and 0:17.0, so the
# scan order is the one the assertions name) and a device that is not a boot
# keyboard on the first one: QEMU's usb-mouse is a HID boot mouse (class 3,
# subclass 1, protocol 2), which the descriptor walk must classify as "not a
# keyboard" - and that classification must NOT end the scan. The keyboard on the
# second controller has to be found, typed through and serviced.
#
# What this proves: the scan visits every controller and does not stop at the
# first device it can address. What it cannot prove: the low-speed path (QEMU's
# usb-kbd is a high-speed device on an xHCI USB2 port, and no QEMU USB device
# offers low speed), the PORTSC write semantics of the acceptance host's AMD
# controllers (QEMU's model ignores writes to PED, which is exactly the defect
# the neutral write in kernel/src/xhci.c removes) and the real receiver's own
# descriptors. Those need the next hardware boot.
cp "$DISK_A" "$DISK_USB_SECOND"
run_usb_hid_phase usb_hid_keyboard_second_controller \
    "$QEMU_MACHINE,i8042=off,usb=off" \
    "$DISK_USB_SECOND" 0 \
    -device 'qemu-xhci,id=xhci0,bus=pcie.0,addr=0x10' \
    -device 'qemu-xhci,id=xhci1,bus=pcie.0,addr=0x11' \
    -device 'usb-mouse,bus=xhci0.0' \
    -device 'usb-kbd,bus=xhci1.0' || {
    echo "usb_hid_keyboard_second_controller: the console did not answer the USB HID keyboard on the second controller" >&2
    exit 1
}
usb_second_log=$TEST_DIR/usb_hid_keyboard_second_controller.log
for record in "$usb_readiness_record" "$usb_keyboard_record" "$usb_controller_record"; do
    if [[ "$(grep -Fc "$record" "$(serial_view "$usb_second_log")")" != 1 ]]; then
        echo "usb_hid_keyboard_second_controller: the record is missing or not exactly once: $record" >&2
        exit 1
    fi
done
# The scan must have visited both controllers, and the first one must have
# published a rejection that names the port and the stage. The port number is
# the controller's own (QEMU numbers the four USB2 ports of qemu-xhci 5..8, so
# the first port of an emulated controller is PORTSC port 5), which is why the
# assertion pins the shape and the stage rather than one port number.
#
# ITEM-452: every visit publishes a record that names its controller
# (`pci=bb:dd.f`) and the machine's controller count, and the walk does not
# stop at the keyboard - so both PCI addresses must appear.  A silent keyboard
# candidate adds the re-bind's record for the same controller, which is why the
# assertion counts *addresses*, not lines.
if ! grep -aqP '^\[XHCI\] state=.* pci=[0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F] controllers=2 ' "$(serial_view "$usb_second_log")"; then
    echo "usb_hid_keyboard_second_controller: a controller record does not name itself and the machine" >&2
    grep -aF '[XHCI] state=' "$usb_second_log" >&2 || true
    exit 1
fi
if [[ "$(grep -aoP '(?<= pci=)[0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F](?= )' "$(serial_view "$usb_second_log")" | sort -u | wc -l)" != 2 ]]; then
    echo "usb_hid_keyboard_second_controller: the scan did not publish both controllers the machine has" >&2
    grep -aF '[XHCI] state=' "$usb_second_log" >&2 || true
    exit 1
fi
if ! grep -Eq '\[XHCI\] state=UNAVAILABLE reason=NOT_A_KEYBOARD detail=P[0-9]+_CONFIG_PARSE_NO_BOOT_KEYBOARD( |$)' "$(serial_view "$usb_second_log")"; then
    echo "usb_hid_keyboard_second_controller: the first controller did not publish its specific rejection" >&2
    grep -F '[XHCI] state=' "$usb_second_log" >&2 || true
    exit 1
fi
rejection_line=$(grep -nF '[XHCI] state=UNAVAILABLE reason=NOT_A_KEYBOARD' "$(serial_view "$usb_second_log")" | head -1 | cut -d: -f1)
ready_line=$(grep -nF '[XHCI] state=READY reason=OK' "$(serial_view "$usb_second_log")" | head -1 | cut -d: -f1)
if [[ -z "$rejection_line" || -z "$ready_line" || "$rejection_line" -ge "$ready_line" ]]; then
    echo "usb_hid_keyboard_second_controller: the scan did not continue past the first controller" >&2
    exit 1
fi
for marker in \
    'hid=BOOT_KEYBOARD' \
    '[USB ] pci=' \
    ' role=KEYBOARD state=BOUND' \
    ' vid=0x' \
    'source=USB_HID ps2=CONTROLLER_ABSENT' \
    'LOCAL INPUT: ENABLED (USB HID KEYBOARD)' \
    'LOCAL INPUT: SOURCE=USB_HID USB=READY' \
    'USB HID: REPORTS=' \
    'hfoc> '; do
    if ! grep -Fq "$marker" "$(serial_view "$usb_second_log")"; then
        echo "usb_hid_keyboard_second_controller: the boot did not publish: $marker" >&2
        exit 1
    fi
done
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$usb_second_log")"; then
    echo "usb_hid_keyboard_second_controller: the boot panicked" >&2
    exit 1
fi
if grep -Fq 'LOCAL INPUT: DISABLED' "$(serial_view "$usb_second_log")"; then
    echo "usb_hid_keyboard_second_controller: the console still reports disabled local input" >&2
    exit 1
fi
if ! grep -Eq 'USB HID: REPORTS=[1-9][0-9]* TRANSFERS=[1-9][0-9]* ERRORS=0' "$(serial_view "$usb_second_log")"; then
    echo "usb_hid_keyboard_second_controller: no USB HID boot report was decoded" >&2
    exit 1
fi
echo "Production boot with the USB HID keyboard on the second xHCI controller: PASS"
grep -F "$usb_readiness_record" "$(serial_view "$usb_second_log")"
grep -F '[XHCI] state=UNAVAILABLE reason=NOT_A_KEYBOARD' "$(serial_view "$usb_second_log")"
grep -F "$usb_keyboard_record" "$(serial_view "$usb_second_log")"


# ------------------------------------------------------------- ITEM-407 ---
# The second hardware run published `state=UNAVAILABLE reason=NO_DEVICE
# detail=NO_CONNECTED_DEVICE` for the controller the keyboard is physically on,
# and the record said nothing about *why* every port reported no connect: a
# controller that was not running, a port whose power was never on, a register
# window that is not PORTSC - or a port with genuinely nothing on it - all look
# the same from the reason alone. The acceptance host has no serial capture, so
# the one photograph of the screen had to be enough, which is what this phase
# pins on the host: a controller with **no device at all** must publish its port
# evidence, and the values must be the ones the scan actually read.
#
# What this proves: the record carries the evidence, the controller is running
# when the ports are read (`usbcmd` run/stop set, `usbsts` host-controller-halt
# clear), the ports were powered by the scan (`pp` all ones - QEMU's xhci model
# reports PORTSC.PP set on every empty port, and this phase is what forbids a
# claim write that loses it) and the first port value is exactly "powered, no
# device connected". What it cannot prove: the same values on the acceptance
# host's AMD controllers, which is the next hardware boot.
cp "$DISK_A" "$DISK_USB_EMPTY"
usb_empty_readiness_record='[APPLIANCE-BOOT] runtime=READY identity=OK config=COMMITTED local_management=READY storage=READY local_input=UNAVAILABLE'
run_boot_machine usb_hid_empty_controller \
    "$QEMU_MACHINE,i8042=off,usb=off" \
    "$usb_empty_readiness_record" \
    -device 'qemu-xhci,id=xhci-empty' \
    -drive "if=none,id=hfoc-empty,file=$DISK_USB_EMPTY,format=raw,cache=writeback,file.locking=off" \
    -device 'nvme,drive=hfoc-empty,serial=HFOC-EMPTY' || {
    echo "usb_hid_empty_controller: the boot with an empty xHCI controller did not reach a readiness record" >&2
    exit 1
}
usb_empty_log=$TEST_DIR/usb_hid_empty_controller.log
if [[ "$(grep -Fc '[XHCI] state=' "$(serial_view "$usb_empty_log")")" != 1 ]]; then
    echo "usb_hid_empty_controller: the scan did not publish exactly one record" >&2
    grep -F '[XHCI] state=' "$usb_empty_log" >&2 || true
    exit 1
fi
empty_record=$(grep -F '[XHCI] state=' "$(serial_view "$usb_empty_log")")
echo "usb_hid_empty_controller: $empty_record"
# The serial log is CRLF-terminated, so the record ends with "\r\n" and the
# shape is anchored with \r?$: without it a "$"-anchored pattern never matches
# a log written by QEMU's file backend.
if ! grep -aP \
    '\[XHCI\] state=UNAVAILABLE reason=NO_DEVICE detail=NO_CONNECTED_DEVICE pci=[0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F] controllers=[0-9]+ taken=(YES|NO) bar=0x[0-9A-F]+ ports=[0-9]+ slots=[0-9]+ slot_en=[0-9]+ ctx=(32|64) vendor=0x[0-9A-F]+ device=0x[0-9A-F]+ usbcmd=[0-9A-F]+ usbsts=[0-9A-F]+ portsc=(0x[0-9A-F]+,)*0x[0-9A-F]+ pp=[0-9A-F]+ ppoff=[0-9]+\r?$' \
    "$(serial_view "$usb_empty_log")"; then
    echo "usb_hid_empty_controller: the record does not carry the port evidence in the pinned shape" >&2
    echo "$empty_record" >&2
    exit 1
fi
python3 - "$(serial_view "$usb_empty_log")" <<'PY'
import re
import sys

line = ""
with open(sys.argv[1], "r", encoding="ascii", errors="replace") as handle:
    for text in handle:
        if text.startswith("[XHCI] state="):
            # The serial log is CRLF-terminated.
            line = text.rstrip("\r\n")
if not line:
    raise SystemExit("usb_hid_empty_controller: no [XHCI] record")

def field(name):
    match = re.search(r" " + name + r"=([^ ]+)", line)
    if match is None:
        raise SystemExit(f"usb_hid_empty_controller: the record has no {name}=")
    return match.group(1)

ports = int(field("ports"))
portsc = [int(value, 16) for value in field("portsc").split(",")]
cmd = int(field("usbcmd"), 16)
sts = int(field("usbsts"), 16)
pp = int(field("pp"), 16)

if ports < 1:
    raise SystemExit("usb_hid_empty_controller: the controller reports no port")
# The record names at most two ports (the one-line console budget); a
# controller with fewer ports names all of them.
if len(portsc) != min(ports, 2):
    raise SystemExit(
        f"usb_hid_empty_controller: {len(portsc)} PORTSC values for "
        f"{ports} ports")
# xHCI 1.1 section 4.2: RS (bit 0) set and HCH (bit 0 of USBSTS) clear, i.e.
# the controller was running while its ports were read.
if (cmd & 0x1) == 0:
    raise SystemExit(
        f"usb_hid_empty_controller: the controller is halted ({cmd:#x})")
if (sts & 0x1) != 0:
    raise SystemExit(
        f"usb_hid_empty_controller: USBSTS.HCH is set ({sts:#x})")
# PORTSC: PP (bit 9) is what the scan asserted, CCS (bit 0) must be clear
# because the controller really has no device, and the speed field (bits
# 13:10) is only valid with CCS set.
for index, value in enumerate(portsc):
    if (value & 0x200) == 0:
        raise SystemExit(
            f"usb_hid_empty_controller: port {index + 1} is unpowered "
            f"({value:#010x})")
    if (value & 0x1) != 0:
        raise SystemExit(
            f"usb_hid_empty_controller: port {index + 1} reports a connected "
            f"device ({value:#010x})")
if pp != (1 << len(portsc)) - 1:
    raise SystemExit(
        f"usb_hid_empty_controller: pp={pp:#x} does not match the published "
        f"ports")
print(
    f"usb_hid_empty_controller: portsc={field('portsc')} usbcmd={cmd:#010x} "
    f"usbsts={sts:#010x} pp={pp:#x}")
PY
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$usb_empty_log")"; then
    echo "usb_hid_empty_controller: the boot panicked" >&2
    exit 1
fi
if ! grep -Fq 'LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)' "$(serial_view "$usb_empty_log")"; then
    echo "usb_hid_empty_controller: the console does not report disabled local input" >&2
    exit 1
fi
echo "Production boot with an empty xHCI controller: PASS"
grep -F '[XHCI] state=UNAVAILABLE reason=NO_DEVICE' "$(serial_view "$usb_empty_log")"


# ITEM-376: the durable update record must be published on the first boot and
# reloaded on the next one, and no boot may publish a degraded lifecycle.
update_record=$UPDATE_LIFECYCLE_RECORD
for label in nvme_single single reload; do
    if [[ "$(grep -Fc "$update_record" "$TEST_DIR/$label.log")" != 1 ]]; then
        echo "$label: durable update record is missing or not exactly once" >&2
        exit 1
    fi
done
if grep -Fq '[UPDATE-ATOMICITY] update_lifecycle v=1 state=UNAVAILABLE' \
        "$TEST_DIR/single.log"; then
    echo "single: the durable update record was not bound" >&2
    exit 1
fi

# ITEM-377: ordinary boot must publish the verified hash-chained audit window
# and a completed external export, and it may not publish a tampered view.
audit_chain=$AUDIT_CHAIN_RECORD
for label in nvme_single single reload; do
    if [[ "$(grep -Fc "$audit_chain" "$TEST_DIR/$label.log")" != 1 ]]; then
        echo "$label: verified audit chain is missing or not exactly once" >&2
        exit 1
    fi
    if ! grep -Fq 'audit_export v=1 state=COMPLETE' \
            "$TEST_DIR/$label.log"; then
        echo "$label: external audit export did not complete" >&2
        exit 1
    fi
done
for label in nvme_single single reload; do
    if grep -Fq 'audit_chain v=1 state=TAMPERED' "$TEST_DIR/$label.log"; then
        echo "$label: ordinary boot published a tampered audit chain" >&2
        exit 1
    fi
    if grep -Fq 'audit_chain v=1 state=DEGRADED' "$TEST_DIR/$label.log"; then
        echo "$label: ordinary boot published a degraded audit chain" >&2
        exit 1
    fi
done

run_boot degraded \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_NO_BACKEND discovered=0 valid=0 selected=NONE storage=DEGRADED mode=DEGRADED state=NOT_LOADED'

run_boot ambiguous \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND discovered=2 valid=2 selected=NONE storage=UNAVAILABLE mode=FAIL_CLOSED state=NOT_LOADED' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc-a,bus=ide.0' \
    -drive "if=none,id=hfoc-b,file=$DISK_B,format=raw,cache=writeback,file.locking=off" \
    -device 'nvme,drive=hfoc-b,serial=HFOC-B'

run_boot single_ahci_multi_controller \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK discovered=2 valid=1 selected=AHCI storage=READY mode=NORMAL state=APPLIANCE_STATE_LOADER_OK' \
    -device 'ich9-ahci,id=hfoc-ahci-b' \
    -drive "if=none,id=hfoc-a,file=$DISK_A,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc-a,bus=ide.0' \
    -drive "if=none,id=hfoc-blank,file=$DISK_BLANK,format=raw,cache=none,file.locking=off" \
    -device 'ide-hd,drive=hfoc-blank,bus=hfoc-ahci-b.0'

# ITEM-376: ordinary boot now owns exactly one durable write -- the initial
# update record in the RECOVERY_METADATA store.  Every other byte of the
# state partition must still be untouched, and the record must really be
# there, so this is a strict superset of the previous no-write assertion.
python3 - "$DISK_A_BEFORE" "$DISK_A" <<'RECORD_CHECK'
import pathlib
import sys

before = pathlib.Path(sys.argv[1]).read_bytes()
after = pathlib.Path(sys.argv[2]).read_bytes()
block_size = 512
first_lba = 34
recovery_metadata_offset = 384                  # layout entry 7 of 8
record_offset = recovery_metadata_offset + 1    # block 0 is the header
record_start = (first_lba + record_offset) * block_size
record_end = record_start + 2 * block_size      # the two record copies

if len(before) != len(after):
    raise SystemExit("production storage boot: disk size changed")
for index in range(len(before)):
    if record_start <= index < record_end:
        continue
    if before[index] != after[index]:
        raise SystemExit(
            "production storage boot: unexpected write at byte "
            f"{index} outside the update record range")
record = after[record_start:record_end]
if b"HFOCUPDT" not in record:
    raise SystemExit(
        "production storage boot: the update record was not published")
if after[record_start:record_start + 2] == before[record_start:record_start + 2]:
    raise SystemExit(
        "production storage boot: the update record was not written")
print("Production storage boot: only the update record changed")
RECORD_CHECK
echo "Production storage boot QEMU test: OK"
