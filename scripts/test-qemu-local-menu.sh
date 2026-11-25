#!/usr/bin/env bash
# ITEM-419: prove the machine's own menu acts.
#
# The appliance has rendered `[1] Status` … `[6] Recovery` on its own screen
# since ITEM-344 and nothing could select one: the machine offered a menu that
# did not respond, and the only way to reboot or power it off was a serial port
# the acceptance host does not have.  This proof does what the operator does -
# types `menu`, walks the selection with the arrow keys, presses ENTER - and
# requires the machine to do what the menu said it would do, through the reset
# path ITEM-411 proved.  A menu that renders but does not act fails here.
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

if [[ "${HFOC_LOCAL_MENU_TEST_INNER:-0}" != 1 ]]; then
    # ITEM-426/427: the wrapper and the deadlines inside it are one contract.
    # This proof walks the menu in seven waits of 50 s each (boot prompt, the
    # menu appearing, the selection moving, the letter path, the selection
    # again, the reboot record) plus a 30 s wait for the machine to leave, so a
    # 90 s wrapper would kill a starved-but-healthy guest at exit 124 with no
    # class record for the phase to retry.  The wrapper dominates that sum.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_LOCAL_MENU_TEST_TIMEOUT:-360}s" \
        env HFOC_LOCAL_MENU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
QEMU_MEMORY=${QEMU_MEMORY:-512}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Local menu test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "local menu test needs the declared image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_PID=

cleanup() {
    local status=$?

    trap - EXIT INT TERM
    set +e
    if [[ -n $QEMU_PID ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null
    fi
    if (( status != 0 )); then
        printf 'local menu test artifacts: %s\n' "$TEST_DIR" >&2
        [[ -f $SERIAL_LOG ]] && tail -n 30 "$SERIAL_LOG" >&2
    else
        rm -rf "$TEST_DIR"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot >"$TEST_DIR/qemu.log" 2>&1 &
QEMU_PID=$!

python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" "$QEMU_PID" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_socket = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
qemu_pid = int(sys.argv[4])

MARKER_DEADLINE = 50.0
periodic = re.compile(
    r"\[LAPIC-TIMER\] irq vector=0x[0-9A-Fa-f]+(?:\r?\n)?|"
    r"\[LAPIC-TIMER\] irq lapic_eoi(?:\r?\n)?|"
    r"\[IOAPIC\] irq[0-9]+ vector=0x[0-9A-Fa-f]+(?:\r?\n)?|"
    r"\[IOAPIC\] irq[0-9]+ lapic_eoi(?:\r?\n)?")


def serial_text():
    try:
        return periodic.sub(
            "", serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def machine_alive():
    stat = pathlib.Path(f"/proc/{qemu_pid}/stat")
    try:
        text = stat.read_text()
    except OSError:
        return False
    fields = text.rsplit(")", 1)[-1].split()
    return bool(fields) and fields[0] != "Z"


def wait_for(needle, deadline, description):
    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("kernel panic before " + description)
        if needle in text:
            return text
        time.sleep(0.05)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT '
        'marker="' + description + '" '
        'deadline=' + str(int(MARKER_DEADLINE)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + description)


deadline = time.monotonic() + MARKER_DEADLINE
wait_for("hfoc> ", deadline, "boot prompt")

while not monitor_socket.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    monitor.connect(str(monitor_socket))

    def receive_prompt():
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
        return b"".join(chunks).decode("utf-8", errors="replace")

    def send_keys(keys):
        for key in keys:
            monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
            receive_prompt()
            time.sleep(0.12)

    def type_word(word):
        for character in word:
            send_keys([character])
        send_keys(["ret"])

    receive_prompt()

    # What an operator does: open the menu, look at it, walk to Reboot, run it.
    type_word("menu")
    text = wait_for("[LOCAL-MENU]", time.monotonic() + MARKER_DEADLINE,
                    "the local menu on the machine's console")
    for entry in ("[1] Status", "[4] Reboot", "[5] Shut down",
                  "UP/DOWN select, ENTER run, Q leave"):
        if entry not in text:
            raise SystemExit(
                "the menu does not offer " + entry + ": " + text[-400:])

    # Reboot is the fourth entry: three Downs from Status.
    send_keys(["down", "down", "down"])
    wait_for("> [4] Reboot", time.monotonic() + MARKER_DEADLINE,
             "the selection to reach Reboot")

    # A letter runs its own entry, which is the path an operator uses when the
    # arrow keys are not the fastest way: `s` is Status.
    send_keys(["s"])
    text = wait_for("[LOCAL-MENU] run action=status",
                    time.monotonic() + MARKER_DEADLINE,
                    "the Status entry to run")
    if "LOCAL INPUT:" not in text and "APPLIANCE" not in text:
        raise SystemExit(
            "the Status entry ran but printed nothing: " + text[-400:])

    # ENTER runs the selection, and the selection is Reboot.
    send_keys(["s", "down", "down", "down"])
    wait_for("> [4] Reboot", time.monotonic() + MARKER_DEADLINE,
             "the selection to return to Reboot")
    send_keys(["ret"])

    text = wait_for("[LOCAL-MENU] reboot now",
                    time.monotonic() + MARKER_DEADLINE,
                    "the machine to take the menu's reboot")
    if "[PLATFORM-RESET] plan " not in text:
        raise SystemExit(
            "the reboot did not go through the ITEM-411 reset plan: " +
            text[-400:])

    # QEMU runs with -no-reboot, so the machine the menu rebooted ends the run.
    leave_deadline = time.monotonic() + 30.0
    while machine_alive() and time.monotonic() < leave_deadline:
        time.sleep(0.05)
    if machine_alive():
        raise SystemExit("the machine did not reboot when the menu said it did")
    print("local menu: the menu rendered, selected and rebooted the machine")
PY
RC=$?
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
(( RC == 0 )) || exit "$RC"

echo "QEMU local menu test: OK"
