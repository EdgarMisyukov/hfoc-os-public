#!/usr/bin/env bash
# Prove the item-305 appliance command through the normal boot console:
# keyboard sends appliance, canonical bounded state is rendered, and HMP quits
# cleanly.
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

if [[ "${HFOC_APPLIANCE_CONSOLE_TEST_INNER:-0}" != 1 ]]; then
    # ITEM-509 increment 4: the proof now types two more commands after the
    # appliance one (`language`, `language ru`) and reads their answers.
    # ITEM-509 increment 7 adds two more (`help`, `help install`) with a longer
    # answer, so the budget moves from the 120 s the language shape needed.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_APPLIANCE_CONSOLE_TEST_TIMEOUT:-150}s" \
        env HFOC_APPLIANCE_CONSOLE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Appliance console test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Appliance console test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Appliance console test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU appliance console baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/appliance-console-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Appliance console test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Appliance console test artifacts: $artifact_dir" >&2
}

best_effort_cleanup() {
    if [[ -z "$QEMU_PID" ]]; then
        return 0
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=
        return 0
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    local deadline=$((SECONDS + 2))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        best_effort_cleanup
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

QEMU_ARGS=(
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown
)
qemu-system-x86_64 "${QEMU_ARGS[@]}" >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

QEMU_WAIT_RESULT=

wait_for_exit() {
    local deadline=$((SECONDS + 5))
    QEMU_WAIT_RESULT=0
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            QEMU_WAIT_RESULT=timeout
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null
    QEMU_WAIT_RESULT=$?
    return "$QEMU_WAIT_RESULT"
}

send_monitor_quit() {
    python3 - "$MONITOR_SOCKET"   <<'PY'
import socket
import sys
import time

def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")

    prompt_deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = prompt_deadline - time.monotonic()
        if remaining <= 0.0:
            fail("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            fail("QEMU HMP closed before prompt")
        prompt += chunk

    try:
        monitor.sendall(b"quit\n")
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")

    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            fail("QEMU monitor EOF timeout after quit")
        if not chunk:
            break
finally:
    monitor.close()
PY
}

successful_shutdown() {
    local quit_error
    if ! quit_error=$(send_monitor_quit 2>&1); then
        FAILURE_MESSAGE="${quit_error:-QEMU monitor quit failed}"
        return 1
    fi
    if ! wait_for_exit; then
        if [[ "$QEMU_WAIT_RESULT" == timeout ]]; then
            FAILURE_MESSAGE="QEMU did not exit after monitor quit"
        else
            FAILURE_MESSAGE="QEMU exited with status $QEMU_WAIT_RESULT after monitor quit"
        fi
        return 1
    fi
    return 0
}

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
deadline = time.monotonic() + 45.0

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
    if "hfoc> " in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic before sendkey")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for boot prompt")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    try:
        monitor.connect(str(socket_path))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

    def receive_prompt():
        chunks = []
        while True:
            chunk = monitor.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed unexpectedly")
            chunks.append(chunk)
            if b"(qemu)" in b"".join(chunks):
                return b"".join(chunks).decode("utf-8", errors="replace")

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        with monitor_log.open("a", encoding="utf-8") as output:
            output.write(receive_prompt())
        time.sleep(0.05)

    required = [
        "appliance v=1",
        "identity product=HFOC-APPLIANCE-V1 arch=x86_64 "
        "platform=q35 hostname=hfoc-appliance",
        "config state=OK source=DEFAULT_MISSING "
        "hostname=hfoc-appliance",
        "health state=OK uptime_ticks=",
        "network state=UNAVAILABLE",
        "services state=UNAVAILABLE",
    ]

    receive_prompt()
    for character in "appliance":
        send_key(character)
    send_key("ret")

    command_deadline = time.monotonic() + 30.0
    while time.monotonic() < command_deadline:
        if all(marker in serial_text() for marker in required):
            break
        if "*** HFOC KERNEL PANIC ***" in serial_text():
            raise SystemExit("Kernel panic during appliance command")
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="appliance command output" '
            'deadline=' + str(int(command_deadline)) + 's\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for appliance command output")

    # ITEM-509 increment 4: the language command on the real console.
    # `language` shows what the machine is set to, and the switch has to answer
    # with exactly one of the two truths: the confirmation in the language just
    # chosen, or the named refusal of a machine whose record cannot be written.
    for character in "language":
        send_key(character)
    send_key("ret")

    language_deadline = time.monotonic() + 30.0
    while time.monotonic() < language_deadline:
        if "LANGUAGE: EN" in serial_text():
            break
        if "*** HFOC KERNEL PANIC ***" in serial_text():
            raise SystemExit("Kernel panic during language command")
        time.sleep(0.05)
    else:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="language show output" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the language show output")

    if serial_text().count("LANGUAGE: EN") != 1:
        raise SystemExit(
            "the language command did not show the active language")

    for character in "language ru":
        send_key("spc" if character == " " else character)
    send_key("ret")

    switch_deadline = time.monotonic() + 30.0
    switch_outcome = None
    while time.monotonic() < switch_deadline:
        text = serial_text()
        if "\u042f\u0417\u042b\u041a \u0421\u041e\u0425\u0420\u0410\u041d\u0401\u041d" in text:
            switch_outcome = "saved"
            break
        if "LANGUAGE IS NOT AVAILABLE" in text:
            switch_outcome = "unavailable"
            break
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during language switch")
        time.sleep(0.05)
    if switch_outcome is None:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="language switch answer" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the language switch answer")
    if "[GLYPH-MISSING]" in serial_text():
        raise SystemExit(
            "a glyph was missing while the console printed Russian")
    language_switch_outcome = switch_outcome

    # ITEM-509 increment 7: help on the real console.  `help` prints the table
    # of contents and `help install` prints one topic; both follow the language
    # the console ended up in, and the proof accepts either truth - the switch
    # above is saved on a machine with a record and refused on one without.
    for character in "help":
        send_key(character)
    send_key("ret")

    help_deadline = time.monotonic() + 30.0
    help_toc = None
    while time.monotonic() < help_deadline:
        text = serial_text()
        if "TOPICS" in text:
            help_toc = "en"
            break
        if "\u0422\u0415\u041c\u042b" in text:
            help_toc = "ru"
            break
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during help")
        time.sleep(0.05)
    if help_toc is None:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="help table of contents" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the help table of contents")

    text = serial_text()
    for topic in ("install", "network", "storage", "account",
                  "certificates", "names", "panel", "recovery", "records"):
        if topic not in text:
            raise SystemExit(
                f"the help table of contents lost topic {topic}")
    if "COMMANDS: HELP CLEAR SCROLL VERSION" not in text:
        raise SystemExit(
            "the help answer lost the command vocabulary line")

    for character in "help install":
        send_key("spc" if character == " " else character)
    send_key("ret")

    topic_deadline = time.monotonic() + 30.0
    topic_titles = (
        "Installation and disk choice",
        "\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 "
        "\u0438 \u0432\u044b\u0431\u043e\u0440 \u0434\u0438\u0441\u043a\u0430")
    while time.monotonic() < topic_deadline:
        text = serial_text()
        if any(title in text for title in topic_titles):
            break
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during the help topic")
        time.sleep(0.05)
    else:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="help topic output" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the help topic output")
    if "[GLYPH-MISSING]" in serial_text():
        raise SystemExit("a glyph was missing while the console printed help")

    text = serial_text()
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[APPLIANCE] SNAPSHOT FAILED" in text or \
        "[APPLIANCE] RENDER FAILED" in text:
    raise SystemExit("appliance command reported a render failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

print("Appliance command output: exact")
print(f"Language switch: {language_switch_outcome}")
print(f"Help table of contents: {help_toc}")
print("Kernel panic: none")
print("QEMU alive: yes")
PY
); then
    FAILURE_MESSAGE=$phase1_output
    printf '%s\n' "$phase1_output" >&2
    exit 1
fi

printf '%s\n' "$phase1_output"
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    FAILURE_MESSAGE="QEMU exited before monitor quit"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "QEMU alive before quit: yes"

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU appliance console test: OK"
