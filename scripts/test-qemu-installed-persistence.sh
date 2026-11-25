#!/usr/bin/env bash
# ITEM-637: the installed disk keeps an owner file across a reboot and across
# the state-preserving update.
#
# ITEM-630 proved HFOCFS on a plain image, ITEM-631/635 bound the workspace to
# block 514 of the installed persistence range and ITEM-634 proved the shell
# through the SSH fixture.  None of those wrote a user file to the *installed*
# disk and read it back after a reboot.  This proof does exactly that against
# the real production media:
#
#   1. install a whole disk from the management-scope medium;
#   2. run the management-scope, test-key update medium over it - the same
#      state-preserving update shape the owner installs - and require the
#      preflight and the key import to verify;
#   3. first installed boot on the QEMU user-mode network: the first-run
#      console conversation creates the account, the production SSH listener
#      starts, its host key is pinned (the scanned key must match the
#      fingerprint the guest publishes), and the owner shell writes one marker
#      into /home/owner; a second session reads the same bytes;
#   4. clean shutdown, second boot: the marker is still there and the pinned
#      host key is still the same key; a wrong client key is refused;
#   5. the update medium runs again, third boot: the marker is still the same
#      bytes and the update still reports the preserved state.
#
# The host also reads the HFOCFS superblock at persistence block 514 and
# requires the two host-seed slots at 512-513 to stay zero, exactly as 635
# declared the layout.  QEMU is not a Toshiba hardware proof.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate dd \
    ssh ssh-keygen ssh-keyscan sha256sum; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_INSTALLED_PERSISTENCE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=10s --signal=TERM \
        "${HFOC_INSTALLED_PERSISTENCE_TEST_TIMEOUT:-1500}s" \
        env HFOC_INSTALLED_PERSISTENCE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
# `-cpu max` advertises RDRAND/RDSEED under TCG so the first installed boot can
# create the host key from the production entropy owner.
QEMU_CPU=${QEMU_CPU:-max}
QEMU_MEMORY=${QEMU_MEMORY:-2176M}
INSTALL_IMAGE=${INSTALL_IMAGE:?the management-scope install medium is required}
UPDATE_IMAGE=${UPDATE_IMAGE:?the management-scope test-key update medium is required}
SSH_KEY_FINGERPRINT=${SSH_KEY_FINGERPRINT:?the test client key fingerprint is required}
PRODUCT_KERNEL=${PRODUCT_KERNEL:-build/hfoc-kernel.elf}
EXPECT_BUILD=${EXPECT_BUILD:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}
CLIENT_KEY=${HFOC_INSTALLED_PERSISTENCE_CLIENT_KEY:-tests/fixtures/ssh/id_ecdsa}
DISK_BYTES=${HFOC_INSTALLED_PERSISTENCE_DISK_BYTES:-536870912}
ADMIN_LOGIN=${HFOC_INSTALLED_PERSISTENCE_LOGIN:-owner}
# The production SSH transport names its one principal `hfoc` (the same name
# the fixture proof uses); the enrolled account's login names the workspace.
SSH_USER=${HFOC_INSTALLED_PERSISTENCE_SSH_USER:-hfoc}
ADMIN_SECRET=${HFOC_INSTALLED_PERSISTENCE_SECRET:-hfoc637persist}
MARKER=${HFOC_INSTALLED_PERSISTENCE_MARKER:-HFOC637-INSTALLED-MARKER-20261120}

test "$QEMU_MACHINE" = q35 || {
    echo "the installed-persistence proof requires QEMU_MACHINE=q35" >&2
    exit 1
}
test "$QEMU_SMP" = 1 || {
    echo "the installed-persistence proof requires QEMU_SMP=1" >&2
    exit 1
}
[[ -f $INSTALL_IMAGE ]] || {
    echo "installed-persistence proof requires install image: $INSTALL_IMAGE" >&2
    exit 1
}
[[ -f $UPDATE_IMAGE ]] || {
    echo "installed-persistence proof requires update image: $UPDATE_IMAGE" >&2
    exit 1
}
[[ -f $CLIENT_KEY ]] || {
    echo "installed-persistence proof requires client key: $CLIENT_KEY" >&2
    exit 1
}

# The one virtio-net profile the product driver binds (ITEM-389), the same one
# the serving and forward proofs use.
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:57,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

TEST_DIR=$(mktemp -d)
FAILURE_MESSAGE=
QEMU_PID=
PORT=
KNOWN_HOSTS=
EXPECTED_HOST_FP=
FIRST_LBA=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/installed-persistence-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-installed-persistence proof failed}" \
            >"$artifact_dir/failure-reason.txt"
        cp -a "$TEST_DIR"/. "$artifact_dir/" 2>/dev/null || true
        echo "Installed-persistence artifacts: $artifact_dir" >&2
    fi
    if [[ -n $QEMU_PID ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

fail() {
    FAILURE_MESSAGE=$*
    echo "INSTALLED-PERSISTENCE: $*" >&2
    exit 1
}

serial_view() {
    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$1.view"
    printf '%s\n' "$1.view"
}

# ITEM-609: a terminal production refusal ends the phase with its reason
# instead of a generic deadline.
wait_marker() {
    local label=$1
    local marker=$2
    local timeout=${3:-180}
    local output status refusal

    if output=$(python3 "$ROOT/scripts/hfoc-qemu-marker-wait.py" \
        "$TEST_DIR/$label.log" "$marker" "$timeout" \
        "$ROOT/scripts/hfoc-serial-view.py" 2>&1); then
        printf '  ok   %s: %s\n' "$label" "$marker" >&2
        return 0
    else
        status=$?
    fi
    printf '%s\n' "$output" >&2
    case $status in
    1) fail "marker not reached in $label: $marker" ;;
    2) fail "kernel panic while waiting in $label for: $marker" ;;
    3)
        refusal=$(printf '%s\n' "$output" |
            sed -n 's/^HFOC-QEMU-TERMINAL-REFUSAL .* refusal="\(.*\)"$/\1/p' |
            head -1)
        fail "terminal production refusal in $label while waiting for $marker: ${refusal:-$output}"
        ;;
    *) fail "marker waiter failed in $label: $marker (exit $status)" ;;
    esac
}

require_record() {
    local label=$1
    local marker=$2
    local view
    view=$(serial_view "$TEST_DIR/$label.log")
    grep -Fq -- "$marker" "$view" || {
        fail "the $label run did not publish: $marker"
    }
    printf '  ok   %s: %s\n' "$label" "$marker" >&2
}

wait_exit() {
    local label=$1
    local timeout=${2:-180}
    local waited=0
    while (( waited < timeout )); do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    fail "the $label run did not finish within ${timeout}s"
}

free_port() {
    python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
}

# start_qemu LABEL DISK IMAGE [MONITOR]
start_qemu() {
    local label=$1
    local disk=$2
    local image=$3
    local monitor=${4:-}

    local -a monitor_arguments=()
    if [[ -n $monitor ]]; then
        monitor_arguments=(-monitor "unix:$monitor,server,nowait")
    fi
    : >"$TEST_DIR/$label.log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -cpu "$QEMU_CPU" -display none -boot order=d,menu=off \
        -cdrom "$image" \
        -drive "if=none,id=target,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=target,bus=ide.0,serial=QM00001" \
        "${monitor_arguments[@]}" \
        -serial "file:$TEST_DIR/$label.log" -no-reboot \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    QEMU_PID=$!
}

# start_installed LABEL DISK MONITOR PORT
#
# No medium: the installed disk boots through its own chain.  The user-mode NIC
# forwards one host port to the production listener on port 22.
start_installed() {
    local label=$1
    local disk=$2
    local monitor=$3
    local port=$4

    : >"$TEST_DIR/$label.log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -cpu "$QEMU_CPU" -display none \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0,serial=QM00001" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22" \
        -device "$NETDEV_DEVICE" \
        -monitor "unix:$monitor,server,nowait" \
        -serial "file:$TEST_DIR/$label.log" -no-reboot -no-shutdown \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    QEMU_PID=$!
    PORT=$port
}

monitor_quit() {
    local socket=$1

    python3 - "$socket" <<'PY' || true
import socket
import sys
import time

deadline = time.monotonic() + 15.0
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    while True:
        try:
            monitor.connect(sys.argv[1])
            break
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            if time.monotonic() > deadline:
                raise SystemExit(1)
            time.sleep(0.1)
    monitor.sendall(b"quit\n")
    time.sleep(0.3)
PY
}

stop_installed() {
    local label=$1
    local socket=$2

    monitor_quit "$socket"
    wait_exit "$label" 30
}

# ITEM-506 increment 5: the unattended install leaves the account unset.  The
# first-run console conversation creates it before the management listener
# opens.  The typed secret never reaches the serial log.
write_enroll_helper() {
    cat >"$TEST_DIR/enroll.py" <<'ENROLLEOF'
import pathlib
import re
import socket
import sys
import time

monitor_path, serial_path, login, secret = sys.argv[1:5]
monitor_path = pathlib.Path(monitor_path)
serial_path = pathlib.Path(serial_path)

DIAGNOSTIC_LINE = re.compile(
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")


def serial_text():
    try:
        return DIAGNOSTIC_LINE.sub(
            "", serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def wait_for(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during first-run enrollment")
        time.sleep(0.05)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="timed out waiting for: %s" '
        'deadline=%ss\n' % (marker, int(timeout)))
    sys.stderr.flush()
    raise SystemExit("timed out waiting for: %s" % marker)


wait_for("[PRODUCTION-FIRST-BOOT]", 180.0)

deadline = time.monotonic() + 30.0
while True:
    try:
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.settimeout(1.0)
        monitor.connect(str(monitor_path))
        break
    except (FileNotFoundError, ConnectionRefusedError, OSError):
        if time.monotonic() > deadline:
            raise SystemExit("the QEMU monitor did not accept a connection")
        time.sleep(0.1)


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


def type_line(line):
    for character in line:
        if character == "-":
            send_key("minus")
        elif character == " ":
            send_key("spc")
        else:
            send_key(character)
    time.sleep(0.3)
    send_key("ret")
    time.sleep(0.1)


def type_until(marker, line, timeout, sensitive=False):
    baseline = serial_text().count(marker)
    for attempt in range(2):
        type_line(line)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = serial_text()
            if text.count(marker) > baseline:
                return
            time.sleep(0.05)
        if attempt == 1 or sensitive:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="timed out waiting for: %s" '
                'deadline=%ss\n' % (marker, int(timeout)))
            sys.stderr.flush()
            raise SystemExit("timed out waiting for: %s" % marker)
        send_key("ret")
        time.sleep(0.5)
        baseline = serial_text().count(marker)
    raise SystemExit("unreachable")


receive_prompt()
# The `credentials` command opens CHANGE mode: the current (default) password
# is asked first, then the new login, then the new password twice.  Typing the
# answers in this order is what the console's conversation expects; a shifted
# order would make the new password equal the new login and be refused.
type_until("Current password:", "credentials", 180.0)
type_until("New login:", secret, 180.0, sensitive=True)
type_until("New password:", login, 180.0)
type_until("Repeat new password:", secret, 180.0, sensitive=True)
type_until("[ADMIN-ENROLLMENT] credentials changed", secret, 180.0,
           sensitive=True)
monitor.close()
print("installed-persistence: the first-run account is set")
ENROLLEOF
}

# wait_listening LABEL: the management listener is up and published its host
# key fingerprint on the serial record.
wait_listening() {
    local label=$1
    local view
    local deadline=$((SECONDS + 300))
    local fingerprint

    while (( SECONDS < deadline )); do
        view=$(serial_view "$TEST_DIR/$label.log") 2>/dev/null || true
        if [[ -f $view ]] && grep -Fq '[PRODUCTION-SSH] state=LISTENING' "$view"; then
            fingerprint=$(grep -o 'hostkey=SHA256:[A-Za-z0-9+/=]*' "$view" |
                tail -1 | sed 's/^hostkey=//')
            if [[ -n $fingerprint ]]; then
                printf '%s' "$fingerprint"
                return 0
            fi
        fi
        if [[ -n $QEMU_PID ]] && ! kill -0 "$QEMU_PID" 2>/dev/null; then
            break
        fi
        sleep 0.25
    done
    fail "$label: the production SSH listener did not publish LISTENING and a host key within 300s"
}

# pin_host_key LABEL FINGERPRINT: fetch the listener's key and pin it only when
# its SHA256 fingerprint matches the fingerprint the guest published on its
# serial record.  Host-key checking is never disabled for the real sessions.
pin_host_key() {
    local label=$1
    local published=$2
    local scanned=$TEST_DIR/$label.hostkey
    local pub=$TEST_DIR/$label.host.pub
    local scanned_fp
    for _ in $(seq 1 40); do
        rm -f "$scanned"
        if timeout 10 ssh-keyscan -T 5 -p "$PORT" -t ecdsa 127.0.0.1 \
            >"$scanned" 2>/dev/null && [[ -s $scanned ]]; then
            break
        fi
        sleep 0.5
    done
    [[ -s $scanned ]] || fail "$label: ssh-keyscan did not read the host key"
    awk '$2 == "ecdsa-sha2-nistp256" {print $2" "$3}' "$scanned" >"$pub"
    [[ -s $pub ]] || fail "$label: ssh-keyscan returned no ECDSA host key"
    scanned_fp=$(ssh-keygen -lf "$pub" -E sha256 | awk '{print $2}')
    [[ "$(printf '%s' "$scanned_fp" | sed 's/=*$//')" == \
       "$(printf '%s' "$published" | sed 's/=*$//')" ]] || fail \
        "$label: the scanned host key $scanned_fp does not match the published $published"
    awk '$2 == "ecdsa-sha2-nistp256"' "$scanned"         >"$TEST_DIR/$label.known_hosts"
    KNOWN_HOSTS=$TEST_DIR/$label.known_hosts
    # ssh-keyscan opens and closes one real session; give the listener's single
    # session slot a moment to return before the first owner session.
    sleep 1
    printf '  ok   %s: host key %s pinned from the published record\n' \
        "$label" "$published" >&2
}

# The production transport only executes the four approved read-only
# diagnostics as an SSH `exec` request.  An owner command therefore runs in a
# real shell (the same pty path ITEM-629/634 proved), not as `exec`.
write_shell_helper() {
    cat >"$TEST_DIR/shell.py" <<'SHELLEOF'
import subprocess
import sys
import threading
import time

port, key, known_hosts, commands_path, output_path = sys.argv[1:6]
with open(commands_path, encoding="utf-8") as handle:
    commands = [line.rstrip("\n") for line in handle if line.strip()]

command = [
    "ssh", "-F", "/dev/null", "-p", port, "-i", key,
    "-o", "IdentitiesOnly=yes",
    "-o", "UserKnownHostsFile=" + known_hosts,
    "-o", "StrictHostKeyChecking=yes",
    "-o", "BatchMode=yes",
    "-o", "LogLevel=ERROR",
    "-o", "KexAlgorithms=ecdh-sha2-nistp256",
    "-o", "Ciphers=aes128-gcm@openssh.com",
    "-o", "MACs=hmac-sha2-256",
    "-tt", "hfoc@127.0.0.1",
]
process = subprocess.Popen(
    command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT, text=True, bufsize=1)
output = []


def read():
    for chunk in iter(lambda: process.stdout.read(1), ""):
        output.append(chunk)


reader = threading.Thread(target=read, daemon=True)
reader.start()
try:
    time.sleep(1.0)
    for line in commands:
        process.stdin.write(line + "\n")
        process.stdin.flush()
        time.sleep(0.7)
    process.stdin.write("exit\n")
    process.stdin.flush()
except BrokenPipeError:
    pass
try:
    process.stdin.close()
except BrokenPipeError:
    pass
try:
    status = process.wait(timeout=20)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait(timeout=5)
    status = 124
reader.join(timeout=2)
with open(output_path, "w", encoding="utf-8") as handle:
    handle.write("".join(output))
raise SystemExit(0 if status == 0 else 1)
SHELLEOF
}

# run_shell_lines KEY OUTPUT COMMAND...
run_shell_lines() {
    local key=$1
    local output=$2
    shift 2
    printf '%s\n' "$@" >"$TEST_DIR/shell.commands"
    timeout 90 python3 "$TEST_DIR/shell.py" "$PORT" "$key" "$KNOWN_HOSTS" \
        "$TEST_DIR/shell.commands" "$output"
}

# parse_cat OUTPUT EXPECTED STATUS_REQUIRED
#
# The tty echoes each command, so `cat` also appears on the command line.  The
# file bytes are the first non-empty line after the echoed `cat` command.  The
# shell status comes from the `echo STATUS=$?` line that follows it.
parse_cat() {
    python3 - "$1" "$2" "$3" <<'PY'
import re
import sys

path, expected, require_zero = sys.argv[1], sys.argv[2], sys.argv[3] == "yes"
text = open(path, encoding="utf-8", errors="replace").read()
lines = [line.rstrip("\r") for line in text.split("\n")]
content = None
for index, line in enumerate(lines):
    if "/bin/cat /home/owner/persist-marker" in line:
        following = index + 1
        while following < len(lines) and lines[following] == "":
            following += 1
        if following < len(lines):
            content = lines[following]
        break
statuses = re.findall(r"^STATUS=([0-9]+)$", "\n".join(lines), re.MULTILINE)
status = statuses[-1] if statuses else None
if require_zero:
    if status != "0":
        raise SystemExit("the file read did not return status 0 (status=%s)" % status)
    if content != expected:
        raise SystemExit("the file bytes differ: %r" % (content,))
else:
    if status == "0":
        raise SystemExit("the file existed when it must not (status=0)")
    if content == expected:
        raise SystemExit("the file bytes were visible before the write")
print("ok")
PY
}

# boot_installed LABEL DISK ENROLL
boot_installed() {
    local label=$1
    local disk=$2
    local enroll=$3
    local socket=$TEST_DIR/$label.monitor
    local published

    rm -f "$socket"
    start_installed "$label" "$disk" "$socket" "$(free_port)"
    if [[ $enroll == yes ]]; then
        python3 "$TEST_DIR/enroll.py" "$socket" "$TEST_DIR/$label.log" \
            "$ADMIN_LOGIN" "$ADMIN_SECRET" ||
            fail "$label: the first-run account conversation failed"
        if grep -aFq -- "$ADMIN_SECRET" "$(serial_view "$TEST_DIR/$label.log")"; then
            fail "$label: the hidden console input leaked the account password"
        fi
    fi
    wait_marker "$label" "[WORKSPACE] state=OK range=514+32768 prefix=/home/owner" 300
    published=$(wait_listening "$label")
    if [[ -n $EXPECTED_HOST_FP && "$published" != "$EXPECTED_HOST_FP" ]]; then
        fail "$label: the host key changed across a reboot ($EXPECTED_HOST_FP -> $published)"
    fi
    EXPECTED_HOST_FP=$published
    pin_host_key "$label" "$published"
    require_record "$label" "[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE"
}

# read_marker LABEL: one real shell session reads the marker bytes.
read_marker() {
    local label=$1
    run_shell_lines "$TEST_DIR/id_ecdsa" "$TEST_DIR/$label.marker.out" \
        '/bin/cat /home/owner/persist-marker' \
        'echo STATUS=$?' ||
        fail "$label: the owner shell could not read the marker"
    parse_cat "$TEST_DIR/$label.marker.out" "$MARKER" yes ||
        fail "$label: the marker bytes changed"
    printf '  ok   %s: /home/owner/persist-marker holds the same bytes\n' \
        "$label" >&2
}

# install_whole_disk LABEL MEDIUM: boot the medium, wait for the install, and
# require the records the flow publishes before the first write.
install_whole_disk() {
    local label=$1
    local medium=$2
    local disk=$TEST_DIR/$label.raw

    truncate -s "$DISK_BYTES" "$disk"
    start_qemu "$label" "$disk" "$medium"
    wait_marker "$label" "[PRODUCTION-INSTALL] applied" 300
    wait_exit "$label" 180
    require_record "$label" "[INSTALLER-LAYOUT] plan mode=clean-whole-disk-bios"
    require_record "$label" "[PRODUCTION-INSTALL] state initialized=VERIFIED"
    printf '%s\n' "$disk"
}

run_update() {
    local label=$1
    local disk=$2

    start_qemu "$label" "$disk" "$UPDATE_IMAGE"
    wait_marker "$label" "[PRODUCTION-INSTALL] applied" 360
    wait_exit "$label" 180
    require_record "$label" "[PRODUCTION-INSTALL] state preserve preflight=VERIFIED"
    require_record "$label" "[PRODUCTION-INSTALL] state preserved=VERIFIED"
    require_record "$label" \
        "[PRODUCTION-INSTALL] ssh-key fingerprint=${SSH_KEY_FINGERPRINT} persisted=VERIFIED"
}

# check_workspace_disk DISK FIRST_LBA LABEL: independent byte check of the
# declared HFOCFS layout - superblock at 514, host-seed slots 512-513 zero.
check_workspace_disk() {
    local disk=$1
    local first_lba=$2
    local label=$3

    python3 - "$disk" "$first_lba" "$label" <<'PY'
import sys

disk, first_lba, label = sys.argv[1], int(sys.argv[2]), sys.argv[3]
block = 512
with open(disk, "rb") as image:
    image.seek((first_lba + 512) * block)
    host_seed = image.read(2 * block)
    image.seek((first_lba + 514) * block)
    magic = image.read(7)
if host_seed != b"\x00" * (2 * block):
    raise SystemExit("%s: the installed workspace changed the host-seed slots" % label)
if magic != b"HFOCFS1":
    raise SystemExit("%s: the installed workspace image is missing at block 514" % label)
print("  ok   %s: HFOCFS starts at 514 and the host seed is untouched" % label,
      file=sys.stderr)
PY
}

echo "QEMU installed-persistence test: build=$EXPECT_BUILD"

cp "$CLIENT_KEY" "$TEST_DIR/id_ecdsa"
chmod 600 "$TEST_DIR/id_ecdsa"
ssh-keygen -q -t ecdsa -b 256 -N '' -f "$TEST_DIR/wrong_key"
write_enroll_helper
write_shell_helper

# --------------------------------------------------------------- INSTALL ---
echo "INSTALL: whole disk from the management-scope medium"
DISK=$(install_whole_disk install "$INSTALL_IMAGE")
# The install runs in a command substitution, so read the published range in
# this shell rather than assigning a global inside the subshell.
FIRST_LBA=$(sed -n 's/.*role=hfoc-persistence first_lba=\([0-9][0-9]*\).*/\1/p' \
    "$(serial_view "$TEST_DIR/install.log")" | head -1)
[[ -n $FIRST_LBA ]] || fail "install: the install did not publish the persistence range"
printf '  ok   install: persistence range starts at %s\n' "$FIRST_LBA" >&2

# ----------------------------------------------------------------- UPDATE ---
echo "UPDATE: the management-scope test-key update preserves the installed state"
run_update update "$DISK"

# --------------------------------------------------------- FIRST BOOT ------
echo "BOOT 1: account, pinned SSH, first owner write"
boot_installed first "$DISK" yes

run_shell_lines "$TEST_DIR/id_ecdsa" "$TEST_DIR/first.absent.out" \
    '/bin/cat /home/owner/persist-marker' \
    'echo STATUS=$?' ||
    fail "first: the owner shell could not run the absent-marker read"
parse_cat "$TEST_DIR/first.absent.out" "$MARKER" no ||
    fail "first: the marker existed before the owner wrote it"
printf '  ok   first: the marker is absent before the write\n' >&2

run_shell_lines "$TEST_DIR/id_ecdsa" "$TEST_DIR/first.write.out" \
    "/bin/echo $MARKER > /home/owner/persist-marker" \
    '/bin/cat /home/owner/persist-marker' \
    'echo STATUS=$?' ||
    fail "first: the owner shell could not write the marker"
parse_cat "$TEST_DIR/first.write.out" "$MARKER" yes ||
    fail "first: the write returned different bytes"
printf '  ok   first: the owner shell wrote and read the marker\n' >&2

run_shell_lines "$TEST_DIR/id_ecdsa" "$TEST_DIR/second-session.out" \
    '/bin/cat /home/owner/persist-marker' \
    'echo STATUS=$?' ||
    fail "first: a second owner session could not read the marker"
parse_cat "$TEST_DIR/second-session.out" "$MARKER" yes ||
    fail "first: a second owner session saw different bytes"
printf '  ok   first: a second SSH session reads the same file\n' >&2

set +e
timeout 45 ssh -F /dev/null -p "$PORT" -i "$TEST_DIR/wrong_key" \
    -o IdentitiesOnly=yes -o UserKnownHostsFile="$KNOWN_HOSTS" \
    -o StrictHostKeyChecking=yes -o BatchMode=yes -o LogLevel=ERROR \
    "$SSH_USER@127.0.0.1" 'echo pwned' \
    >"$TEST_DIR/wrong-key.out" 2>"$TEST_DIR/wrong-key.err"
wrong_key_status=$?
set -e
(( wrong_key_status != 0 )) || fail "first: a client key that is not the owner's was accepted"
grep -Fq 'pwned' "$TEST_DIR/wrong-key.out" && fail \
    "first: the refused client key executed a command"
printf '  ok   first: a client key that is not the owner key is refused\n' >&2
check_workspace_disk "$DISK" "$FIRST_LBA" first

stop_installed first "$TEST_DIR/first.monitor"

# -------------------------------------------------------- SECOND BOOT ------
echo "BOOT 2: the same bytes and the same host key after a reboot"
boot_installed second "$DISK" no
read_marker second
check_workspace_disk "$DISK" "$FIRST_LBA" second
stop_installed second "$TEST_DIR/second.monitor"

# ---------------------------------------------------------- RE-UPDATE ------
echo "UPDATE 2: the same update runs again and keeps the file"
run_update update-again "$DISK"
check_workspace_disk "$DISK" "$FIRST_LBA" update-again

# --------------------------------------------------------- THIRD BOOT ------
echo "BOOT 3: the file survives the update"
boot_installed third "$DISK" no
read_marker third
check_workspace_disk "$DISK" "$FIRST_LBA" third
stop_installed third "$TEST_DIR/third.monitor"

echo "QEMU installed-persistence test: OK"
