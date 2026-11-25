#!/usr/bin/env bash
# ITEM-588 QEMU proof: the state-preserving update on a legacy-BIOS install.
#
# The owner's Toshiba ran the first hardware update from `473b39bf` and was
# refused with `PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE writes=0`: the
# installed appliance had no configuration and no account (the CPU has no
# entropy source, so the account could not be set), and the preflight treated a
# default-only state as a reason to refuse instead of a state to keep.
#
# This proof drives the real production media on a legacy-BIOS machine and
# pins both halves of the fix:
#
#   DEFAULTS  install whole-disk from an authorised medium (no settings), then
#             run the update medium against that disk: the preflight proves the
#             ranges and the loader and reports `config=DEFAULT admin=ABSENT`,
#             the run applies, and the installed disk boots the payload the
#             update medium carries while the state partition's bytes are
#             exactly what they were.
#   SETTINGS  the same, installed from the language medium so the state holds a
#             real setting: the preflight reports `config=PRESENT`, the update
#             applies, and the installed console still answers in the
#             preserved language.
#   RANGE     the persistence entry is moved by one LBA in the target's table:
#             the update is refused with `RANGE_MISMATCH` and both ranges, and
#             the disk is byte for byte what it was.
#   UNREADABLE the persistence partition's first store header is zeroed: the
#             update is refused with `LOADER_NOT_READY`, still writes nothing.
#
# ITEM-646: every negative phase freezes the emulator at its terminal refusal
# record.  The machine continues into the installed appliance after a refusal,
# and that boot writes its own runtime state; without the freeze that write
# races the "byte for byte unchanged" comparison (observed as the
# `oversized-declaration refusal wrote to the disk` CI flake).
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp mcopy xorriso \
    sha256sum truncate dd; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_INSTALL_UPDATE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALL_UPDATE_TEST_TIMEOUT:-600}s" \
        env HFOC_INSTALL_UPDATE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
# ITEM-589: the CI runner is one cgroup with a 10 GiB high / 11 GiB max bound,
# and the installer proof already runs one 3 GiB guest.  This proof adds a
# second concurrent one, so it takes the smallest guest the supported-hardware
# policy admits: the policy floor is 2 GiB of usable RAM and QEMU reserves a few
# tens of MiB, so 2176 MiB leaves the guest above the floor without doubling
# the phase's footprint.
QEMU_MEMORY=${QEMU_MEMORY:-2176M}
INSTALL_IMAGE=${INSTALL_IMAGE:?the whole-disk install medium is required}
LANGUAGE_IMAGE=${LANGUAGE_IMAGE:?the language install medium is required}
UPDATE_IMAGE=${UPDATE_IMAGE:?the update medium is required}
SSH_KEY_UPDATE_IMAGE=${SSH_KEY_UPDATE_IMAGE:?the SSH-key update medium is required}
CAPACITY_UPDATE_IMAGE=${CAPACITY_UPDATE_IMAGE:?the capacity update medium is required}
OVERSIZED_UPDATE_IMAGE=${OVERSIZED_UPDATE_IMAGE:?the oversized update medium is required}
SSH_KEY_FINGERPRINT=${SSH_KEY_FINGERPRINT:?the SSH key fingerprint is required}
CAPACITY_CONFIRM=${CAPACITY_CONFIRM:-ITEM-394-UNATTENDED-INSTALL}
CAPACITY_TARGET=${CAPACITY_TARGET:-QM00001}
CAPACITY_ADDRESS=${CAPACITY_ADDRESS:-10.0.2.15/24}
CAPACITY_FORWARD_TARGET=${CAPACITY_FORWARD_TARGET:-10.0.2.2}
CAPACITY_FORWARD_CLIENTS=${CAPACITY_FORWARD_CLIENTS:-10.0.2.2,10.0.2.3,10.0.2.4,10.0.2.5}
PRODUCT_KERNEL=${PRODUCT_KERNEL:-build/hfoc-kernel.elf}
EXPECT_BUILD=${EXPECT_BUILD:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}

for image in "$INSTALL_IMAGE" "$LANGUAGE_IMAGE" "$UPDATE_IMAGE" \
    "$SSH_KEY_UPDATE_IMAGE" "$CAPACITY_UPDATE_IMAGE" \
    "$OVERSIZED_UPDATE_IMAGE" "$PRODUCT_KERNEL"; do
    [[ -f "$image" ]] || {
        echo "required medium is missing: $image" >&2
        exit 1
    }
done

test "${QEMU_MACHINE:-}" = q35 || {
    echo "the update proof must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "the update proof must set QEMU_SMP=1" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
FAILURE_MESSAGE=
QEMU_PID=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/production-install-update-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-update proof failed}" \
            >"$artifact_dir/failure-reason.txt"
        cp -a "$TEST_DIR"/. "$artifact_dir/" 2>/dev/null || true
        echo "Update-proof artifacts: $artifact_dir" >&2
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
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
    echo "UPDATE-TEST: $*" >&2
    exit 1
}

serial_view() {
    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$1.view"
    printf '%s\n' "$1.view"
}

# ITEM-636: the installed medium must carry the native userland root, not the
# bare source tree with an empty /bin.  This reads the archive the installer
# actually stages beside the kernel and requires /bin/sh to be a real ELF file.
rootfs_archive_has_shell() {
    local image=$1
    local label=$2
    local arc=$TEST_DIR/rootfs-$label.arc

    rm -f "$arc"
    xorriso -osirrox on -indev "$image" \
        -extract /boot/hfoc-rootfs.arc "$arc" >/dev/null 2>&1 ||
        fail "$label: cannot read the medium's root initramfs"
    python3 - "$arc" "$label" <<'PY' || fail "$label: the root initramfs has no native /bin/sh"
import struct
import sys

path, label = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
if len(data) < 32:
    raise SystemExit(1)
magic, version, count, total, _checksum, _flags, _reserved = \
    struct.unpack_from("<8sIIIIII", data, 0)
if magic != b"HFOCIR01" or version != 1 or total != len(data) or count > 64:
    raise SystemExit(1)

found = False
offset = 32
for _ in range(count):
    kind, _mode, path_length, data_length, data_offset, _entry_sum, _r = \
        struct.unpack_from("<HHIIIII", data, offset)
    entry_path = data[offset + 24:offset + 24 + path_length].decode("utf-8")
    if entry_path == "/bin/sh" and kind == 1 and data_length >= 4:
        found = data[data_offset:data_offset + 4] == b"\x7fELF"
    offset = (data_offset + data_length + 3) & ~3
if not found:
    raise SystemExit(1)
print("  ok   %s: the root initramfs carries a native /bin/sh (%d bytes)" %
      (label, len(data)), file=sys.stderr)
PY
}

# start_qemu LABEL DISK IMAGE [MONITOR_SOCKET]
start_qemu() {
    local label=$1
    local disk=$2
    local image=$3
    local monitor=${4:-}

    local -a monitor_arguments=()
    if [[ -n "$monitor" ]]; then
        monitor_arguments=(-monitor "unix:$monitor,server,nowait")
    fi
    : >"$TEST_DIR/$label.log"
    # The medium boots first: after the install the target disk carries its own
    # Limine chain, so a firmware that is left to choose would boot the
    # installed appliance instead of the update medium.
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$image" \
        -drive "if=none,id=target,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=target,bus=ide.0,serial=QM00001" \
        "${monitor_arguments[@]}" \
        -serial "file:$TEST_DIR/$label.log" -no-reboot \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    QEMU_PID=$!
}

# The installed disk boots on its own: no medium, a monitor for the console.
start_installed() {
    local label=$1
    local disk=$2
    local monitor=$3

    : >"$TEST_DIR/$label.log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0,serial=QM00001" \
        -monitor "unix:$monitor,server,nowait" \
        -serial "file:$TEST_DIR/$label.log" -no-reboot -no-shutdown \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    QEMU_PID=$!
}

stop_qemu() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        # A negative phase freezes the vCPU at its terminal record.  SIGTERM
        # stays pending while the process is stopped, so resume it once to let
        # QEMU flush and exit; without this the wait below would hang.
        kill -CONT "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    QEMU_PID=
}

# ITEM-609: the wait lives in scripts/hfoc-qemu-marker-wait.py, which the host
# regression drives with the saved refusal shape.  A terminal production
# refusal ends the phase with its reason instead of a generic deadline.
#
# ITEM-646: when STOP_PID is given (the negative scenarios), the waiter sends
# SIGSTOP the moment its marker appears.  After a refusal the guest would
# otherwise continue into the installed appliance, whose boot writes runtime
# state and makes the "refusal wrote nothing" comparison race the guest.
wait_marker() {
    local label=$1
    local marker=$2
    local timeout=${3:-120}
    local stop_pid=${4:-}
    local output status refusal
    local -a stop_args=()

    if [[ -n "$stop_pid" ]]; then
        stop_args=(--stop-pid "$stop_pid")
    fi
    if output=$(python3 "$ROOT/scripts/hfoc-qemu-marker-wait.py" \
        "$TEST_DIR/$label.log" "$marker" "$timeout" \
        "$ROOT/scripts/hfoc-serial-view.py" "${stop_args[@]}" 2>&1); then
        return 0
    else
        status=$?
    fi
    printf '%s\n' "$output" >&2
    case $status in
    1) fail "marker not reached in $label: $marker" ;;
    2) fail "kernel panic while waiting in $label for: $marker" ;;
    3)
        # The saved failure-reason.txt must carry the production answer, not
        # just the fact that the wait ended.
        refusal=$(printf '%s\n' "$output" |
            sed -n 's/^HFOC-QEMU-TERMINAL-REFUSAL .* refusal="\(.*\)"$/\1/p' |
            head -1)
        fail "terminal production refusal in $label while waiting for $marker: ${refusal:-$output}"
        ;;
    4) fail "the $label emulator could not be frozen at its refusal: $output" ;;
    *) fail "marker waiter failed in $label: $marker (exit $status)" ;;
    esac
}

# A marker the console prints in a non-ASCII language is matched against the
# raw bytes: the shared serial view decodes the stream as ASCII and replaces
# what it cannot read, so the view cannot carry the Cyrillic label.
wait_raw_marker() {
    local label=$1
    local marker=$2
    local timeout=${3:-60}
    local waited=0

    while (( waited < timeout )); do
        if grep -aFq -- "$marker" "$TEST_DIR/$label.log"; then
            printf '  ok   %s: %s\n' "$label" "$marker" >&2
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    fail "the raw marker was not reached in $label: $marker"
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
    local view
    view=$(serial_view "$TEST_DIR/$label.log")
    fail "the $label run did not finish: $(tail -n 3 "$view" | tr '\n' ' ')"
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

reject_record() {
    local label=$1
    local marker=$2
    local view
    view=$(serial_view "$TEST_DIR/$label.log")
    if grep -Fq -- "$marker" "$view"; then
        fail "the $label run published what it must not: $marker"
    fi
    printf '  ok   %s: absent %s\n' "$label" "$marker" >&2
}

# The bytes of the state partition, and the whole-disk fingerprint a refused
# run must reproduce exactly.
state_hash() {
    local disk=$1
    local first_lba=$2
    local length_lba=$3

    dd if="$disk" bs=512 skip="$first_lba" count="$length_lba" \
        status=none | sha256sum | awk '{print $1}'
}

# ITEM-646: the proof is about disk contents, so fingerprint the whole file by
# size and SHA-256.  The old fingerprint also carried the allocated-block count
# (`stat %b`), which is filesystem metadata rather than a byte of the disk and
# could change without any content change; hashing all 512 MiB is stricter than
# the old 4 MiB prefix and makes the failure message name both sides.
disk_fingerprint() {
    local path=$1

    printf '%s %s\n' "$(stat -c '%s' "$path")" \
        "$(sha256sum "$path" | awk '{print $1}')"
}

# The persistence range the install published, and the boot-volume range the
# filesystem check needs.
plan_role() {
    local label=$1
    local role=$2
    local view
    view=$(serial_view "$TEST_DIR/$label.log")
    sed -n "s/.*role=$role first_lba=\([0-9][0-9]*\) length_lba=\([0-9][0-9]*\).*/\1 \2/p" \
        "$view" | head -1
}

# Type keys on the guest's console through the monitor socket.
type_keys() {
    local label=$1
    local socket=$2
    local text=$3

    python3 - "$socket" "$text" <<'PY'
import socket
import sys
import time

path, text = sys.argv[1], sys.argv[2]
monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(10.0)
    monitor.connect(path)
    prompt = b""
    while b"(qemu)" not in prompt:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit("the monitor closed before its prompt")
        prompt += chunk
    for character in text:
        if character == " ":
            key = "spc"
        elif character == "-":
            key = "minus"
        else:
            key = character
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        chunk = b""
        while b"(qemu)" not in chunk:
            data = monitor.recv(4096)
            if not data:
                raise SystemExit("the monitor closed while typing")
            chunk += data
        time.sleep(0.05)
    monitor.sendall(b"sendkey ret\n")
    time.sleep(0.3)
finally:
    monitor.close()
PY
}

# The product kernel staged on the installed disk's boot volume must be the one
# the update medium carries: that is what "boots the new build from disk" means
# for one tree's proof.
staged_kernel_matches() {
    local disk=$1
    local boot_first_lba=$2
    local label=$3

    rm -f "$TEST_DIR/medium-kernel.elf" "$TEST_DIR/disk-kernel.elf"
    xorriso -osirrox on -indev "$UPDATE_IMAGE" \
        -extract /boot/hfoc-kernel.elf "$TEST_DIR/medium-kernel.elf" \
        >/dev/null 2>&1 || fail "cannot read the update medium's product kernel"
    mcopy -n -i "$disk@@$((boot_first_lba * 512))" \
        ::/boot/hfoc-kernel.elf "$TEST_DIR/disk-kernel.elf" \
        >/dev/null 2>&1 || fail "cannot read the installed boot volume"
    local medium_hash disk_hash
    medium_hash=$(sha256sum "$TEST_DIR/medium-kernel.elf" | awk '{print $1}')
    disk_hash=$(sha256sum "$TEST_DIR/disk-kernel.elf" | awk '{print $1}')
    [[ "$medium_hash" == "$disk_hash" ]] || fail \
        "$label: the installed boot volume does not carry the update medium's kernel"
    printf '  ok   %s: the installed disk carries the update medium product kernel\n' \
        "$label" >&2
}

# install_whole_disk LABEL MEDIUM DISK_BYTES
install_whole_disk() {
    local label=$1
    local medium=$2
    local bytes=$3
    local disk=$TEST_DIR/$label.raw

    truncate -s "$bytes" "$disk"
    start_qemu "$label" "$disk" "$medium"
    wait_marker "$label" "[PRODUCTION-INSTALL] applied" 180
    wait_exit "$label" 180
    require_record "$label" "[INSTALLER-LAYOUT] plan mode=clean-whole-disk-bios"
    require_record "$label" "[PRODUCTION-INSTALL] state initialized=VERIFIED"
    printf '%s\n' "$disk"
}

# run_update LABEL DISK EXPECT [IMAGE [REFUSAL]]
#
# EXPECT is `applied` for a run that must complete and reset - QEMU is started
# with `-no-reboot`, so the guest's own reset ends the emulator - and `refused`
# for a run that must stop before the first write: a refusal leaves the machine
# running the appliance entry, exactly as the owner's Toshiba did, so the
# refusal record ends the run and the harness stops the emulator.  REFUSAL is
# the exact media/flow reason (`CONTROLLER` for the post-plan refusals,
# `TOKEN_TOO_LARGE` for an oversized declaration).
run_update() {
    local label=$1
    local disk=$2
    local expect=$3
    local image=${4:-$UPDATE_IMAGE}
    local refusal=${5:-CONTROLLER}

    start_qemu "$label" "$disk" "$image"
    case $expect in
    applied)
        wait_marker "$label" "[PRODUCTION-INSTALL] applied" 240
        wait_exit "$label" 120
        ;;
    refused)
        # Freeze at the terminal record: the guest otherwise continues into
        # the installed appliance before this shell can stop the emulator, and
        # that boot's own writes would contaminate the zero-write proof.
        wait_marker "$label" \
            "[PRODUCTION-INSTALL] refused reason=$refusal" 240 "$QEMU_PID"
        stop_qemu
        ;;
    *)
        fail "run_update: unknown expectation $expect"
        ;;
    esac
}

boot_installed() {
    local label=$1
    local disk=$2
    local marker=$3

    local socket=$TEST_DIR/$label.monitor
    rm -f "$socket"
    start_installed "$label" "$disk" "$socket"
    wait_marker "$label" "$marker" 180
}

echo "QEMU production install update test: legacy BIOS, build=$EXPECT_BUILD"

# ITEM-636: both media that carry the installed root must carry the same native
# userland as `make iso`; a bare rootfs archive would boot an installed system
# without /bin/sh and without the shell the owner was promised.
rootfs_archive_has_shell "$INSTALL_IMAGE" install
rootfs_archive_has_shell "$UPDATE_IMAGE" update

# ------------------------------------------------------------------ DEFAULTS
echo "DEFAULTS: an installed appliance with no settings is updated, not refused"
DEFAULT_DISK=$(install_whole_disk defaults "$INSTALL_IMAGE" 536870912)
DEFAULT_STATE=$(plan_role defaults hfoc-persistence) || true
[[ -n "$DEFAULT_STATE" ]] || fail "the install did not publish the persistence range"
DEFAULT_BOOT=$(plan_role defaults boot-volume) || true
[[ -n "$DEFAULT_BOOT" ]] || fail "the install did not publish the boot volume range"
STATE_BEFORE=$(state_hash "$DEFAULT_DISK" "${DEFAULT_STATE% *}" "${DEFAULT_STATE#* }")
run_update defaults-update "$DEFAULT_DISK" applied
require_record defaults-update \
    "[PRODUCTION-INSTALL] state preserve preflight=VERIFIED"
require_record defaults-update "current=${DEFAULT_STATE% *}+${DEFAULT_STATE#* }"
require_record defaults-update "plan=${DEFAULT_STATE% *}+${DEFAULT_STATE#* }"
require_record defaults-update "loader=READY config=DEFAULT admin=ABSENT"
require_record defaults-update "[PRODUCTION-INSTALL] state preserved=VERIFIED"
STATE_AFTER=$(state_hash "$DEFAULT_DISK" "${DEFAULT_STATE% *}" "${DEFAULT_STATE#* }")
[[ "$STATE_BEFORE" == "$STATE_AFTER" ]] || fail \
    "the update changed the state partition bytes of a default-only volume"
printf '  ok   defaults: the state partition bytes are unchanged\n'
staged_kernel_matches "$DEFAULT_DISK" "${DEFAULT_BOOT% *}" defaults
boot_installed defaults-installed "$DEFAULT_DISK" "[APPLIANCE-BOOT]"
stop_qemu
require_record defaults-installed "[APPLIANCE-BOOT] runtime=READY"
reject_record defaults-installed "[PRODUCTION-INSTALL] applied"

# ------------------------------------------------------------------ SSH KEY
# ITEM-606: the owner-confirmed public key travels as a medium module, is
# written into the installed state by the update, and the installer's read-back
# through the installed reader names the fingerprint.  The state record itself
# is checked from the disk image.  The private key never reaches the guest.
echo "SSH KEY: an owner-confirmed public key is imported into the installed state"
KEY_DISK=$(install_whole_disk ssh-key "$INSTALL_IMAGE" 536870912)
KEY_STATE=$(plan_role ssh-key hfoc-persistence) || true
[[ -n "$KEY_STATE" ]] || fail "the key install published no persistence range"
run_update ssh-key-update "$KEY_DISK" applied "$SSH_KEY_UPDATE_IMAGE"
require_record ssh-key-update \
    "[PRODUCTION-INSTALL] ssh-key fingerprint=${SSH_KEY_FINGERPRINT} persisted=VERIFIED"
python3 - "$KEY_DISK" "${KEY_STATE% *}" <<'PY'
import sys

disk, first_lba = sys.argv[1], int(sys.argv[2])
# CERT namespace starts at block 192 of the persistence partition; the
# authorized-key record is after the TLS record (12), the name table (32), the
# ACME control record (1) and the host-key record (4).
record_lba = first_lba + 192 + 12 + 32 + 1 + 4
with open(disk, "rb") as image:
    image.seek(record_lba * 512)
    header = image.read(8)
if header != b"HFSSHAUT":
    raise SystemExit("the installed state has no HFSSHAUT record")
PY
printf '  ok   ssh-key: the installed state carries the imported authorized key\n'

# ------------------------------------------------------------------ CAPACITY
# ITEM-638: the owner's update declaration is 540 bytes and the full standard
# declaration with four forward clients is 580; the old 512-byte parser bound
# refused it as TOKEN_MISSING before any disk was touched.  This is the real
# media path: install an appliance, apply the >512-byte LAYOUT=update medium,
# read the applied/preserved records, boot the installed kernel, then prove an
# oversized declaration is refused by name with the disk byte for byte intact.
echo "CAPACITY: a >512-byte standard update declaration is applied, not refused"
CAPACITY_DISK=$(install_whole_disk capacity "$INSTALL_IMAGE" 536870912)
CAPACITY_BOOT=$(plan_role capacity boot-volume) || true
[[ -n "$CAPACITY_BOOT" ]] || fail "the capacity install published no boot volume range"
run_update capacity-update "$CAPACITY_DISK" applied "$CAPACITY_UPDATE_IMAGE"
require_record capacity-update \
    "[PRODUCTION-INSTALL] media token=PRESENT confirm=$CAPACITY_CONFIRM target=$CAPACITY_TARGET management=management-interface mode=update network=static address=$CAPACITY_ADDRESS"
require_record capacity-update "forward=on target=$CAPACITY_FORWARD_TARGET"
require_record capacity-update "clients=$CAPACITY_FORWARD_CLIENTS"
require_record capacity-update \
    "[PRODUCTION-INSTALL] state preserve preflight=VERIFIED"
require_record capacity-update "[PRODUCTION-INSTALL] state preserved=VERIFIED"
staged_kernel_matches "$CAPACITY_DISK" "${CAPACITY_BOOT% *}" capacity
boot_installed capacity-installed "$CAPACITY_DISK" "[APPLIANCE-BOOT]"
stop_qemu
require_record capacity-installed "[APPLIANCE-BOOT] runtime=READY"
reject_record capacity-installed "[PRODUCTION-INSTALL] applied"

echo "CAPACITY-OVERSIZED: a declaration over the finite bound is refused, zero writes"
OVERSIZED_DISK=$(install_whole_disk oversized "$INSTALL_IMAGE" 536870912)
OVERSIZED_BEFORE=$(disk_fingerprint "$OVERSIZED_DISK")
run_update oversized-update "$OVERSIZED_DISK" refused \
    "$OVERSIZED_UPDATE_IMAGE" TOKEN_TOO_LARGE
require_record oversized-update \
    "[PRODUCTION-INSTALL] refused reason=TOKEN_TOO_LARGE writes=0"
# The refusal is terminal for the installer, but the machine then continues to
# the installed appliance.  That later boot legitimately writes its runtime
# state, so the negative phase freezes the vCPU at the refusal record; this
# record proves it did not slip through into the next boot and contaminate the
# comparison.  ITEM-646.
if grep -aEq '^HFOC OS .* INITIALISED\r?$' "$TEST_DIR/oversized-update.log"; then
    fail "the oversized-declaration refusal ran on into the installed appliance"
fi
OVERSIZED_AFTER=$(disk_fingerprint "$OVERSIZED_DISK")
[[ "$OVERSIZED_BEFORE" == "$OVERSIZED_AFTER" ]] || fail \
    "the oversized-declaration refusal wrote to the disk: before='$OVERSIZED_BEFORE' after='$OVERSIZED_AFTER'"
printf '  ok   capacity: the oversized refusal wrote nothing\n'

# ------------------------------------------------------------------ SETTINGS
echo "SETTINGS: the language the install wrote into the state survives the update"
SETTINGS_DISK=$(install_whole_disk settings "$LANGUAGE_IMAGE" 536870912)
SETTINGS_STATE=$(plan_role settings hfoc-persistence) || true
[[ -n "$SETTINGS_STATE" ]] || fail "the language install published no persistence range"
require_record settings "[PRODUCTION-INSTALL] language=ru"
run_update settings-update "$SETTINGS_DISK" applied
require_record settings-update \
    "[PRODUCTION-INSTALL] state preserve preflight=VERIFIED"
require_record settings-update "loader=READY config=PRESENT"
boot_installed settings-installed "$SETTINGS_DISK" "[APPLIANCE-BOOT]"
require_record settings-installed \
    "[WORKSPACE] state=OK range=514+32768 prefix=/home/owner hostseed=SEPARATE"
if grep -aFq -- 'Пароль:' "$TEST_DIR/settings-installed.log"; then
    fail "settings-installed: the workspace fabricated a host seed and started the entropy login"
fi
type_keys settings-installed "$TEST_DIR/settings-installed.monitor" "language"
wait_raw_marker settings-installed "ЯЗЫК" 60
printf '  ok   settings: the installed console still answers in the preserved language\n'
python3 - "$SETTINGS_DISK" "${SETTINGS_STATE% *}" <<'PY'
import sys

disk, first_lba = sys.argv[1], int(sys.argv[2])
block = 512
with open(disk, "rb") as image:
    image.seek((first_lba + 512) * block)
    host_seed = image.read(2 * block)
    image.seek((first_lba + 514) * block)
    magic = image.read(7)
if host_seed != b"\x00" * (2 * block):
    raise SystemExit("the installed workspace changed the host-seed slots")
if magic != b"HFOCFS1":
    raise SystemExit("the installed workspace image is missing at block 514")
PY
printf '  ok   settings: the installed workspace starts at 514 and leaves the host seed alone\n'
stop_qemu

# --------------------------------------------------------------------- RANGE
echo "RANGE: a moved persistence range is refused before the first write"
RANGE_DISK=$(install_whole_disk range "$INSTALL_IMAGE" 536870912)
RANGE_STATE=$(plan_role range hfoc-persistence) || true
[[ -n "$RANGE_STATE" ]] || fail "the install published no persistence range"
RANGE_FIRST=${RANGE_STATE% *}
RANGE_LENGTH=${RANGE_STATE#* }
python3 - "$RANGE_DISK" "$RANGE_FIRST" <<'PY'
import struct
import sys
import zlib

# Move the persistence entry into free space well past the plan's footprint,
# keeping its length, in *both* GPT copies, and rebuild the entry-array and
# header checksums the reader verifies.  The table stays valid - no overlap, no
# bad bounds - and only the range differs from the one this plan writes, which
# is the case the preflight must refuse with both ranges named.
SECTOR = 512
ENTRY_COUNT = 128
ENTRY_SIZE = 128
RELOCATION_LBA = 400000


def read_entries(disk, lba, entry_count, entry_size):
    disk.seek(lba * SECTOR)
    return bytearray(disk.read(entry_count * entry_size))


def relocate(entries, first_lba):
    for index in range(ENTRY_COUNT):
        offset = index * ENTRY_SIZE
        first, last = struct.unpack_from("<QQ", entries, offset + 32)
        if first == first_lba and last > first:
            length = last - first + 1
            struct.pack_into("<QQ", entries, offset + 32, RELOCATION_LBA,
                             RELOCATION_LBA + length - 1)
            return index
    raise SystemExit("the persistence entry was not found in the GPT")


def rebuild(disk, header_lba):
    disk.seek(header_lba * SECTOR)
    header = bytearray(disk.read(SECTOR))
    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    entries = read_entries(disk, entries_lba, entry_count, entry_size)
    moved = relocate(entries, int(sys.argv[2]))
    disk.seek(entries_lba * SECTOR)
    disk.write(bytes(entries))
    struct.pack_into("<I", header, 88, zlib.crc32(bytes(entries)) & 0xFFFFFFFF)
    header_size = struct.unpack_from("<I", header, 12)[0]
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 16,
                     zlib.crc32(bytes(header[:header_size])) & 0xFFFFFFFF)
    disk.seek(header_lba * SECTOR)
    disk.write(bytes(header))
    return moved


with open(sys.argv[1], "r+b") as disk:
    primary = rebuild(disk, 1)
    disk.seek(SECTOR)
    alternate_lba = struct.unpack_from("<Q", disk.read(SECTOR), 32)[0]
    backup = rebuild(disk, alternate_lba)
    print("relocated GPT entry", primary + 1,
          "and its copy", backup + 1, "at", alternate_lba,
          "to", RELOCATION_LBA)
PY
RANGE_FINGERPRINT_BEFORE=$(disk_fingerprint "$RANGE_DISK")
run_update range-update "$RANGE_DISK" refused
require_record range-update \
    "[PRODUCTION-INSTALL] state preserve refused=RANGE_MISMATCH"
require_record range-update "current=PRESENT 400000+$RANGE_LENGTH"
require_record range-update "plan=$RANGE_FIRST+$RANGE_LENGTH"
require_record range-update "writes=0"
require_record range-update \
    "refused reason=CONTROLLER status=PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE writes=0"
RANGE_FINGERPRINT_AFTER=$(disk_fingerprint "$RANGE_DISK")
[[ "$RANGE_FINGERPRINT_BEFORE" == "$RANGE_FINGERPRINT_AFTER" ]] || fail \
    "the refused update wrote to the disk"
printf '  ok   range: the refused run wrote nothing\n'

# ---------------------------------------------------------------- UNREADABLE
echo "UNREADABLE: a state partition the reader cannot load is refused before the first write"
BROKEN_DISK=$(install_whole_disk broken "$INSTALL_IMAGE" 536870912)
BROKEN_STATE=$(plan_role broken hfoc-persistence) || true
[[ -n "$BROKEN_STATE" ]] || fail "the install published no persistence range"
dd if=/dev/zero of="$BROKEN_DISK" bs=512 \
    seek="${BROKEN_STATE% *}" count=2048 conv=notrunc status=none
BROKEN_FINGERPRINT_BEFORE=$(disk_fingerprint "$BROKEN_DISK")
run_update broken-update "$BROKEN_DISK" refused
require_record broken-update \
    "[PRODUCTION-INSTALL] state preserve refused=LOADER_NOT_READY"
require_record broken-update "writes=0"
BROKEN_FINGERPRINT_AFTER=$(disk_fingerprint "$BROKEN_DISK")
[[ "$BROKEN_FINGERPRINT_BEFORE" == "$BROKEN_FINGERPRINT_AFTER" ]] || fail \
    "the refused unreadable-state update wrote to the disk"
printf '  ok   unreadable: the refused run wrote nothing\n'

echo "QEMU production install update test: OK"
