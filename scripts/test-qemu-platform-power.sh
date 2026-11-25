#!/usr/bin/env bash
# ITEM-418: prove the two power actions an operator can ask the machine for.
#
# The console is driven through the keyboard, exactly as an operator would: the
# proof waits for the boot prompt, sends `shutdown`, and requires the module's
# own records - the plan, the mechanism, the PM1 write - and then requires QEMU
# to leave on its own, which is what a platform that honours S5 does.  A machine
# that keeps running would leave the `result=STILL_RUNNING` record behind, and
# the proof fails on that too: "powered off" is never reported from a machine
# that is still there.
#
# The second scenario is the same console command for `reboot`, which runs the
# reset chain ITEM-411 proved on hardware; QEMU exits under `-no-reboot`.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp grep; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PLATFORM_POWER_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PLATFORM_POWER_TEST_TIMEOUT:-180}s" \
        env HFOC_PLATFORM_POWER_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Platform power test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Platform power test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Platform power test must not set QEMU_CPU" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Platform power test needs an image: $IMAGE" >&2
    exit 1
}
echo "QEMU platform power baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
QEMU_PID=
FAILURE_MESSAGE=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        printf '%s\n' "${FAILURE_MESSAGE:-platform power proof failed}" >&2
        for log in "$TEST_DIR"/*.log; do
            [[ -f "$log" ]] || continue
            echo "----- ${log##*/} (tail) -----" >&2
            tail -n 20 "$log" >&2
        done
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null
    fi
    rm -rf "$TEST_DIR"
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

trap 'exit 124' TERM
trap 'exit 130' INT

# wait_for_exit: QEMU leaves by itself when the guest powers off (no
# `-no-shutdown`) or resets (with `-no-reboot`); a machine that stays is the
# failure this waits out.
wait_for_exit() {
    local deadline=$((SECONDS + 20))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
    return 0
}

# type_action SERIAL SOCKET ACTION: wait for the prompt, then type the command
# one key at a time, the way an operator's keyboard would.
type_action() {
    local serial=$1
    local socket=$2
    local action=$3

    python3 - "$serial" "$socket" "$action" \
        "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
action = sys.argv[3]
deadline = time.monotonic() + 60.0

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


while time.monotonic() < deadline:
    text = serial_text()
    if "hfoc> " in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic before the power command")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so - the class the
    # acceptance gate retries, never an assertion.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT '
        'marker="boot prompt" '
        'deadline=60s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for the boot prompt")

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
        time.sleep(0.05)

    receive_prompt()
    for character in action:
        send_key(character)
    send_key("ret")
PY
}

# run_scenario ACTION: one boot, one console command, and the records and the
# exit that command has to produce.
run_scenario() {
    local action=$1
    local serial=$TEST_DIR/$action.log
    local monitor_log=$TEST_DIR/$action-monitor.log
    local qemu_log=$TEST_DIR/$action-qemu.log
    local socket=$TEST_DIR/$action.sock

    : >"$serial"
    : >"$monitor_log"
    : >"$qemu_log"

    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -cdrom "$IMAGE" \
        -serial "file:$serial" \
        -monitor "unix:$socket,server,nowait" \
        -no-reboot >"$qemu_log" 2>&1 &
    QEMU_PID=$!

    if ! type_action "$serial" "$socket" "$action"; then
        FAILURE_MESSAGE="$action: the console command was not accepted"
        return 1
    fi
    if ! wait_for_exit; then
        FAILURE_MESSAGE="$action: the machine did not leave; a platform that \
honours the request does not keep running"
        return 1
    fi
    QEMU_PID=

    case "$action" in
    shutdown)
        for fragment in \
            '[PLATFORM-POWER] plan action=SHUTDOWN mechanism=ACPI_S5 ' \
            ' pm1a=0x' \
            ' slp_typ=0x' \
            ' dsdt=OK sleep_offset=0x' \
            '[PLATFORM-POWER] attempt=1/1 action=SHUTDOWN space=IO address=0x' \
            ' value=0x'; do
            grep -Fq "$fragment" "$(serial_view "$serial")" || {
                FAILURE_MESSAGE="shutdown: the log does not carry: $fragment"
                return 1
            }
        done
        grep -Fq '[PLATFORM-POWER] result=STILL_RUNNING' "$(serial_view "$serial")" && {
            FAILURE_MESSAGE="shutdown: the module reported a machine that is \
still running, yet QEMU left"
            return 1
        }
        # The sleep type is the firmware's, not a constant of this kernel: the
        # record has to name one the DSDT actually declared.
        grep -Eq 'slp_typ=0x[0-9A-F]+ slp_typ_b=0x[0-9A-F]+' "$(serial_view "$serial")" || {
            FAILURE_MESSAGE="shutdown: the plan does not state a sleep type"
            return 1
        }
        echo "platform power: shutdown planned and performed through ACPI S5"
        ;;
    reboot)
        for fragment in \
            '[PLATFORM-POWER] plan action=REBOOT mechanism=RESET_CHAIN' \
            '[PLATFORM-RESET] plan reset=' \
            '[PLATFORM-RESET] attempt=1/'; do
            grep -Fq "$fragment" "$(serial_view "$serial")" || {
                FAILURE_MESSAGE="reboot: the log does not carry: $fragment"
                return 1
            }
        done
        echo "platform power: reboot ran the reset chain"
        ;;
    *)
        FAILURE_MESSAGE="unknown scenario: $action"
        return 1
        ;;
    esac
    return 0
}

run_scenario shutdown
run_scenario reboot

echo "QEMU platform power test: OK"
