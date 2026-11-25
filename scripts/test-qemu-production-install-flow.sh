#!/usr/bin/env bash
# ITEM-394 QEMU proof for the product install run on ordinary boot.
# ITEM-399: and for the fact that the disk it installs runs the *product*
# appliance, with the management scope the medium declared.
#
# The existing production-installer proof drives the controller in-process
# under the QEMU fixture.  This proof boots the installer medium for real, on
# UEFI firmware (OVMF), with an AHCI disk attached, and checks:
#
#   1. an authorised medium installs the declared layout and publishes the
#      ITEM-391 numbers, and the GPT on the disk matches the published plan
#      (read back by this script from the raw image, not from the kernel);
#   2. an unauthorised medium refuses with zero writes and the disk is
#      byte-identical afterwards;
#   3. an authorised medium with no matching disk refuses with zero writes;
#   4. an authorised medium whose disk is below the declared minimum refuses
#      with zero writes;
#   5. ITEM-517: the same authorised medium booted on legacy BIOS (SeaBIOS,
#      QEMU's default) installs the BIOS layout and the measured Limine MBR
#      stage recipe, and the disk it installed boots on its own without UEFI
#      to the sealed chain attestation and the ordinary readiness record;
#   6. ITEM-399: the disk the run installed boots the *installed* kernel the
#      medium carried - the product appliance build, with no
#      `[PRODUCTION-INSTALLER]` record and no QEMU-fixture record - reaches the
#      sealed chain attestation and the ordinary readiness record, and carries
#      on its ESP the configuration the medium sealed, `cmdline:` scope
#      included;
#   7. ITEM-399: a medium that declared `management-interface` installs a disk
#      whose boot publishes `scope=MANAGEMENT_INTERFACE
#      source=boot-declaration`, while the `local-only` medium yields the local
#      scope - the installed configuration states the scope, it is not a
#      default.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate sha256sum head stat; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_INSTALL_FLOW_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALL_FLOW_TEST_TIMEOUT:-600}s" \
        env HFOC_INSTALL_FLOW_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
PLAIN_IMAGE=${PLAIN_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer.iso}
AUTHORISED_IMAGE=${AUTHORISED_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-authorised.iso}
AUTHORISED_ISO_ROOT=${AUTHORISED_ISO_ROOT:-build/production-installer/iso_root-authorised}
MANAGEMENT_IMAGE=${MANAGEMENT_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-management.iso}
# ITEM-446: the medium that declares a static network, and the declaration it
# carries.  The values are the QEMU user-mode network, so the installed disk can
# prove the address without a DHCP exchange.
NETWORK_IMAGE=${NETWORK_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-network.iso}
NETWORK_ISO_ROOT=${NETWORK_ISO_ROOT:-build/production-installer/iso_root-network}
NETWORK_ADDRESS=${HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS:-10.0.2.15/24}
NETWORK_GATEWAY=${HFOC_INSTALL_MEDIA_NETWORK_PROOF_GATEWAY:-10.0.2.2}
NETWORK_DNS=${HFOC_INSTALL_MEDIA_NETWORK_PROOF_DNS:-10.0.2.3}
# ITEM-447: the address the menu sets, which the medium did not declare.
NETWORK_MENU_ADDRESS=${HFOC_MENU_NETWORK_PROOF_ADDRESS:-10.0.2.16/24}
MANAGEMENT_ISO_ROOT=${MANAGEMENT_ISO_ROOT:-build/production-installer/iso_root-management}
# ITEM-449: the medium that declares a forward, and the rule it carries.  The
# installed disk has to state the rule it was installed with - the relay half
# of the item is what serves it, and this proof is the declaration reaching the
# appliance.
FORWARD_IMAGE=${FORWARD_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-forward.iso}
FORWARD_ISO_ROOT=${FORWARD_ISO_ROOT:-build/production-installer/iso_root-forward}
FORWARD_TARGET=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET:-10.0.2.2}
FORWARD_TARGET_PORT=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT:-18443}
FORWARD_LISTEN_PORT=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT:-19443}
FORWARD_CLIENTS=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS:-10.0.2.2}
# ITEM-509 increment 27 (509.7c-2): the medium that declares Russian.  It
# carries the same authorised declaration plus `HFOC-INSTALL-LANGUAGE=ru`; the
# install has to write that language into the installed state, read it back
# through the installed system's own reader and publish the persisted record.
# Every other medium here declares nothing and keeps its exact token bytes, so
# the record checks of the English cases do not move.
LANGUAGE_IMAGE=${LANGUAGE_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-language.iso}
LANGUAGE_ISO_ROOT=${LANGUAGE_ISO_ROOT:-build/production-installer/iso_root-language}
# ITEM-405: the medium that declares the destructive layout.  The stale-ESP case
# is a whole-disk install - the acceptance host's disk carried another system,
# and the operator's medium declared that the disk was expendable - so it needs
# the declared medium rather than the undeclared one, which would preserve what
# it found.
WHOLE_DISK_IMAGE=${WHOLE_DISK_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-whole-disk.iso}
WHOLE_DISK_ISO_ROOT=${WHOLE_DISK_ISO_ROOT:-build/production-installer/iso_root-whole-disk}
AUTHORISED_TARGET=${HFOC_INSTALL_MEDIA_TARGET:-QM00001}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
# The installer policy requires at least 2 GiB of usable memory, so the
# proof machine is given 4 GiB; the record names the observed figure.
QEMU_MEMORY=${QEMU_MEMORY:-3072M}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Install-flow test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Install-flow test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Install-flow test must not set QEMU_CPU" >&2
    exit 1
}
for path in "$PLAIN_IMAGE" "$AUTHORISED_IMAGE" "$MANAGEMENT_IMAGE" \
    "$NETWORK_IMAGE" \
    "$AUTHORISED_ISO_ROOT/boot/hfoc-installed.conf" \
    "$MANAGEMENT_ISO_ROOT/boot/hfoc-installed.conf" \
    "$NETWORK_ISO_ROOT/boot/hfoc-installed.conf" \
    "$WHOLE_DISK_IMAGE" \
    "$WHOLE_DISK_ISO_ROOT/boot/hfoc-installed.conf" \
    "$OVMF_CODE" "$OVMF_VARS"; do
    [[ -f "$path" ]] || {
        echo "Missing required file: $path" >&2
        exit 1
    }
done

# The declared acceptance disk class: 234375000 sectors, the same media the
# ITEM-391 contract proof publishes.
ACCEPTANCE_BYTES=$((234375000 * 512))
SMALL_BYTES=$((64 * 1024 * 1024))

TEST_DIR=$(mktemp -d)
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
FAILURE_MESSAGE=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/production-install-flow-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-Install-flow QEMU proof failed}" \
            >"$FAILURE_REASON"
        cp -a "$TEST_DIR"/. "$artifact_dir/" 2>/dev/null || true
        echo "Install-flow artifacts: $artifact_dir" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# A 120 GB sparse image is never hashed whole: the fingerprint is the image
# size, its allocated block count (any write anywhere allocates) and the hash of
# the first 4 MiB, which carries the GPT header and the partition array.  A
# refused run must reproduce every one of them.
disk_fingerprint() {
    local path=$1

    printf '%s %s\n' "$(stat -c '%s %b' "$path")" \
        "$(head -c 4194304 "$path" | sha256sum | awk '{print $1}')"
}


# run_boot LABEL FIRMWARE IMAGE DISK_SERIAL DISK_BYTES EXPECT_MARKER [LAYOUT]
#
# ITEM-409: a caller that has to put a second disk on the machine - the
# acceptance server's Debian disk, which this install must offer and must never
# write - fills `RUN_BOOT_EXTRA` with the extra `-drive`/`-device` arguments
# before the call and clears it afterwards.  Every other case runs with the
# empty array.
RUN_BOOT_EXTRA=()
#
# ITEM-409: what the run must publish about the menu it installs.  The defaults
# are the single-disk machine (the appliance alone); a caller that puts a second
# disk on the machine sets its own, because the menu then offers that disk's
# system as well.  `|` separates the fragments.
RUN_BOOT_MENU_EXPECT='state=GENERATED reason=appliance-only stage=no-other-disk|entries=1|loaders=0|disks=1|target=1|d1=NONE/esp0/NO_ESP/0'
RUN_BOOT_ENTRY_EXPECT='menu=\EFI\BOOT\limine.conf entries=1 timeout=10s source=GENERATED payload_set=7'
#

# run_boot LABEL FIRMWARE IMAGE DISK_SERIAL DISK_BYTES EXPECT_MARKER [LAYOUT] [RESET]
#
# RESET is `reset` for a run that must complete and reboot the machine, and
# `no-reset` (the default) for a run that must not.
#
# ITEM-411: this harness runs the guest with `-no-reboot` and **without**
# `-no-shutdown`, so a guest reset request makes QEMU exit with status 0 and the
# reset the flow performs after a completed install is observable as the guest
# going away - its absence as the guest staying alive.  The pair matters: QEMU
# maps `-no-reboot` to `reboot=shutdown` and `-no-shutdown` to
# `shutdown=pause`, so with both flags a guest reset moves the machine to
# `RUN_STATE_SHUTDOWN` and pauses it instead of ending the run, which is
# indistinguishable from a machine that never reset.  Neither flag is needed for
# the refusal runs: nothing in the kernel shuts the machine down, and a panic is
# a halted loop, so the guest stays up.
run_boot() {
    local label=$1
    local firmware=$2
    local image=$3
    local serial=$4
    local disk_bytes=$5
    local expect=$6
    local layout=${7:-}
    local expect_reset=${8:-no-reset}
    local disk=$TEST_DIR/$label.raw
    local serial_log=$TEST_DIR/$label.log
    local vars=$TEST_DIR/$label-vars.fd
    local before after
    local output
    local qemu_status=0

    # ITEM-405: a caller that prepared the target image before this call - the
    # stale-ESP case writes a foreign volume into the region the whole-disk
    # layout declares as its ESP role - keeps it.  Every other case starts from
    # a fresh sparse image of the declared size.
    if [[ -f "$disk" ]]; then
        [[ "$(stat -c '%s' "$disk")" == "$disk_bytes" ]] || {
            echo "$label: prepared disk has the wrong size" >&2
            return 1
        }
    else
        truncate -s "$disk_bytes" "$disk"
    fi
    cp "$OVMF_VARS" "$vars"
    before=$(disk_fingerprint "$disk")
    : >"$serial_log"

    if [[ "$firmware" == uefi ]]; then
        qemu-system-x86_64 \
            -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
            -display none -cdrom "$image" \
            -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
            -drive "if=pflash,format=raw,file=$vars" \
            -drive "if=none,id=target,file=$disk,format=raw,cache=writeback,file.locking=off" \
            -device "ide-hd,drive=target,bus=ide.0,serial=$serial" \
            "${RUN_BOOT_EXTRA[@]}" \
            -serial "file:$serial_log" -no-reboot \
            >"$QEMU_LOG" 2>&1 &
    else
        qemu-system-x86_64 \
            -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
            -display none -cdrom "$image" \
            -drive "if=none,id=target,file=$disk,format=raw,cache=writeback,file.locking=off" \
            -device "ide-hd,drive=target,bus=ide.0,serial=$serial" \
            "${RUN_BOOT_EXTRA[@]}" \
            -serial "file:$serial_log" -no-reboot \
            >"$QEMU_LOG" 2>&1 &
    fi
    local qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$expect" \
        "$expect_reset" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import signal
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
qemu_pid = int(sys.argv[2])
expect = sys.argv[3]
expect_reset = sys.argv[4]
deadline = time.monotonic() + 90.0


def declare_marker_timeout(marker, timeout):
    '''ITEM-456: declare the timing class the suite retries.

    A proof that stopped because it did not observe a marker inside its own
    deadline is not the same event as one whose assertion failed: the guest may
    be healthy and merely starved by a loaded TCG runner.  The suite retries
    exactly the targets that write this one strict record
    (`scripts/hfoc-qemu-failure-class.sh`).
    '''
    sys.stderr.write('HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ds\n'
                     % (marker, int(timeout)))
    sys.stderr.flush()

# ITEM-410: an install that succeeds attaches the boot log to the disk it just
# staged and writes the install records into it.  The completion record is
# published after the device flush, so waiting for it is what makes the
# read-back below deterministic instead of a race with the kill that ends this
# phase.  A run that refuses never reaches the attach, so a refusal is waited
# for on its own record.
install_log = re.compile(
    r"\[BOOTLOG\] state=READY .*reason=install .*flushed=[1-9]")
expects_log = "refused" not in expect


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def exited():
    """True once QEMU is really gone.

    A finished child is a zombie until the shell collects it, and
    `os.kill(pid, 0)` succeeds for a zombie, so the process table alone
    cannot tell a QEMU that just exited from one that is still running.
    `/proc/<pid>/stat` does: the state field is `Z` for a zombie and the
    file is gone once the process is reaped.
    """
    try:
        stat = pathlib.Path(f"/proc/{qemu_pid}/stat").read_text()
    except OSError:
        return True
    # Field 2 is the command in parentheses and may itself contain spaces
    # or parentheses, so the state is the first field after the last ')'.
    return stat.rpartition(")")[2].split()[:1] == ["Z"]


def running():
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        return False
    return not exited()


while time.monotonic() < deadline:
    text = serial_text()
    if expect in text and (not expects_log or install_log.search(text)):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before the install record")
    # ITEM-413: a flow that stops short publishes the stage and the status it
    # stopped at, and never resets.  Waiting out the deadline for a record that
    # cannot come would hide that line, so the wait ends on it instead.
    if "[PRODUCTION-INSTALL] failed stage=" in text:
        raise SystemExit(
            "the install flow stopped before the record this case waits "
            "for: " +
            [line for line in text.splitlines()
             if line.startswith("[PRODUCTION-INSTALL] failed ")][-1])
    if not running():
        raise SystemExit(f"QEMU exited before: {expect}")
    time.sleep(0.05)
else:
    declare_marker_timeout(expect, 90)
    raise SystemExit(
        f"timed out waiting for: {expect} and the appliance boot log flush")


if expect_reset == "reset":
    # ITEM-411: the completed install must reboot the machine.  QEMU runs with
    # -no-reboot and without -no-shutdown, so a guest reset makes it exit on its
    # own: waiting for that exit is the proof that the flow reset the machine,
    # and a guest still running after its timeout is the proof that it did not.
    exit_deadline = time.monotonic() + 60.0
    while time.monotonic() < exit_deadline:
        if not running():
            break
        time.sleep(0.05)
    else:
        try:
            os.kill(qemu_pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        raise SystemExit(
            "the machine did not reboot itself after the completed install; "
            "QEMU is still running after: " + expect + "\n" +
            serial_text()[-3000:])
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("kernel panic during the reset after the install")
else:
    # A run that refused or failed must keep its behaviour: the machine stays
    # up, so QEMU is still running and no reset happened.
    time.sleep(3.0)
    if not running():
        raise SystemExit(
            "QEMU exited after a run that must not reset the machine\n" +
            serial_text()[-3000:])
    try:
        os.kill(qemu_pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
PY
    ); then
        FAILURE_MESSAGE="$label: $output"
        printf '%s\n' "$output" >&2
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || qemu_status=$?
    # A guest reset ends QEMU with status 0 under -no-reboot; anything else is
    # not the reset this proof claims.
    if [[ "$expect_reset" == reset && "$qemu_status" != 0 ]]; then
        FAILURE_MESSAGE="$label: QEMU exited with status $qemu_status after the completed install, expected 0 (a guest reset)"
        printf '%s\n' "$FAILURE_MESSAGE" >&2
        return 1
    fi
    after=$(disk_fingerprint "$disk")

    if ! python3 - "$label" "$serial_log" "$disk" "$before" "$after" \
        "$expect" "$serial" "$layout" "$expect_reset" "$RUN_BOOT_MENU_EXPECT" \
        "$RUN_BOOT_ENTRY_EXPECT" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import hashlib
import pathlib
import re
import struct
import sys
import zlib


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

label, serial_path, disk_path, before, after, expect, serial, layout, \
    expect_reset = sys.argv[1:10]
menu_expect = sys.argv[10].split("|")
entry_expect = sys.argv[11]
text = SERIAL_VIEW.normalise(
    pathlib.Path(serial_path).read_bytes().decode("ascii", errors="replace"))
with open(disk_path, "rb") as handle:
    disk = handle.read(1024 * 1024)


def require(condition, message):
    if not condition:
        raise SystemExit(f"{label}: {message}")


def read_at(handle, offset, length):
    handle.seek(offset)
    data = handle.read(length)
    require(len(data) == length,
            f"the image is shorter than {offset + length} bytes")
    return data


def limine_stage_blob():
    """The pinned 22,516-byte stage image `binary_limine_hdd_bin_data`.

    It is parsed out of `deps/limine/limine-bios-hdd.h`, the header the
    installer kernel includes, so the raw-image assertion below compares LBA0
    with the same bytes the kernel wrote rather than with a literal only this
    proof carries.
    """
    source = pathlib.Path("deps/limine/limine-bios-hdd.h").read_text()
    body = source.split("{", 1)[1].rsplit("}", 1)[0]
    data = bytearray()
    for token in body.split(","):
        token = token.strip()
        if not token:
            continue
        data.append(int(token, 16) if token.lower().startswith("0x")
                    else int(token, 10))
    return bytes(data)


def gpt_crc32(image):
    return zlib.crc32(image) & 0xFFFFFFFF


require(expect in text, f"missing marker: {expect}")
require("*** HFOC KERNEL PANIC ***" not in text, "kernel panic detected")

# The media declaration and the candidate list are published before any
# decision, and the operator's disk is the one marked selected.
require(text.count("[PRODUCTION-INSTALL] media token=") == 1,
        "expected exactly one media declaration record")

# ITEM-411: a completed install publishes one completion record and reboots
# through a mechanism the plan names; a run that must not reset publishes
# neither.
plan_lines = [line for line in text.splitlines()
              if line.startswith("[PLATFORM-RESET] plan ")]
attempt_lines = [line for line in text.splitlines()
                 if line.startswith("[PLATFORM-RESET] attempt=")]
complete_lines = [line for line in text.splitlines()
                  if line.startswith("[PRODUCTION-INSTALL] complete ")]
mechanisms = {"ACPI", "PCI", "I8042", "FAULT"}

if expect_reset == "reset":
    require(len(complete_lines) == 1,
            f"expected exactly one completion record, got "
            f"{len(complete_lines)}")
    complete = complete_lines[0]
    require(complete.startswith(
        f"[PRODUCTION-INSTALL] complete disk=1 identity={serial} "
        f"medium=REMOVE reset="),
        f"the completion record does not name the disk, the medium and the "
        f"reset: {complete}")
    selected = complete.split(" reset=", 1)[1].split(" ", 1)[0]
    require(selected in mechanisms,
            f"the completion record names no declared reset mechanism: "
            f"{complete}")
    # ITEM-417: the record ends with what the operator is shown, so the
    # countdown is read where it sits rather than at the end of the line.
    countdown = re.search(r" in=(\d+)s ", complete + " ")
    require(countdown is not None and int(countdown.group(1)) >= 1,
            f"the completion record does not state a reboot countdown: "
            f"{complete}")
    require(re.search(r" screen=(BANNER|NONE)$", complete) is not None,
            f"the completion record does not state what the operator was "
            f"shown: {complete}")
    require(len(plan_lines) == 1,
            f"expected exactly one reset plan record, got {len(plan_lines)}")
    plan = plan_lines[0]
    require(" reset=" in plan and " steps=" in plan,
            f"the reset plan does not name its mechanism and steps: {plan}")
    steps = plan.split(" steps=", 1)[1].split(" ", 1)[0].split(">")
    require(steps[-1] == "FAULT",
            f"the reset plan does not end in the fault: {plan}")
    require(all(step in mechanisms for step in steps),
            f"the reset plan names no declared reset mechanism: {plan}")
    require(len(set(steps)) == len(steps),
            f"the reset plan tries a mechanism twice: {plan}")
    require(plan.split(" reset=", 1)[1].split(" ", 1)[0] == selected,
            f"the completion record and the reset plan disagree: "
            f"{complete} / {plan}")
    require(len(attempt_lines) >= 1,
            "no reset mechanism was attempted")
    for attempt in attempt_lines:
        require(attempt.split(" reset=", 1)[1].split(" ", 1)[0] in mechanisms,
                f"an attempt names no declared reset mechanism: {attempt}")
    # The last attempt is the mechanism the platform honoured: a mechanism that
    # resets the machine never returns to publish the next one.
    last = attempt_lines[-1]
    print(f"{label}: install completed, reset through "
          f"{last.split(' reset=', 1)[1].split(' ', 1)[0]} "
          f"({len(attempt_lines)} attempt(s), plan: "
          f"{plan.strip()})")
else:
    require(not complete_lines,
            "a run that must not reset published a completion record")
    require(not plan_lines and not attempt_lines,
            "a run that must not reset planned or attempted a reset")

if "refused" in expect or "failed" in expect:
    require(serial in text,
            "the refusal does not name the disk that was scanned")
    require(before == after,
            "a refused run modified the target disk")
    require("writes=0" in text,
            "a refused run does not state zero writes")
    require("planned_layout=ABSENT" not in text,
            "unreachable")

# ITEM-517: the plan mode the BIOS contract publishes selects the BIOS branch
# below.  A UEFI run never carries it, so the UEFI assertions are exactly the
# ones they always were.
bios_install = "[INSTALLER-LAYOUT] plan mode=clean-whole-disk-bios" in text

if expect.startswith("[PRODUCTION-INSTALL] applied") and not bios_install:
    # The installed footprint is the ITEM-391 contract: one plan record, four
    # role records, and the numbers below.  The GPT on the disk is parsed here,
    # independently of the kernel, and must reproduce every one of them.
    plan_lines = [line for line in text.splitlines()
                  if line.startswith("[INSTALLER-LAYOUT] plan ")]
    role_lines = [line for line in text.splitlines()
                  if line.startswith("[INSTALLER-LAYOUT] role=")]
    require(len(plan_lines) == 1,
            f"expected 1 layout plan record, got {len(plan_lines)}")
    require(len(role_lines) == 4,
            f"expected 4 layout role records, got {len(role_lines)}")
    require("[PRODUCTION-INSTALLER] proof init" not in text,
            "the QEMU fixture ran in a product install boot")
    for fragment in (
            "mode=clean-whole-disk",
            "media_lba=234375000",
            "minimum_media_lba=331810",
            "declared_lba=329728",
            "declared_mib=161",
            "first_lba=2048",
            "footprint_lba=329728",
            "footprint_mib=161",
    ):
        require(fragment in plan_lines[0],
                f"layout plan record is missing {fragment}")
    expected_roles = {
        "efi-system": (2048, 67584),
        "hfoc-system": (69632, 131072),
        "hfoc-persistence": (200704, 65536),
        "hfoc-recovery": (266240, 65536),
    }
    for role_line in role_lines:
        match = re.match(
            r"\[INSTALLER-LAYOUT\] role=(\S+) first_lba=(\d+) "
            r"length_lba=(\d+) length_mib=(\d+) reused=(\d+)",
            role_line)
        require(match is not None, f"unparsable role record: {role_line}")
        role = match.group(1)
        require(role in expected_roles, f"unknown role: {role}")
        first, length = expected_roles[role]
        require(int(match.group(2)) == first and
                int(match.group(3)) == length,
                f"{role} does not carry the contracted geometry")

    require("[PRODUCTION-INSTALL] media sealed=VERIFIED" in text,
            "the media manifest was not verified")
    require("[PRODUCTION-INSTALL] esp payload=VERIFIED" in text,
            "the ESP boot payload was not written")
    # ITEM-398: the whole-disk target disk here carries nothing at all, so the
    # run takes that path and says so - before the first write - with the reason
    # and the fact that the operator declared nothing.  ITEM-405: a run driven
    # by the medium that declares the destructive layout spells that out
    # instead, and passes its own record here.
    layout_record = layout or ("[PRODUCTION-INSTALL] layout mode=whole-disk "
                               "reason=no-existing-system declared=0")
    require(layout_record in text,
            "the layout decision is not published before the first write")
    require(text.index("[PRODUCTION-INSTALL] layout mode=") <
            text.index("[INSTALLER-LAYOUT] plan "),
            "the layout decision is published after the first write")
    # ITEM-397: the product flow stages the installed boot set through the
    # boot stage and publishes exactly one boot-entry record naming the
    # mechanism that makes the firmware find the disk.
    #
    # ITEM-409: the record states *what* configuration the loader reads and how
    # many entries it offers, because the generated menu is the installed
    # configuration for every install now - a machine that carries nothing but
    # this appliance included.  `entries=1` is the appliance entry on its own,
    # and the timeout is the one the menu declares.
    boot_entry_lines = [line for line in text.splitlines()
                        if line.startswith("[PRODUCTION-INSTALL] boot-entry ")]
    require(len(boot_entry_lines) == 1,
            f"expected exactly one boot-entry record, got "
            f"{len(boot_entry_lines)}")
    record = boot_entry_lines[0]
    require(record.startswith(
        "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
        "mechanism=REMOVABLE_MEDIA_PATH "
        "path=\\EFI\\BOOT\\BOOTX64.EFI "
        "kernel=\\boot\\hfoc-kernel.elf "
        "config=\\boot\\limine\\limine.conf "),
        f"the boot-entry record does not carry the staged set: {record}")
    require(entry_expect in record,
            f"the boot-entry record does not state the installed "
            f"configuration: {record}")
    # ITEM-409: the record is the one place the operator (and the acceptance
    # run) reads which configuration was installed, so the sealed fallback can
    # never be published as if it were a menu.
    require("menu_reason=" not in record,
            f"the record claims the sealed fallback for a generated menu: "
            f"{record}")
    # ITEM-409: the flow publishes what the menu offers and which stage decided
    # when it offers nothing but this appliance, before the first write.
    boot_menu_lines = [line for line in text.splitlines()
                       if line.startswith("[PRODUCTION-INSTALL] boot-menu ")]
    require(len(boot_menu_lines) == 1,
            f"expected exactly one boot-menu record, got "
            f"{len(boot_menu_lines)}")
    menu_record = boot_menu_lines[0]
    for fragment in menu_expect:
        require(fragment in menu_record,
                f"the boot-menu record is missing {fragment}: {menu_record}")
    # ITEM-423: a menu that offers fewer systems than the machine carries has to
    # say which of the three causes it was - no controller matched, a matched
    # controller that stopped inside the NVMe bring-up, or a controller that
    # produced no namespace - so the record names what the probe saw.
    # ITEM-429 adds the step and the wait: a timeout is a controller that never
    # reported ready or a driver that never waited, and the record says which.
    for fragment in ("pci_ahci=", "pci_nvme=", " ahci=", " nvme=",
                     "nvme_status=", "nvme_step=", "nvme_wait_ms="):
        require(fragment in menu_record,
                f"the boot-menu record does not name the probe evidence "
                f"({fragment}): {menu_record}")
    require(text.index("[PRODUCTION-INSTALL] boot-menu ") <
            text.index("[PRODUCTION-INSTALL] boot-entry "),
            "the menu decision is published after the boot-entry record")
    # ITEM-409: the staged set is the sealed five plus the menu this run built.
    for fragment in (
            "required_sectors=",
            "esp_first_lba=2048",
            "esp_sectors=67584",
            "sealed_bytes=",
            "sealed_digest=",
            "existing_loader=FREE",
            "existing_config=FREE",
            "verify=SEALED_MEDIA variable=UNAVAILABLE boot_entry=NONE",
    ):
        require(fragment in record,
                f"the boot-entry record is missing {fragment}: {record}")
    require("existing_loader_bytes=" not in record and
            "existing_config_bytes=" not in record,
            "the record claims a preserved file on an empty ESP")
    require("staged_bytes=" in text and
            "mechanism=REMOVABLE_MEDIA_PATH" in text,
            "the esp record does not publish the staged bytes and mechanism")
    require("[PRODUCTION-INSTALL] state initialized=VERIFIED" in text,
            "the persistent state volume was not initialised")
    # ITEM-502: the erase is an explicit step of every install, and the
    # persistence volume it left behind carries headers and zero journals - no
    # record of a previous run can be replayed from it.
    require("[PRODUCTION-INSTALL] state erase stores=" in text and
            "verify=CLEAN" in text,
            "the install did not publish its state erase step")
    _persistence = re.search(
        r"\[INSTALLER-LAYOUT\] role=hfoc-persistence first_lba=(\d+) "
        r"length_lba=(\d+)", text)
    require(_persistence is not None,
            "the layout record does not name the persistence role")
    _first = int(_persistence.group(1))
    _length = int(_persistence.group(2))
    require(_length >= 512,
            "the persistence role is smaller than the default store layout")
    # The state volume sits past the first megabyte the GPT checks read, so it
    # is read from the image directly.
    with open(disk_path, "rb") as _persistence_handle:
        _persistence_handle.seek(_first * 512)
        require(_persistence_handle.read(512)[0:8] == b"HFOCDATA",
                "the persistence volume does not start with a data-region "
                "header")
        for _block in range(1, 64):
            require(_persistence_handle.read(512) == b"\x00" * 512,
                    f"persistence journal block {_block} is not zero after "
                    f"erase")

    # Independent read-back: the GPT header and the partition array on the raw
    # image must agree with the published plan, sector for sector.
    header = disk[512:1024]
    require(header[0:8] == b"EFI PART", "no GPT header on the target disk")
    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    require(entry_count >= 4 and entry_size >= 128,
            "the GPT does not carry four 128-byte entries")
    array = disk[entries_lba * 512:entries_lba * 512 + entry_count * entry_size]
    found = {}
    for index in range(entry_count):
        entry = array[index * entry_size:(index + 1) * entry_size]
        if entry[0:16] == b"\x00" * 16:
            continue
        first = struct.unpack_from("<Q", entry, 32)[0]
        last = struct.unpack_from("<Q", entry, 40)[0]
        found[(first, last + 1 - first)] = index
    required_pairs = {
        (2048, 67584): "efi-system",
        (69632, 131072): "hfoc-system",
        (200704, 65536): "hfoc-persistence",
        (266240, 65536): "hfoc-recovery",
    }
    for pair, role in required_pairs.items():
        require(pair in found,
                f"the written GPT has no {role} partition at {pair}")
    require(len(found) == 4,
            f"the written GPT carries {len(found)} partitions, expected 4")

# ------------------------------------------------------- the BIOS install ---
# ITEM-517: the same proof for the machine without UEFI.  The install plan is
# the BIOS contract's own mode, the boot chain is mechanism=BIOS_MBR, and the
# raw image is read here - LBA0 against the measured Limine recipe, the stage2
# region against the pinned blob, the GPT CRCs and the five role extents -
# before the installed disk is booted on its own below.
if bios_install:
    require("[PRODUCTION-INSTALL] media token=PRESENT confirm="
            "ITEM-394-UNATTENDED-INSTALL target=" + serial +
            " management=local-only mode=default" in text,
            "the BIOS run did not publish the medium declaration")
    plan_lines = [line for line in text.splitlines()
                  if line.startswith("[INSTALLER-LAYOUT] plan ")]
    role_lines = [line for line in text.splitlines()
                  if line.startswith("[INSTALLER-LAYOUT] role=")]
    require(len(plan_lines) == 1,
            f"expected 1 BIOS layout plan record, got {len(plan_lines)}")
    require(len(role_lines) == 5,
            f"expected 5 BIOS layout role records, got {len(role_lines)}")
    for fragment in (
            "mode=clean-whole-disk-bios",
            "media_lba=234375000",
            "minimum_media_lba=333858",
            "minimum_media_mib=163",
            "declared_lba=331776",
            "declared_mib=162",
            "first_lba=2048",
            "footprint_lba=331776",
            "footprint_mib=162",
    ):
        require(fragment in plan_lines[0],
                f"the BIOS layout plan record is missing {fragment}")
    expected_roles = {
        "bios-boot": (2048, 2048),
        "boot-volume": (4096, 67584),
        "hfoc-system": (71680, 131072),
        "hfoc-persistence": (202752, 65536),
        "hfoc-recovery": (268288, 65536),
    }
    for role_line in role_lines:
        match = re.match(
            r"\[INSTALLER-LAYOUT\] role=(\S+) first_lba=(\d+) "
            r"length_lba=(\d+) length_mib=(\d+) reused=(\d+)",
            role_line)
        require(match is not None, f"unparsable BIOS role record: {role_line}")
        role = match.group(1)
        require(role in expected_roles, f"unknown BIOS role: {role}")
        first, length = expected_roles[role]
        require(int(match.group(2)) == first and
                int(match.group(3)) == length,
                f"{role} does not carry the contracted BIOS geometry")
    require("[PRODUCTION-INSTALL] layout mode=whole-disk "
            "reason=no-existing-system declared=0" in text,
            "the BIOS run did not take the whole-disk path")
    require(text.index("[PRODUCTION-INSTALL] layout mode=") <
            text.index("[INSTALLER-LAYOUT] plan "),
            "the BIOS layout decision is published after the first write")
    require("[PRODUCTION-INSTALL] media sealed=VERIFIED" in text,
            "the BIOS run did not verify the media manifest")
    payload_lines = [line for line in text.splitlines()
                     if line.startswith(
                         "[PRODUCTION-INSTALL] bios payload=VERIFIED ")]
    require(len(payload_lines) == 1,
            f"expected exactly one BIOS payload record, got "
            f"{len(payload_lines)}")
    for fragment in (
            "first_lba=4096",
            "length_lba=67584",
            "mode=whole-disk",
            "mechanism=BIOS_MBR",
            "bios_boot_first_lba=2048",
            "bios_boot_sectors=2048",
            "stage2_bytes=22004",
            "payload_set=6",
            "staged_bytes=",
            "payload_bytes=",
    ):
        require(fragment in payload_lines[0],
                f"the BIOS payload record is missing {fragment}")
    boot_entry_lines = [line for line in text.splitlines()
                        if line.startswith("[PRODUCTION-INSTALL] boot-entry ")]
    require(len(boot_entry_lines) == 1,
            f"expected exactly one BIOS boot-entry record, got "
            f"{len(boot_entry_lines)}")
    record = boot_entry_lines[0]
    require(record.startswith(
        "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
        "mechanism=BIOS_MBR "
        "path=\\boot\\limine\\limine.conf "
        "kernel=\\boot\\hfoc-kernel.elf "
        "config=\\boot\\hfoc\\limine.conf "
        "manifest=\\boot\\hfoc-boot.man "
        "loader=\\boot\\limine\\limine-bios.sys "),
        f"the BIOS boot-entry record does not carry the staged set: {record}")
    for fragment in (
            "entries=1",
            "timeout=10s",
            "source=GENERATED",
            "payload_set=6",
            "payload_bytes=",
            "required_sectors=",
            "boot_volume_first_lba=4096",
            "boot_volume_sectors=67584",
            "bios_boot_first_lba=2048",
            "bios_boot_sectors=2048",
            "stage1_lba=0",
            "stage2_first_lba=2048",
            "stage2_bytes=22004",
            "stage2_offset=1048576",
            "stage2_field=420",
            "stage_writes=44",
            "limine_bios_sys_bytes=290964",
            "verify=READBACK gpt=VALID",
    ):
        require(fragment in record,
                f"the BIOS boot-entry record is missing {fragment}: {record}")
    require(re.search(r"blob_digest=[0-9a-f]{64}", record) is not None,
            f"the BIOS boot-entry record publishes no blob digest: {record}")
    boot_menu_lines = [line for line in text.splitlines()
                       if line.startswith("[PRODUCTION-INSTALL] boot-menu ")]
    require(len(boot_menu_lines) == 1,
            f"expected exactly one BIOS boot-menu record, got "
            f"{len(boot_menu_lines)}")
    for fragment in menu_expect:
        require(fragment in boot_menu_lines[0],
                f"the BIOS boot-menu record is missing {fragment}: "
                f"{boot_menu_lines[0]}")
    require(text.index("[PRODUCTION-INSTALL] boot-menu ") <
            text.index("[PRODUCTION-INSTALL] boot-entry "),
            "the BIOS menu decision is published after the boot-entry record")
    menu_lines = [line for line in text.splitlines()
                  if line.startswith("[INSTALLER-BOOT-MENU] menu ")]
    require(len(menu_lines) == 1,
            f"expected exactly one BIOS menu record, got {len(menu_lines)}")
    for fragment in (
            "firmware=bios-legacy",
            "entries=1",
            "loaders=0",
            "missing=0",
            "unreadable_disks=0",
    ):
        require(fragment in menu_lines[0],
                f"the BIOS menu record is missing {fragment}: {menu_lines[0]}")
    require("[INSTALLER-BOOT-MENU] disk id=1 esp=none volume=boot() "
            "scan=NO_ESP loaders=0" in text,
            "the BIOS menu record does not record the target disk NO_ESP")
    # The serial capture carries CRLF (the serial writer translates every LF),
    # so the two markers are located by their text and the block is normalised.
    config_begin = text.index("[INSTALLER-BOOT-MENU] config-begin") + \
        len("[INSTALLER-BOOT-MENU] config-begin")
    config_end = text.index("[INSTALLER-BOOT-MENU] config-end")
    installed_menu = text[config_begin:config_end].replace("\r\n", "\n")
    for fragment in (
            "timeout: 10",
            "/HFOC OS",
            "    path: boot():/boot/hfoc-kernel.elf",
            "    module_path: boot():/boot/hfoc-boot.man",
            "    module_path: boot():/boot/hfoc/limine.conf",
            "    cmdline: hfoc.management=local-only",
            "# disk id=1 reason=NO_ESP",
    ):
        require(fragment in installed_menu,
                f"the installed BIOS menu is missing {fragment!r}")
    require("protocol: efi" not in installed_menu,
            "the installed BIOS menu offers an EFI chainload entry")
    require("[PRODUCTION-INSTALL] esp payload=VERIFIED" not in text,
            "a BIOS install published the UEFI ESP payload record")
    require("[PRODUCTION-INSTALL] state initialized=VERIFIED" in text and
            "[PRODUCTION-INSTALL] state erase stores=" in text and
            "verify=CLEAN" in text,
            "the BIOS install did not initialise and erase the state volume")

    # The raw image, independent of every record above.  The first-megabyte
    # read at the top of this script stops short of the BIOS boot partition, so
    # the stage2 region and the GPT are read here through a fresh handle.
    disk_sectors = pathlib.Path(disk_path).stat().st_size // 512
    blob = limine_stage_blob()
    require(len(blob) == 22516,
            f"the pinned Limine stage image is {len(blob)} bytes, not 22516")
    require(hashlib.sha256(blob).hexdigest() ==
            "83a205ab84c1888a67c1b5c1a8fdc6bace9a61fc4566082e266382266674e54a",
            "deps/limine/limine-bios-hdd.h is not the pinned 12.3.2 stage")
    lba0 = disk[0:512]
    # ITEM-549: the writer repairs the register loss this firmware's AHCI
    # INT 13h handler causes - Limine's Stage1 counts its sectors in ECX - by
    # redirecting both BIOS calls into trampolines inside the stage's own zero
    # padding.  The expected sector is therefore the pinned image with that
    # patch applied, byte for byte.
    stage1 = bytearray(blob[0:512])
    stage1[0xAD:0xAD + 32] = bytes([
        0xC7, 0x04, 0x1E, 0x00, 0x66, 0x51, 0xCD, 0x13, 0x66, 0x59,
        0xE9, 0x48, 0x00,
        0xB4, 0x42, 0xF8, 0x66, 0x51, 0xCD, 0x13, 0x66, 0x59,
        0xE9, 0x79, 0x00,
        0x0F, 0x82, 0xA4, 0x00, 0xE9, 0xC2, 0xFF,
    ])
    stage1[0x8B:0x8F] = bytes([0xE9, 0x38, 0x00, 0x90])
    stage1[0xFC:0x101] = bytes([0xE9, 0xAE, 0xFF, 0x90, 0x90])
    stage1[0x13A:0x13F] = bytes([0xE9, 0x7D, 0xFF, 0x90, 0x90])
    # The recipe patches the little-endian stage2 offset at 0x1a4, so the
    # byte-for-byte comparison excludes exactly those eight bytes; the field
    # itself is required next.
    require(lba0[0:218] == stage1[0:218] and
            lba0[224:0x1A4] == stage1[224:0x1A4] and
            lba0[0x1AC:440] == stage1[0x1AC:440] and
            lba0[510:512] == stage1[510:512] == b"\x55\xaa",
            "LBA0 is not Limine's stage1 with the registers preserved and "
            "the two ranges restored")
    require(struct.unpack_from("<Q", lba0, 0x1A4)[0] == 2048 * 512,
            "the stage2 offset field is not bios_boot_first_lba * 512")
    require(lba0[218:224] == b"\x00" * 6,
            "the stage1 write did not restore bytes 218..223")
    require(lba0[440:446] == b"\x00" * 6,
            "the stage1 write did not restore the MBR's zero signature bytes")
    protective = lba0[446:462]
    require(protective[4] == 0xEE and
            struct.unpack_from("<I", protective, 8)[0] == 1 and
            struct.unpack_from("<I", protective, 12)[0] == disk_sectors - 1,
            "the protective MBR entry is not the one the layout wrote")
    require(all(entry == b"\x00" * 16 for entry in
                (lba0[462:478], lba0[478:494], lba0[494:510])),
            "the MBR carries a partition entry beyond the protective one")
    bios_handle = open(disk_path, "rb")
    try:
        stage2 = read_at(bios_handle, 2048 * 512, 22016)
        require(stage2[0:22004] == blob[512:22516],
                "the BIOS boot partition does not carry Limine's stage2")
        require(stage2[22004:22016] == b"\x00" * 12,
                "the stage2 write did not preserve the partial sector's tail")
        header = read_at(bios_handle, 512, 512)
        require(header[0:8] == b"EFI PART",
                "no GPT header after the BIOS stage write")
        header_size = struct.unpack_from("<I", header, 12)[0]
        require(20 <= header_size <= 512,
                f"the GPT header declares {header_size} bytes")
        stored_crc = struct.unpack_from("<I", header, 16)[0]
        scratch = bytearray(header[0:header_size])
        scratch[16:20] = b"\x00" * 4
        require(gpt_crc32(bytes(scratch)) == stored_crc,
                "the primary GPT header CRC is invalid")
        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        entry_count = struct.unpack_from("<I", header, 80)[0]
        entry_size = struct.unpack_from("<I", header, 84)[0]
        require(entry_count >= 5 and entry_size >= 128,
                "the GPT does not carry five 128-byte entries")
        array = read_at(bios_handle, entries_lba * 512,
                        entry_count * entry_size)
        require(gpt_crc32(array) ==
                struct.unpack_from("<I", header, 88)[0],
                "the primary GPT array CRC is invalid")
        backup_lba = struct.unpack_from("<Q", header, 32)[0]
        require(backup_lba == disk_sectors - 1,
                f"the primary header's backup LBA is {backup_lba}, not the "
                f"last sector")
        backup = read_at(bios_handle, backup_lba * 512, 512)
        require(backup[0:8] == b"EFI PART",
                "no backup GPT header after the BIOS stage write")
        backup_size = struct.unpack_from("<I", backup, 12)[0]
        require(20 <= backup_size <= 512,
                f"the backup GPT header declares {backup_size} bytes")
        backup_crc = struct.unpack_from("<I", backup, 16)[0]
        scratch = bytearray(backup[0:backup_size])
        scratch[16:20] = b"\x00" * 4
        require(gpt_crc32(bytes(scratch)) == backup_crc,
                "the backup GPT header CRC is invalid")
        backup_array = read_at(
            bios_handle, struct.unpack_from("<Q", backup, 72)[0] * 512,
            entry_count * entry_size)
        require(gpt_crc32(backup_array) ==
                struct.unpack_from("<I", backup, 88)[0],
                "the backup GPT array CRC is invalid")
        require(backup_array == array,
                "the primary and backup GPT arrays differ")
    finally:
        bios_handle.close()
    found = {}
    for index in range(entry_count):
        entry = array[index * entry_size:(index + 1) * entry_size]
        if entry[0:16] == b"\x00" * 16:
            continue
        first = struct.unpack_from("<Q", entry, 32)[0]
        last = struct.unpack_from("<Q", entry, 40)[0]
        found[(first, last + 1 - first)] = entry[0:16]
    required_pairs = {
        (2048, 2048): "bios-boot",
        (4096, 67584): "boot-volume",
        (71680, 131072): "hfoc-system",
        (202752, 65536): "hfoc-persistence",
        (268288, 65536): "hfoc-recovery",
    }
    for pair, role in required_pairs.items():
        require(pair in found,
                f"the written GPT has no {role} partition at {pair}")
    require(len(found) == 5,
            f"the written GPT carries {len(found)} partitions, expected 5")
    require(found[(2048, 2048)] == b"Hah!IdontNeedEFI",
            "the BIOS boot partition does not carry the measured type GUID")

print(f"{label}: exact")
PY
    then
        FAILURE_MESSAGE="$label: disk or record verification failed"
        return 1
    fi

    printf '%s\n' "$output"
    return 0
}

# ---------------------------------------------------------------- boot it ---
# ITEM-397: the disk the product flow installed has to boot on its own.  The
# image the previous phase wrote is booted here with no installer medium
# attached, so the firmware has to find the loader the flow staged on the
# removable-media path, and the installed system has to reach the ordinary
# readiness record with persistent state and attest the sealed chain.
#
# ITEM-399: what boots is the *installed* kernel the medium carried, so this
# phase also requires the absence of every installer-build record.  Before this
# item the ESP received the installer kernel, and an installed appliance ran
# the QEMU fixture on every boot.
#
# boot_installed_disk DISK LABEL MEDIUM_ISO_ROOT DECLARED_SCOPE BIND_SCOPE \
#     [CHAINLOAD_PATH [NETWORK_ADDRESS [MENU_SETUP [MENU_ADDRESS \
#      [MENU_ANSWER [PROBE]]]]]]
#
# DECLARED_SCOPE is the media declaration's value (`local-only` /
# `management-interface`, what the installed configuration carries on its
# `cmdline:`), BIND_SCOPE the scope name the kernel publishes for it
# (`LOCAL_ONLY` / `MANAGEMENT_INTERFACE`).  ITEM-446: NETWORK_ADDRESS, when
# given, is the static address the boot must publish in the addressing record
# without running a DHCP discover.  ITEM-447: MENU_SETUP, when `menu`, drives
# the machine's own menu over the monitor to set the address the medium did not
# declare, asserts the record, and leaves through the menu's Reboot entry.
# ITEM-496: MENU_ANSWER is what the gesture answers - `static` (the default,
# MENU_ADDRESS), `dhcp` (discovery), or `bad-address` (an address the IPv4
# owner refuses, which must change nothing).  MENU_SETUP `expect-dhcp` boots a
# disk whose record says DHCP and requires the lease instead.  PROBE `icmp`
# replaces the user-mode NIC with a socket backend the proof itself answers
# ARP on, and requires the appliance to answer an ICMP echo request at
# NETWORK_ADDRESS: a `[PRODUCTION-NET-MODE] mode=STATIC` record is a reading,
# and an address that does not answer is not on the interface.
boot_installed_disk() {
    local disk=$1
    local label=$2
    local medium_iso_root=$3
    local declared_scope=$4
    local bind_scope=$5
    local chainload_path=${6:-}
    local network_address=${7:-}
    local menu_setup=${8:-}
    local menu_address=${9:-}
    local menu_answer=${10:-static}
    local probe=${11:-}
    local net_port=""
    local serial_log=$TEST_DIR/$label-installed-boot.log
    local vars=$TEST_DIR/$label-installed-boot-vars.fd
    local monitor_socket=$TEST_DIR/$label-monitor.sock
    local qemu_pid output
    local network_arguments=()
    local monitor_arguments=()
    # ITEM-464: the installed configuration asks for 1024x768 on its entry.  The
    # attached EDID advertises 1920x1080 as the preferred mode, so a boot that
    # ignored the request would come up at 1920x1080; requiring the actual mode
    # to be 1024x768 is what proves the request reached the installed system.
    local display_arguments=(-vga none -device 'VGA,edid=on,xres=1920,yres=1080')

    [[ -f "$disk" ]] || {
        echo "installed disk is missing: $disk" >&2
        return 1
    }
    # ITEM-446: a declared static address can only be proven on a machine that
    # has an interface, so the network case boots with the QEMU user-mode NIC
    # whose subnet the declaration names.  Every other case keeps the NIC-less
    # machine it had.
    if [[ "$menu_setup" == menu ]]; then
        monitor_arguments=(-monitor "unix:$monitor_socket,server,nowait")
    fi
    if [[ "$probe" == icmp ]]; then
        # ITEM-496: the host side of the wire has to answer ARP and ask for the
        # echo, so the NIC is a raw socket backend instead of the user-mode
        # stack - the same backend the production DHCP/DNS proof uses.
        net_port=$(python3 - <<'PYPORT'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PYPORT
)
        network_arguments=(-netdev "socket,id=net0,listen=127.0.0.1:$net_port"
            -device 'virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on')
    elif [[ -n "$network_address" || "$menu_setup" == menu ||
            "$menu_setup" == expect-dhcp ]]; then
        # The one virtio-net profile the product driver binds: the
        # non-transitional function with the feature set the ITEM-389 driver
        # negotiates (the same device the production serving proof uses).  A
        # transitional device stays NO_DEVICE, which the addressing record
        # would honestly publish but which proves nothing.
        network_arguments=(-netdev "user,id=net0"
            -device 'virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on')
    fi
    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none "${display_arguments[@]}" \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        "${network_arguments[@]}" "${monitor_arguments[@]}" \
        -serial "file:$serial_log" -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$label" \
        "$bind_scope" "$network_address" "$menu_setup" "$monitor_socket" \
        "$menu_address" "$menu_answer" "$probe" "$net_port" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import signal
import socket
import struct
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
qemu_pid = int(sys.argv[2])
label = sys.argv[3]
bind_scope = sys.argv[4]
network_address = sys.argv[5] if len(sys.argv) > 5 else ""
menu_setup = sys.argv[6] if len(sys.argv) > 6 else ""
monitor_socket = sys.argv[7] if len(sys.argv) > 7 else ""
menu_address = sys.argv[8] if len(sys.argv) > 8 else ""
menu_answer = sys.argv[9] if len(sys.argv) > 9 else "static"
probe = sys.argv[10] if len(sys.argv) > 10 else ""
net_port = int(sys.argv[11]) if len(sys.argv) > 11 and sys.argv[11] else 0
# A phase that names a mode this proof does not know is a broken invocation,
# not a boot to assert against: fail before waiting on the guest.
if menu_answer not in ("static", "dhcp", "bad-address"):
    raise SystemExit(f"unknown menu answer {menu_answer!r}")
if probe not in ("", "icmp"):
    raise SystemExit(f"unknown probe mode {probe!r}")
if probe == "icmp" and net_port == 0:
    raise SystemExit("the icmp probe has no socket backend port")
deadline = time.monotonic() + 240.0
MENU_GATEWAY = "10.0.2.2"
MENU_DNS = "10.0.2.3"
# ITEM-456: the menu phase is interactive and the guest is a TCG-emulated
# machine: on a loaded runner one answer's processing (menu, validation,
# persistence write, record) took longer than the 30 s this phase used to
# allow, and the proof failed twice on 2026-09-19 (176 s and 195 s) waiting for
# the commit record.  The window is a patience bound, not an assertion: the
# marker still has to appear, and a marker that never appears now declares the
# retryable timing class above instead of a bare assertion.
MENU_WAIT_SECONDS = 90.0


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def declare_marker_timeout(marker, timeout):
    '''ITEM-456: declare the timing class before this proof exits.

    A proof that stops because it did not observe a marker inside its own
    deadline is not the same event as one whose assertion failed: the guest may
    be healthy and merely starved by a loaded TCG runner.  The suite retries
    exactly the targets that say so, using the one strict record
    `scripts/hfoc-qemu-failure-class.sh` accepts.  This proof used to exit with
    a bare "timed out waiting for ..." line, so two load-induced failures on
    2026-09-19 (176 s and 195 s on the ITEM-447 menu phase) were classified as
    assertions and never retried.
    '''
    sys.stderr.write('HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ds\n'
                     % (marker, int(timeout)))
    sys.stderr.flush()


def wait_for_text(marker, timeout):
    '''Wait for one marker, or fail with the tail of what the guest said.'''
    wait_deadline = time.monotonic() + timeout
    while time.monotonic() < wait_deadline:
        text = serial_text()
        if marker in text:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("kernel panic during the menu answer")
        time.sleep(0.05)
    declare_marker_timeout(marker, timeout)
    raise SystemExit(
        f"timed out waiting for {marker!r}; the guest said:\n"
        + serial_text()[-2000:])


# ---------------------------------------------------------------------------
# ITEM-496: the host side of the wire.
#
# A `[PRODUCTION-NET-MODE] mode=STATIC address=...` record is the kernel's own
# reading of its active configuration; it is not the same fact as an address
# that answers.  The probe below is a minimal Ethernet peer on the socket
# backend: it answers the guest's ARP for the gateway so the guest's stack has
# a next hop, asks who-has the static address, and requires the echo reply to
# come back from it.  An appliance that only wrote the record fails here.
# ---------------------------------------------------------------------------

PROBE_PEER_MAC = bytes.fromhex("5254001234aa")
PROBE_PEER_IP = bytes([10, 0, 2, 2])


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def ip_checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for index in range(0, len(data), 2):
        total += (data[index] << 8) | data[index + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def build_probe_arp_request(sender_mac, sender_ip, target_ip):
    frame = bytearray()
    frame += b"\xff" * 6
    frame += sender_mac
    frame += b"\x08\x06"
    frame += b"\x00\x01\x08\x00\x06\x04\x00\x01"
    frame += sender_mac + sender_ip
    frame += b"\x00" * 6 + target_ip
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def build_probe_arp_reply(destination_mac, requester_ip, sender_ip):
    frame = bytearray()
    frame += destination_mac + PROBE_PEER_MAC
    frame += b"\x08\x06"
    frame += b"\x00\x01\x08\x00\x06\x04\x00\x02"
    frame += PROBE_PEER_MAC + sender_ip
    frame += destination_mac + requester_ip
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def build_probe_icmp_request(destination_mac, guest_ip, identifier, sequence):
    message = bytearray()
    message += b"\x08\x00\x00\x00"
    message += struct.pack("!HH", identifier, sequence)
    message += b"hfoc-item-496"
    message[2:4] = struct.pack("!H", ip_checksum(bytes(message)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(message))
    ip += b"\x49\x60\x00\x00\x40\x01\x00\x00"
    ip += PROBE_PEER_IP + guest_ip
    ip[10:12] = struct.pack("!H", ip_checksum(bytes(ip)))
    frame = bytearray()
    frame += destination_mac + PROBE_PEER_MAC + b"\x08\x00" + ip + message
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def parse_probe_arp(frame):
    '''(opcode, sender_mac, sender_ip, target_ip) or None.'''
    if len(frame) < 42 or frame[12:14] != b"\x08\x06":
        return None
    if frame[14:16] != b"\x00\x01" or frame[16:18] != b"\x08\x00":
        return None
    return (struct.unpack("!H", frame[20:22])[0], frame[22:28], frame[28:32],
            frame[38:42])


def parse_probe_icmp_reply(frame, identifier, sequence):
    if len(frame) < 14 + 20 + 8 or frame[12:14] != b"\x08\x00":
        return False
    if frame[14] != 0x45 or frame[23] != 1:
        return False
    ihl = (frame[14] & 0x0F) * 4
    total = struct.unpack("!H", frame[16:18])[0]
    icmp = 14 + ihl
    if total < ihl + 8 or len(frame) < icmp + 8:
        return False
    if frame[icmp] != 0 or frame[icmp + 1] != 0:
        return False
    if struct.unpack("!HH", frame[icmp + 4:icmp + 8]) != (identifier, sequence):
        return False
    return ip_checksum(frame[icmp:icmp + (total - ihl)]) == 0


def probe_static_address(guest_address, timeout=60.0):
    '''Require an ARP answer and an ICMP echo reply from the static address.'''
    guest_ip = bytes(int(part) for part in guest_address.split("/")[0]
                     .split("."))
    identifier = 0x4960
    sequence = 1
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
        connection.settimeout(5.0)
        connect_deadline = time.monotonic() + 15.0
        while True:
            try:
                connection.connect(("127.0.0.1", net_port))
                break
            except OSError as exc:
                if time.monotonic() >= connect_deadline:
                    raise SystemExit(
                        f"the socket NIC did not accept the probe: {exc}")
                if not qemu_running():
                    raise SystemExit("QEMU exited before the address probe")
                time.sleep(0.05)
        buffer = b""

        def next_frame(wait):
            nonlocal buffer
            frame_deadline = time.monotonic() + wait
            while len(buffer) < 4:
                remaining = frame_deadline - time.monotonic()
                if remaining <= 0:
                    return None
                connection.settimeout(remaining)
                try:
                    chunk = connection.recv(65536)
                except socket.timeout:
                    return None
                if not chunk:
                    raise SystemExit("QEMU network socket closed")
                buffer += chunk
            length = struct.unpack("!I", buffer[:4])[0]
            while len(buffer) < 4 + length:
                remaining = frame_deadline - time.monotonic()
                if remaining <= 0:
                    return None
                connection.settimeout(remaining)
                try:
                    chunk = connection.recv(65536)
                except socket.timeout:
                    return None
                if not chunk:
                    raise SystemExit("QEMU network socket closed")
                buffer += chunk
            frame = buffer[4:4 + length]
            buffer = buffer[4 + length:]
            return frame

        def send(frame):
            connection.sendall(struct.pack("!I", len(frame)) + frame)

        asked = False
        answered_arp = False
        sent_echo = False
        probe_deadline = time.monotonic() + timeout
        while time.monotonic() < probe_deadline:
            if not asked:
                send(build_probe_arp_request(
                    PROBE_PEER_MAC, PROBE_PEER_IP, guest_ip))
                asked = True
            frame = next_frame(0.2)
            if frame is None:
                if not qemu_running():
                    raise SystemExit("QEMU exited during the address probe")
                # Re-ask; a frame may have been lost before the guest's netif
                # was up.
                asked = False
                continue
            arp = parse_probe_arp(frame)
            if arp is not None:
                opcode, sender_mac, sender_ip, target_ip = arp
                if opcode == 1 and target_ip == PROBE_PEER_IP:
                    send(build_probe_arp_reply(
                        sender_mac, sender_ip, target_ip))
                elif opcode == 2 and sender_ip == guest_ip:
                    answered_arp = True
                    if not sent_echo:
                        send(build_probe_icmp_request(
                            sender_mac, guest_ip, identifier, sequence))
                        sent_echo = True
                continue
            if parse_probe_icmp_reply(frame, identifier, sequence):
                print(f"{label}: the static address answers ARP and ICMP: "
                      f"{guest_address}")
                return
        raise SystemExit(
            "the appliance did not answer an ICMP echo request at the static "
            f"address {guest_address} (arp={answered_arp} echo_sent={sent_echo});"
            " the addressing record alone is not an address on the wire")


KEY_NAMES = {
    ".": "dot",
    "/": "slash",
    "-": "minus",
    " ": "spc",
}
for _digit in "0123456789":
    KEY_NAMES[_digit] = _digit


def open_monitor():
    monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    monitor.settimeout(5.0)
    monitor.connect(monitor_socket)
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit("the QEMU monitor closed unexpectedly")
        received += chunk
    return monitor


def monitor_key(monitor, key):
    monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit("the QEMU monitor closed unexpectedly")
        received += chunk
    time.sleep(0.05)


def monitor_line(monitor, text):
    for character in text:
        key = KEY_NAMES.get(character, character)
        monitor_key(monitor, key)
    monitor_key(monitor, "ret")


readiness = re.compile(
    r"\[APPLIANCE-BOOT\] runtime=READY identity=OK config=[A-Z_]+ "
    r"local_management=READY storage=READY "
    r"local_input=(?:AVAILABLE|UNAVAILABLE)")
required = [
    r"\[BOOT\] Bootloader: Limine ",
    r"\[BOOT-CHAIN\] verdict=ATTESTED",
    r"\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK "
    r"discovered=1 valid=1 selected=A[HC]CI storage=READY",
    readiness.pattern,
    r"\[PMM \] status=OK",
    r"\[KBD \] state=",
    # ITEM-410: the installed appliance writes its own boot log into the disk it
    # boots from, and says so.  The log is read back out of the raw image below,
    # after QEMU has stopped, so this wait has to end on a marker the store
    # publishes only once the record is on the medium: `reason=readiness
    # flushed=N` is that completion record.
    #
    # The plain `[BOOTLOG] state=READY` record is not that marker - the boot's
    # first flush already publishes it - and waiting for it ended the wait while
    # the readiness record was still staged, so this proof killed the guest on a
    # race it had made itself.  ITEM-410's own installed-disk proof already
    # waits for the completion record; this proof now waits the same way.
    r"\[BOOTLOG\] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN",
    r"\[BOOTLOG\] state=READY .*reason=readiness .*flushed=[1-9]",
    # ITEM-466 (round 5): the installed display record and the loop's
    # microsecond budget are part of the proof, so the wait has to see both.
    r"\[FB\] width=\d+ height=\d+ pitch=\d+ cells=\d+x\d+ bpp=32 "
    r"modes=\d+ chosen=\d+x\d+ requested=\d+x\d+ actual=\d+x\d+ "
    r"fallback=\d+ scale=\d+ cell_px=\d+x\d+ preferred=\d+x\d+ "
    r"available_count=\d+ available=\S+",
    r"\[PRODUCTION-LOOP\] .*iter_max_us=\d+",
]

while time.monotonic() < deadline:
    text = serial_text()
    if all(re.search(pattern, text) for pattern in required):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic booting the installed disk")
    time.sleep(0.1)
else:
    missing = [pattern for pattern in required
               if not re.search(pattern, serial_text())]
    declare_marker_timeout("; ".join(missing), 240)
    raise SystemExit(
        "the installed disk did not boot; missing: " + "; ".join(missing) +
        "\n" + serial_text()[-3000:])

text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("kernel panic detected")
if len(re.findall(readiness.pattern, text)) != 1:
    raise SystemExit("expected exactly one appliance readiness record")

# ITEM-447: the address the medium did not declare, set from the machine's own
# menu.  The operator's whole gesture is replayed: `menu`, one DOWN to the
# Network entry, ENTER, then the four answers; the record must name what was
# committed, and the machine must leave through the menu's own Reboot entry so
# the next boot reads the record the menu wrote.
#
# ITEM-496: the same gesture is replayed for the three answers the item owns -
# a static address, `dhcp`, and an address the IPv4 owner refuses.  Every
# question and every record is waited for as one whole line, because the
# questions are now IRQ-safe writes; a proof that finds a split `gateway: `
# would be reading the defect this item removed.
if menu_setup == "menu":
    if menu_answer == "static" and not menu_address:
        raise SystemExit("the menu phase needs the address it sets")
    monitor = open_monitor()
    try:
        monitor_line(monitor, "menu")
        wait_for_text("[LOCAL-MENU] open", MENU_WAIT_SECONDS)
        monitor_key(monitor, "down")
        monitor_key(monitor, "ret")
        wait_for_text("[LOCAL-MENU] run action=network", MENU_WAIT_SECONDS)
        # ITEM-496: the disk came from the network medium, so the reading the
        # questions start from is a static address - and it is the live wire's,
        # not the record the menu is about to replace.
        wait_for_text(
            "[PRODUCTION-NET-SET] state=OFFERED current=STATIC address=",
            MENU_WAIT_SECONDS)
        wait_for_text("network mode [dhcp|static] (empty cancels): ",
                      MENU_WAIT_SECONDS)
        if menu_answer == "dhcp":
            monitor_line(monitor, "dhcp")
            expected = ("[PRODUCTION-NET-SET] mode=DHCP persisted=OK "
                        "applies=reboot")
            wait_for_text(expected, MENU_WAIT_SECONDS)
            # The machine was static when the operator answered, so the record
            # must name that configuration as the one still running - not
            # DHCP, which is only stored.
            wait_for_text(expected + " current=STATIC address=",
                          MENU_WAIT_SECONDS)
            print(f"{label}: menu answer committed: {expected} current=<static>")
        elif menu_answer == "bad-address":
            # The operator typed an address the owner refuses: the whole
            # answer is refused with the owner's reason and nothing changes.
            monitor_line(monitor, "static")
            wait_for_text("address a.b.c.d/prefix: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, "10.0.2.16/33")
            wait_for_text("gateway: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, MENU_GATEWAY)
            wait_for_text("dns: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, MENU_DNS)
            wait_for_text(
                "[PRODUCTION-NET-SET] state=REFUSED "
                "reason=NETWORK_IPV4_INVALID_PREFIX\n", MENU_WAIT_SECONDS)
            print(f"{label}: the bad address was refused with its reason")
        else:
            monitor_line(monitor, "static")
            wait_for_text("address a.b.c.d/prefix: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, menu_address)
            wait_for_text("gateway: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, MENU_GATEWAY)
            wait_for_text("dns: ", MENU_WAIT_SECONDS)
            monitor_line(monitor, MENU_DNS)
            expected = ("[PRODUCTION-NET-SET] mode=STATIC address="
                        + menu_address + " gateway=" + MENU_GATEWAY
                        + " dns=" + MENU_DNS + " persisted=OK applies=reboot")
            wait_for_text(expected, MENU_WAIT_SECONDS)
            # The record names the configuration still on the interface, and
            # that is the old one: the address just committed reaches the wire
            # at the next boot, not at this answer.
            wait_for_text(expected + " current=", MENU_WAIT_SECONDS)
            if re.search(r"applies=reboot current=STATIC address="
                         + re.escape(menu_address), serial_text()):
                raise SystemExit(
                    "the answer record claims the new address is already "
                    "running")
            print(f"{label}: menu answer committed: {expected} current=<old>")
        # Leave through the menu, so the record is written and the machine
        # resets the way the operator's own gesture does.
        monitor_line(monitor, "menu")
        wait_for_text("[LOCAL-MENU] open", MENU_WAIT_SECONDS)
        for _ in range(3):
            monitor_key(monitor, "down")
        monitor_key(monitor, "ret")
        wait_for_text("[LOCAL-MENU] reboot now", MENU_WAIT_SECONDS)
        print(f"{label}: reset through the menu after the "
              f"{menu_answer} answer")
    finally:
        monitor.close()
    try:
        os.kill(qemu_pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    raise SystemExit(0)
# ITEM-496: the boot after the menu answered `dhcp`.  The record says DHCP and
# the wire grants the lease; a static address left over from the previous
# configuration would fail here.
if menu_setup == "expect-dhcp":
    if not re.search(r"\[PRODUCTION-NET-MODE\] mode=DHCP clock=", text):
        raise SystemExit(
            "the disk set to DHCP did not publish a DHCP addressing record; "
            "the guest said:\n" + text[-2000:])
    if re.search(r"\[PRODUCTION-NET-MODE\] mode=STATIC", text):
        raise SystemExit(
            "the disk set to DHCP still holds a static address")
    wait_for_text("[PRODUCTION-DHCP] state=ACTIVE lease=ACQUIRED address="
                  "10.0.2.15", 120.0)
    print(f"{label}: DHCP is in force again and the lease is "
          f"10.0.2.15")
    try:
        os.kill(qemu_pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    raise SystemExit(0)
for pattern in required:
    if not re.search(pattern, text):
        raise SystemExit(f"missing record matching {pattern!r}")
# ITEM-466 (round 5): the installed entry asks for no mode of its own, so the
# guest boots the firmware's preferred mode - the 1920x1080 the attached EDID
# advertises - with the automatic font scale 2, and `[FB]` names the preference,
# the available list and the cell/grid that scale produced.
geometry = re.search(
    r"\[FB\] width=(\d+) height=(\d+) pitch=(\d+) cells=(\d+)x(\d+) bpp=(\d+) "
    r"modes=(\d+) chosen=(\d+)x(\d+) requested=(\d+)x(\d+) actual=(\d+)x(\d+) "
    r"fallback=(\d+) scale=(\d+) cell_px=(\d+)x(\d+) preferred=(\d+)x(\d+) "
    r"available_count=(\d+) available=(\S+)", text)
if geometry is None:
    raise SystemExit("the installed boot published no [FB] geometry record")
actual = (int(geometry.group(12)), int(geometry.group(13)))
requested = (int(geometry.group(10)), int(geometry.group(11)))
chosen = (int(geometry.group(8)), int(geometry.group(9)))
mode_count = int(geometry.group(7))
fallback = int(geometry.group(14))
scale = int(geometry.group(15))
cell_px = (int(geometry.group(16)), int(geometry.group(17)))
preferred = (int(geometry.group(18)), int(geometry.group(19)))
available_count = int(geometry.group(20))
if requested != preferred:
    raise SystemExit(
        f"requested={requested} is not the preferred mode {preferred}")
if preferred != (1920, 1080):
    raise SystemExit(
        f"the installed firmware preferred {preferred}, not the EDID's "
        f"1920x1080")
if actual != (1920, 1080):
    raise SystemExit(
        f"the installed system booted {actual}, not the native 1920x1080")
if fallback != 0:
    raise SystemExit(f"the native mode was not honoured: fallback={fallback}")
if mode_count == 0 or available_count == 0:
    raise SystemExit("the installed firmware mode list was empty")
if chosen != actual:
    raise SystemExit(
        f"the selection rule chose {chosen}, not the actual {actual}")
if scale != 2:
    raise SystemExit(f"the automatic font scale is {scale}, not 2 at 1920x1080")
if cell_px != (8 * scale, 16 * scale):
    raise SystemExit(f"the cell {cell_px} does not follow scale={scale}")
loops = re.findall(
    r"\[PRODUCTION-LOOP\][^\r\n]*iter_us=(\d+) iter_max_us=(\d+)", text)
if not loops:
    raise SystemExit("the installed boot published no loop cost record")
iter_worst = max(int(entry[1]) for entry in loops)
ITER_US_BUDGET = 500000
if iter_worst > ITER_US_BUDGET:
    raise SystemExit(
        f"the installed main loop took {iter_worst} us, over the "
        f"{ITER_US_BUDGET} us budget")
# ITEM-446: an installed appliance that declared a static address comes up on
# it and does not run a DHCP discover.  The address in the record is the active
# configuration's own, so this reads what the interface has, not what the
# declaration asked for.
if network_address:
    expected_network = (r"\[PRODUCTION-NET-MODE\] mode=STATIC address="
                        + re.escape(network_address))
    if not re.search(expected_network, text):
        raise SystemExit(
            f"the installed boot did not publish {expected_network!r}")
    if re.search(r"\[PRODUCTION-DHCP\] .*discover=[1-9]", text):
        raise SystemExit(
            "a static declaration still ran a DHCP discover")
    # ITEM-496: the record is the kernel's reading, and the reading is only
    # true if the address answers.  The probe is the host's own ARP+ICMP, so a
    # machine that printed the record without putting the address on the wire
    # fails here.
    if probe == "icmp":
        probe_static_address(network_address)
# The installed boot is an ordinary appliance boot: the install flow must not
# engage again.
if "[PRODUCTION-INSTALL] " in text:
    raise SystemExit("the install flow ran in the installed boot")
# ITEM-399: and it is the *product* appliance that booted.  Before this item
# the ESP received the installer kernel, so every installed appliance ran the
# QEMU fixture and published `[PRODUCTION-INSTALLER] proof OK` on each boot.
# Every record of that build is a defect here.
installer_records = [line for line in text.splitlines()
                     if "[PRODUCTION-INSTALLER]" in line]
if installer_records:
    raise SystemExit(
        "the installed disk booted an installer build; it published: " +
        "; ".join(installer_records[:4]))
if "[PRODUCTION-INSTALLER] proof init" in text or \
        "[PRODUCTION-INSTALLER] proof OK" in text:
    raise SystemExit("the installer-build proof marker is in the installed boot")
# ITEM-399: the scope the medium declared, stated by the installed system as a
# boot declaration - never as a default.
bind_records = [line for line in text.splitlines()
                if line.startswith("[PRODUCTION-MGMT-BIND] ")]
if len(bind_records) != 1:
    raise SystemExit(
        f"expected exactly one management bind record, got {len(bind_records)}")
expected = (f"[PRODUCTION-MGMT-BIND] scope={bind_scope} "
            "source=boot-declaration")
if not bind_records[0].startswith(expected):
    raise SystemExit(
        f"the installed boot does not publish {expected!r}: {bind_records[0]}")
print(f"{label}: installed-disk: exact")
print(f"{label}: installed-disk readiness with persistent state: READY")
print(f"{label}: installed-disk management: {bind_records[0]}")

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before the installed boot completed")
PY
    ); then
        FAILURE_MESSAGE="$label: $output"
        printf '%s\n' "$output" >&2
        # ITEM-456: a proof that failed must not leave its guest running.  This
        # path used to return with the QEMU process still alive, so the suite's
        # own "Check for leaked QEMU processes" step failed too and the next
        # target raced a guest that was still writing to the shared tree
        # (observed on 2026-09-19 after the menu-phase marker timeout).
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"

    # ITEM-399: the configuration the installed ESP carries is the one the
    # medium sealed - read out of the finished image here, by this script,
    # independently of the kernel.
    assert_installed_configuration "$disk" \
        "$medium_iso_root/boot/hfoc-installed.conf" "$declared_scope" \
        "$chainload_path" || return 1
    # ITEM-410: and the appliance wrote its own boot log into that disk.
    assert_boot_log "$disk" "$label" || return 1
    # ITEM-497: the installed boot also left its own input verdict there.
    assert_input_verdict "$disk" "$label" || return 1
    return 0
}

# assert_installed_diag_export DISK LABEL MODE
#
# ITEM-582: the installed system exports its own diagnosis to a separately
# prepared HFCD stick.  MODE is `before` (the stick was attached at QEMU start)
# or `after` (it is plugged in through the monitor once the installed system is
# at its console).  A foreign mass-storage device is attached in front of the
# stick on the same hub, so the walk must pass it and the foreign disk must be
# byte-identical afterwards.  The report is extracted from the stick image by
# the operator's Debian script, which verifies the journal and its CRC.
#
# The finished system boots from its own SATA (IDE) disk: the install proof
# above ran the installer, and this function only boots what it installed.
assert_installed_diag_export() {
    local disk=$1
    local label=$2
    local mode=$3
    local serial_log=$TEST_DIR/$label-diag-boot.log
    local monitor_socket=$TEST_DIR/$label-diag-monitor.sock
    local vars=$TEST_DIR/$label-diag-vars.fd
    local stick=$TEST_DIR/hfcd-diag.raw
    local foreign=$TEST_DIR/hfcd-foreign.raw
    local report_dir=$TEST_DIR/$label-diag-report
    local qemu_pid output report before after
    local usb_args

    [[ -f "$disk" ]] || { echo "$label: installed disk is missing" >&2; return 1; }
    ARTIFACT_DIR="build/test-artifacts/$label-diag-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    mkdir -p "$ARTIFACT_DIR"
    save_diag_artifacts() {
        cp -a "$serial_log" "$ARTIFACT_DIR/" 2>/dev/null || true
        cp -a "$TEST_DIR/$label-diag-extract.txt" "$ARTIFACT_DIR/" 2>/dev/null || true
        cp -a "$stick" "$ARTIFACT_DIR/diag-stick.raw" 2>/dev/null || true
        cp -a "$foreign" "$ARTIFACT_DIR/foreign.raw" 2>/dev/null || true
        echo "$label: artifacts kept in $ARTIFACT_DIR" >&2
    }
    if [[ ! -f "$stick" ]]; then
        python3 scripts/hfoc-diag-stick-prepare.py --output "$stick" --force \
            >/dev/null || {
            echo "$label: cannot prepare the HFCD stick" >&2
            return 1
        }
    fi
    if [[ ! -f "$foreign" ]]; then
        # The foreign device is the same prepared image with another disk id:
        # a FAT volume that is not the diagnostic medium, attached in front.
        cp "$stick" "$foreign"
        printf '\x78\x56\x34\x12' | dd of="$foreign" bs=1 seek=440 \
            conv=notrunc status=none
    fi
    before=$(sha256sum "$foreign" | awk '{print $1}')

    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    # ITEM-582: the production xHCI walk probes root ports; the HS-RMH topology
    # is proven separately by the EHCI walk host model (a QEMU usb-hub is a
    # USB 1.1 device, and HFOC does not walk an xHCI hub's children).  Here the
    # foreign disk and the HFCD stick sit on two root ports, foreign first.
    if [[ "$mode" == before ]]; then
        usb_args=(
            -device "qemu-xhci,id=usb0"
            -drive "if=none,id=foreign,file=$foreign,format=raw,file.locking=off"
            -drive "if=none,id=diag,file=$stick,format=raw,file.locking=off"
            -device "usb-storage,bus=usb0.0,port=1,drive=foreign"
            -device "usb-storage,bus=usb0.0,port=2,drive=diag"
        )
    elif [[ "$mode" == full ]]; then
        # ITEM-590: the owner's Toshiba binds the diagnostic stick on EHCI
        # during the boot walk and the machine's own telemetry then fills the
        # reserved journal (512 slots in about twenty-three seconds).  The
        # proof has to reproduce that condition, or the export it exercises is
        # never reached with a FULL journal: the xHCI boot walk above refuses
        # the foreign device and leaves the operator's save to bind the stick
        # lazily, which is a different path.
        usb_args=(
            -device "ich9-usb-ehci1,id=ehci0"
            -drive "if=none,id=foreign,file=$foreign,format=raw,file.locking=off"
            -drive "if=none,id=diag,file=$stick,format=raw,file.locking=off"
            -device "usb-storage,bus=ehci0.0,port=1,drive=foreign"
            -device "usb-storage,bus=ehci0.0,port=2,drive=diag"
        )
    else
        # The after-boot variant plugs the two media into an EHCI controller
        # (USB 2.0): the production late-discovery path for EHCI re-measures
        # every root port and walks it again, which is exactly the defect this
        # item fixes.  QEMU's hot-plugged SuperSpeed storage answers the first
        # BOT command with COMMAND_FAILED, so the SS controller is exercised by
        # the pre-boot variant instead.
        usb_args=(
            -device "ich9-usb-ehci1,id=ehci0"
            -drive "if=none,id=foreign,file=$foreign,format=raw,file.locking=off"
            -drive "if=none,id=diag,file=$stick,format=raw,file.locking=off"
        )
    fi

    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none -vga none -device 'VGA,edid=on,xres=1920,yres=1080' \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        -monitor "unix:$monitor_socket,server,nowait" \
        "${usb_args[@]}" \
        -serial "file:$serial_log" -no-reboot \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$monitor_socket" \
        "$mode" "$label" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import importlib.util
import os
import pathlib
import socket
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
qemu_pid = int(sys.argv[2])
monitor_path = pathlib.Path(sys.argv[3])
mode = sys.argv[4]
label = sys.argv[5]


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def fail(message):
    print(f"{label}: {message}", file=sys.stderr)
    raise SystemExit(1)


def wait_text(marker, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            fail("kernel panic before " + what)
        if not qemu_running():
            fail("QEMU exited before " + what)
        time.sleep(0.1)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' + marker + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    fail("timed out waiting for " + what)


text = wait_text("[APPLIANCE-BOOT] runtime=READY", 240.0,
                 "the installed system readiness")
wait_text("hfoc> ", 120.0, "the installed system console")

monitor_deadline = time.monotonic() + 10.0
while not monitor_path.exists():
    if time.monotonic() >= monitor_deadline:
        fail("timed out waiting for the QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(5.0)
    try:
        monitor.connect(str(monitor_path))
    except OSError as exc:
        fail(f"QEMU monitor did not accept connection: {exc}")
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            fail("the QEMU monitor closed unexpectedly")
        received += chunk

    def command(line, expect_reply=True):
        monitor.sendall((line + "\n").encode("ascii"))
        if not expect_reply:
            return
        reply = b""
        while b"(qemu)" not in reply:
            chunk = monitor.recv(4096)
            if not chunk:
                fail("the QEMU monitor closed unexpectedly")
            reply += chunk
        if b"Error" in reply or b"error" in reply:
            fail("the QEMU monitor refused " + line + ": " +
                 reply.decode("ascii", errors="replace"))
        time.sleep(0.05)

    if mode == "after":
        # Hot-plug onto the EHCI root ports, foreign first: the machine must
        # find both without resetting anything it already bound.
        command("device_add usb-storage,bus=ehci0.0,port=1,drive=foreign")
        command("device_add usb-storage,bus=ehci0.0,port=2,drive=diag")

    def key(name):
        command("sendkey " + name)

    def type_command(text):
        for character in text:
            key("spc" if character == " " else character)
        key("ret")

    def wait_saves(expected, timeout, what):
        """Collect distinct `DIAG SAVED id=` sessions until `expected` of them."""
        sessions = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = serial_text()
            for line in text.splitlines():
                if line.startswith("DIAG SAVED id="):
                    session = line.split("id=", 1)[1].split()[0]
                    if session not in sessions:
                        sessions.append(session)
                if line.startswith("DIAG FAILED"):
                    fail("the installed system refused `diag save`: " + line)
            if len(sessions) >= expected:
                return sessions
            if "*** HFOC KERNEL PANIC ***" in text:
                fail("kernel panic during `diag save`")
            time.sleep(0.1)
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="DIAG SAVED" deadline='
            + str(int(timeout)) + 's\n')
        sys.stderr.flush()
        fail("the installed system printed no DIAG SAVED (" + what + ")")
        return sessions

    if mode == "full":
        # ITEM-590: the machine's own telemetry fills the reserved journal
        # before the operator's command, exactly as it did on the owner's
        # Toshiba (512 slots in about twenty-three seconds).  The export must
        # recover the room through the journal's own rewind instead of reading
        # a healthy FULL journal as a missing medium, which is what the laptop
        # did: `DIAG FAILED reason=NO_MEDIUM step=MEDIUM_PROBE` and not one
        # `[DIAG]` record on the stick.
        wait_text("[INSTALL-LOG] state=FULL reason=CAPACITY", 240.0,
                  "the journal to fill")

    type_command("diag save")
    sessions = wait_saves(1, 180.0, "the first save")
    if mode == "full":
        # ITEM-590: a second save on the recovered journal is its own session,
        # with its own identifier - the rewind is not a one-way door.
        type_command("diag save")
        sessions = wait_saves(2, 180.0, "the second save")
    saved = "DIAG SAVED id=" + sessions[-1]
    if mode == "full":
        if len(sessions) != 2 or len(set(sessions)) != 2:
            fail("two saves after the fill did not produce two sessions: "
                 + repr(sessions))
        print(f"{label}: the journal was FULL and two saves produced "
              f"sessions {sessions[0]} and {sessions[1]}")
    # `quit` closes the monitor before it answers, so it is sent without
    # waiting for a prompt.
    command("quit", expect_reply=False)
print(f"{label}: installed system saved the diagnosis: {saved}")
PY
    ); then
        printf '%s\n' "$output" >&2
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
        save_diag_artifacts
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"

    rm -rf "$report_dir"
    mkdir -p "$report_dir"
    python3 scripts/hfoc-diag-read.py --disk "$stick" --output "$report_dir" \
        >"$TEST_DIR/$label-diag-extract.txt" 2>&1 || {
        echo "$label: extraction failed" >&2
        cat "$TEST_DIR/$label-diag-extract.txt" >&2
        return 1
    }
    report=$(find "$report_dir" -name 'HFOC-DIAG-*.txt' -print -quit)
    [[ -n "$report" && -f "$report" ]] || {
        echo "$label: the extractor wrote no report" >&2
        cat "$TEST_DIR/$label-diag-extract.txt" >&2
        return 1
    }
    grep -q 'verified' "$TEST_DIR/$label-diag-extract.txt" || {
        echo "$label: the extractor did not verify the journal" >&2
        cat "$TEST_DIR/$label-diag-extract.txt" >&2
        return 1
    }
    for record in 'build=' '[NET-STATUS]' '[NET-ADDR]' '[LOGIN-TRACE]' '[PANEL]' '[RNG]' '[ADMIN]' 'state=END records='; do
        grep -Fq -- "$record" "$report" || {
            echo "$label: the extracted report lacks $record" >&2
            return 1
        }
    done
    # ITEM-614: this boot reaches `diag save` without a local login, so the
    # exported trace must say so instead of inventing an `ok=1` or zeros.
    if grep -Fq 'available=1' "$report" ||
        ! grep -Fq 'available=0 state=NOT_ATTEMPTED' "$report"; then
        echo "$label: the extracted cold-boot trace claims a login result" >&2
        return 1
    fi
    after=$(sha256sum "$foreign" | awk '{print $1}')
    [[ "$before" == "$after" ]] || {
        echo "$label: the foreign disk changed ($before -> $after)" >&2
        return 1
    }
    grep -Fq 'DIAG SAVED id=' <(printf '%s\n' "$output") || {
        echo "$label: no DIAG SAVED line" >&2
        return 1
    }
    echo "$label: diagnostic export from the installed system: OK ($report)"
    return 0
}

# boot_installed_bios_disk DISK LABEL [BIND_SCOPE]
#
# ITEM-517: the disk the BIOS install wrote has to boot on its own, with no
# installer medium and no UEFI firmware: SeaBIOS executes Limine's MBR stage1,
# stage2 reads `limine-bios.sys` and the generated menu from the FAT boot
# volume, and the installed appliance has to reach the ordinary readiness
# record with persistent state and attest the sealed chain.  The QEMU
# invocation is the UEFI helper's without the two pflash drives, and the wait
# is its own because a legacy-BIOS machine has no ESP: the boot log store has
# no target to attach to (the install flow attached it at install time), so the
# `[BOOTLOG]` records the UEFI helper waits for do not exist here.  Everything
# the installed system must prove is still required:
#
#   * the Limine boot record, the sealed chain attestation, the production
#     boot verdict with one selected A[HC]CI backend, the readiness record and
#     the loop budget;
#   * the absence of every install-flow and installer-build record - this boot
#     is the *product* appliance the medium carried, not the installer;
#   * the management scope the medium declared, read from the boot entry's own
#     command line.
#
# A marker that does not appear inside the deadline declares the retryable
# timing class (`declare_marker_timeout()`, ITEM-456), so a loaded TCG runner
# is a retry and not a false assertion.
boot_installed_bios_disk() {
    local disk=$1
    local label=$2
    local bind_scope=${3:-LOCAL_ONLY}
    local serial_log=$TEST_DIR/$label-installed-bios-boot.log
    local qemu_pid output
    # ITEM-466: the display arguments the UEFI installed-disk helper uses, so
    # the two boots differ in the firmware alone.  The geometry itself is not
    # asserted here: the BIOS boot entry asks for no mode of its own.
    local display_arguments=(-vga none -device 'VGA,edid=on,xres=1920,yres=1080')

    [[ -f "$disk" ]] || {
        echo "installed BIOS disk is missing: $disk" >&2
        return 1
    }
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none "${display_arguments[@]}" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        -serial "file:$serial_log" -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$label" \
        "$bind_scope" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import signal
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
qemu_pid = int(sys.argv[2])
label = sys.argv[3]
bind_scope = sys.argv[4]
deadline = time.monotonic() + 240.0


def declare_marker_timeout(marker, timeout):
    '''ITEM-456: declare the timing class the suite retries.

    The same strict record the install-flow waits write, so a legacy-BIOS boot
    that did not finish inside its deadline is retried as timing rather than
    reported as a failed assertion.
    '''
    sys.stderr.write('HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ds\n'
                     % (marker, int(timeout)))
    sys.stderr.flush()


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def exited():
    try:
        stat = pathlib.Path(f"/proc/{qemu_pid}/stat").read_text()
    except OSError:
        return True
    return stat.rpartition(")")[2].split()[:1] == ["Z"]


def running():
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        return False
    return not exited()


readiness = re.compile(
    r"\[APPLIANCE-BOOT\] runtime=READY identity=OK config=[A-Z_]+ "
    r"local_management=READY storage=READY "
    r"local_input=(?:AVAILABLE|UNAVAILABLE)")
required = [
    r"\[BOOT\] Bootloader: Limine ",
    r"\[BOOT-CHAIN\] verdict=ATTESTED",
    r"\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK "
    r"discovered=1 valid=1 selected=A[HC]CI storage=READY",
    readiness.pattern,
    r"\[PMM \] status=OK",
    r"\[KBD \] state=",
    r"\[FB\] width=\d+ height=\d+ pitch=\d+",
    r"\[PRODUCTION-LOOP\] .*iter_max_us=\d+",
]

while time.monotonic() < deadline:
    text = serial_text()
    if all(re.search(pattern, text) for pattern in required):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic booting the installed BIOS disk")
    if not running():
        raise SystemExit(
            "QEMU exited before the installed BIOS disk booted")
    time.sleep(0.1)
else:
    missing = [pattern for pattern in required
               if not re.search(pattern, serial_text())]
    declare_marker_timeout("; ".join(missing), 240)
    raise SystemExit(
        "the installed BIOS disk did not boot; missing: " +
        "; ".join(missing) + "\n" + serial_text()[-3000:])

text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("kernel panic detected")
if len(re.findall(readiness.pattern, text)) != 1:
    raise SystemExit("expected exactly one appliance readiness record")
# The installed boot is an ordinary appliance boot: the install flow must not
# engage again, and what boots is the product kernel, not an installer build.
if "[PRODUCTION-INSTALL] " in text:
    raise SystemExit("the install flow ran in the installed BIOS boot")
installer_records = [line for line in text.splitlines()
                     if "[PRODUCTION-INSTALLER]" in line]
if installer_records:
    raise SystemExit(
        "the installed BIOS disk booted an installer build; it published: " +
        "; ".join(installer_records[:4]))
# The scope the medium declared, stated by the installed system as a boot
# declaration - never as a default.
bind_records = [line for line in text.splitlines()
                if line.startswith("[PRODUCTION-MGMT-BIND] ")]
if len(bind_records) != 1:
    raise SystemExit(
        f"expected exactly one management bind record, got "
        f"{len(bind_records)}")
expected = (f"[PRODUCTION-MGMT-BIND] scope={bind_scope} "
            "source=boot-declaration")
if not bind_records[0].startswith(expected):
    raise SystemExit(
        f"the installed BIOS boot does not publish {expected!r}: "
        f"{bind_records[0]}")
# A legacy-BIOS machine has no ESP for the boot log store to attach to, and
# this proof does not invent one: the boot log on this disk is the install-time
# log the flow attached to the FAT boot volume.
print(f"{label}: installed BIOS disk: exact")
print(f"{label}: installed BIOS disk readiness: READY")
print(f"{label}: installed BIOS disk chain: ATTESTED")
print(f"{label}: installed BIOS disk management: {bind_records[0]}")

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before the installed BIOS boot completed")
PY
    ); then
        FAILURE_MESSAGE="$label: $output"
        printf '%s\n' "$output" >&2
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"
    return 0
}

# assert_installed_configuration DISK MEDIUM_CONFIG DECLARED_SCOPE [CHAINLOAD_PATH]
#
# The ESP is a FAT32 volume written by the kernel's ESP writer, and what it
# carries is read back from the raw image by `scripts/hfoc-esp-read.py` - the
# GPT is parsed there, the volume's BPB is parsed there, and the file is
# reassembled from its cluster chain.  Nothing of this comes from the kernel,
# and both comparisons with the medium's own files are byte-for-byte `cmp`:
# the installed kernel the ESP carries is the medium's *product* kernel (ITEM-399;
# before this item it was the installer build), and the sealed configuration the
# boot chain attests is the medium's sealed one with its declared management
# scope.
#
# ITEM-409 adds the configuration the *loader* reads, `\EFI\BOOT\limine.conf`:
# the generated menu is the installed configuration for every install now, so
# this reads it out of the finished image and requires the appliance entry (with
# its declared scope and the sealed chain's module paths) as the first entry -
# Limine's default - and the documented timeout.  CHAINLOAD_PATH, when given, is
# the chainload entry the machine's other system must be reachable through.
assert_installed_configuration() {
    local disk=$1
    local medium_config=$2
    local declared_scope=$3
    local chainload=${4:-}
    local staged
    local staged_kernel
    local menu

    staged=$TEST_DIR/$(basename "$disk").installed-conf
    staged_kernel=$TEST_DIR/$(basename "$disk").installed-kernel
    menu=$TEST_DIR/$(basename "$disk").installed-menu

    python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'boot\hfoc-kernel.elf' >"$staged_kernel" || {
        echo "cannot read the installed kernel from $disk" >&2
        return 1
    }
    cmp -s "$staged_kernel" "$(dirname -- "$medium_config")/hfoc-kernel.elf" || {
        echo "the ESP kernel is not the medium's installed (product) kernel" >&2
        return 1
    }
    if bash scripts/hfoc-file-marker.sh "$staged_kernel" \
            '[PRODUCTION-INSTALLER] proof init'; then
        echo "the ESP kernel is an installer build" >&2
        return 1
    fi
    printf '%s\n' \
        "installed kernel: $(wc -c <"$staged_kernel") bytes, sha256" \
        "  $(sha256sum "$staged_kernel" | awk '{print $1}'), identical to the" \
        "  medium's product kernel and carrying no installer proof marker"

    python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'boot\limine\limine.conf' >"$staged" || {
        echo "cannot read the installed configuration from $disk" >&2
        return 1
    }
    cmp -s "$staged" "$medium_config" || {
        echo "the staged configuration is not the bytes the medium sealed:" >&2
        echo "  staged: $staged" >&2
        echo "  medium: $medium_config" >&2
        return 1
    }
    grep -Fq 'path: boot():/boot/hfoc-kernel.elf' "$staged" || {
        echo "the staged configuration does not name the staged kernel" >&2
        return 1
    }
    # ITEM-466 (round 5): the installed entry asks for no mode of its own; the
    # firmware's preferred mode boots and the kernel publishes it.  A config
    # that still carried a fixed `resolution:` would be the hardcode the owner
    # removed, so the staged entry must not declare one.
    if grep -Eq '^[[:space:]]*resolution:' "$staged"; then
        echo "the staged configuration still hardcodes a console mode" >&2
        return 1
    fi
    local cmdline="cmdline: hfoc.management=$declared_scope"
    [[ "$(grep -c 'cmdline:' "$staged")" == 1 ]] || {
        echo "the staged configuration does not declare exactly one command line" >&2
        return 1
    }
    grep -Fq "$cmdline" "$staged" || {
        echo "the staged configuration does not carry: $cmdline" >&2
        return 1
    }
    printf '%s\n' \
        "installed configuration: $(wc -c <"$staged") bytes, identical to the" \
        "  medium's sealed configuration, $cmdline"

    # ITEM-409: and the configuration the loader actually reads.
    python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'EFI\BOOT\limine.conf' >"$menu" || {
        echo "cannot read the installed menu from $disk" >&2
        return 1
    }
    python3 - "$menu" "$declared_scope" "$chainload" <<'PY'
import pathlib
import sys

menu_path, declared_scope, chainload = sys.argv[1], sys.argv[2], sys.argv[3]
text = pathlib.Path(menu_path).read_text()
lines = text.splitlines()


def require(condition, message):
    if not condition:
        raise SystemExit("installed menu: " + message)


require("timeout: 10" in lines,
        "the installed menu does not declare the documented ten-second timeout")
entries = [line for line in lines if line.startswith("/")]
require(entries and entries[0] == "/HFOC OS",
        f"the appliance is not the first entry (Limine's default): {entries}")
require(text.count("cmdline:") == 1,
        "the menu declares more than one command line")
for fragment in (
        "    protocol: limine",
        "    path: boot():/boot/hfoc-kernel.elf",
        f"    cmdline: hfoc.management={declared_scope}",
        "    module_path: boot():/boot/hfoc-boot.man",
        "    module_path: boot():/boot/limine/limine.conf",
):
    require(fragment in text,
            f"the appliance entry does not carry {fragment!r}")
require("resolution:" not in text,
        "the installed menu still hardcodes a console mode")
if chainload:
    require(f"    path: {chainload}" in text,
            f"the installed menu does not offer {chainload!r}")
    appliance_index = lines.index("/HFOC OS")
    chainload_index = [index for index, line in enumerate(lines)
                       if chainload in line]
    require(chainload_index and appliance_index < chainload_index[0],
            "the chainload entry precedes the appliance entry")
print("installed menu: " + "; ".join(entries) +
      f" (timeout 10s, appliance first, scope={declared_scope})")
if chainload:
    print("installed menu: chainload entry " + chainload)
PY
    return 0
}

# assert_bios_boot_volume DISK LABEL MEDIUM_ISO_ROOT
#
# ITEM-517: the legacy-BIOS install has no ESP; its boot set lives on the FAT
# boot volume, which is the second partition of the BIOS layout (slot 1, after
# the `Hah!IdontNeedEFI` BIOS boot partition in slot 0).  `hfoc-esp-read.py`
# selects that partition by its GPT slot index and reads the file out of the
# raw image, so every comparison below is byte-for-byte against the medium's
# own bytes:
#
#   * `\boot\limine\limine.conf` is the generated menu Limine reads - the
#     appliance entry, the declared management scope, the chain-configuration
#     module and the honest `NO_ESP` comments, with no EFI chainload entry;
#   * `\boot\hfoc\limine.conf` is the sealed configuration the menu names as
#     the chain module, byte-identical to the medium's sealed one;
#   * `\boot\hfoc-kernel.elf` is the medium's *product* kernel (ITEM-399), not
#     the installer build;
#   * `\boot\hfoc-boot.man` is the sealed boot-chain manifest the medium built;
#   * `\boot\limine\limine-bios.sys` is the pinned 12.3.2 loader, 290,964
#     bytes, byte-identical to `deps/limine/limine-bios.sys`.
assert_bios_boot_volume() {
    local disk=$1
    local label=$2
    local medium_iso_root=$3
    local menu
    local chain
    local staged_kernel
    local manifest
    local loader
    local loader_bytes

    menu=$TEST_DIR/$(basename "$disk").bios-menu
    chain=$TEST_DIR/$(basename "$disk").bios-chain
    staged_kernel=$TEST_DIR/$(basename "$disk").bios-kernel
    manifest=$TEST_DIR/$(basename "$disk").bios-manifest
    loader=$TEST_DIR/$(basename "$disk").bios-loader

    [[ -f "$disk" ]] || {
        echo "$label: the installed BIOS disk is missing: $disk" >&2
        return 1
    }
    # The installed LBA0 is the measured stage1 recipe *and* the one repair
    # `limine bios-install` applies to an MBR disk whose table is not bootable:
    # partition 1 carries the active flag.  A firmware that consults the flag
    # before it runs the boot sector - the L875's InsydeH2O does - skips a disk
    # without it and never reaches stage1; SeaBIOS does not, so this is the only
    # place the property is asserted before the hardware run.
    python3 - "$disk" "$label" <<'PY' || return 1
import sys

disk, label = sys.argv[1], sys.argv[2]
with open(disk, "rb") as handle:
    lba0 = handle.read(512)
if len(lba0) != 512 or lba0[510:512] != b"\x55\xaa":
    print("%s: LBA0 carries no 55aa boot signature" % label, file=sys.stderr)
    sys.exit(1)
flags = [lba0[446 + index * 16] for index in range(4)]
if flags[0] != 0x80 or any(flag & 0x80 for flag in flags[1:]):
    print("%s: LBA0 active entry is %r, expected partition 1 alone"
          % (label, flags), file=sys.stderr)
    sys.exit(1)
print("installed LBA0: partition 1 active (0x80), signature 55aa")
PY
    python3 scripts/hfoc-esp-read.py --disk "$disk" --partition-index 1 \
        --path 'boot\limine\limine.conf' >"$menu" || {
        echo "$label: cannot read the BIOS menu from $disk" >&2
        return 1
    }
    for fragment in \
        'timeout: 10' \
        '/HFOC OS' \
        '    path: boot():/boot/hfoc-kernel.elf' \
        '    module_path: boot():/boot/hfoc-boot.man' \
        '    module_path: boot():/boot/hfoc/limine.conf' \
        '    cmdline: hfoc.management=local-only' \
        '# disk id=1 reason=NO_ESP'; do
        grep -Fq "$fragment" "$menu" || {
            echo "$label: the installed BIOS menu does not carry: $fragment" >&2
            return 1
        }
    done
    if grep -Fq 'protocol: efi' "$menu"; then
        echo "$label: the installed BIOS menu offers an EFI chainload entry" >&2
        return 1
    fi
    printf '%s\n' \
        "installed BIOS menu: $(wc -c <"$menu") bytes, appliance entry," \
        "  chain-configuration module and NO_ESP comments, no EFI entry"

    python3 scripts/hfoc-esp-read.py --disk "$disk" --partition-index 1 \
        --path 'boot\hfoc\limine.conf' >"$chain" || {
        echo "$label: cannot read the BIOS chain configuration from $disk" >&2
        return 1
    }
    cmp -s "$chain" "$medium_iso_root/boot/hfoc-installed.conf" || {
        echo "$label: the BIOS chain configuration is not the medium's" \
            "sealed one" >&2
        return 1
    }
    printf '%s\n' \
        "installed BIOS chain configuration: $(wc -c <"$chain") bytes," \
        "identical to the medium's sealed configuration"

    python3 scripts/hfoc-esp-read.py --disk "$disk" --partition-index 1 \
        --path 'boot\hfoc-kernel.elf' >"$staged_kernel" || {
        echo "$label: cannot read the BIOS kernel from $disk" >&2
        return 1
    }
    cmp -s "$staged_kernel" "$medium_iso_root/boot/hfoc-kernel.elf" || {
        echo "$label: the BIOS kernel is not the medium's product kernel" >&2
        return 1
    }
    if bash scripts/hfoc-file-marker.sh "$staged_kernel" \
            '[PRODUCTION-INSTALLER] proof init'; then
        echo "$label: the BIOS kernel is an installer build" >&2
        return 1
    fi
    printf '%s\n' \
        "installed BIOS kernel: $(wc -c <"$staged_kernel") bytes, identical" \
        "  to the medium's product kernel and carrying no installer marker"

    python3 scripts/hfoc-esp-read.py --disk "$disk" --partition-index 1 \
        --path 'boot\hfoc-boot.man' >"$manifest" || {
        echo "$label: cannot read the BIOS manifest from $disk" >&2
        return 1
    }
    cmp -s "$manifest" "$medium_iso_root/boot/hfoc-installed.chain" || {
        echo "$label: the BIOS manifest is not the medium's sealed chain" >&2
        return 1
    }
    printf '%s\n' \
        "installed BIOS manifest: $(wc -c <"$manifest") bytes, identical to" \
        "  the medium's sealed boot chain"

    python3 scripts/hfoc-esp-read.py --disk "$disk" --partition-index 1 \
        --path 'boot\limine\limine-bios.sys' >"$loader" || {
        echo "$label: cannot read limine-bios.sys from $disk" >&2
        return 1
    }
    loader_bytes=$(wc -c <"$loader")
    [[ "$loader_bytes" == 290964 ]] || {
        echo "$label: limine-bios.sys is $loader_bytes bytes, not 290964" >&2
        return 1
    }
    cmp -s "$loader" deps/limine/limine-bios.sys || {
        echo "$label: limine-bios.sys is not the pinned 12.3.2 loader" >&2
        return 1
    }
    printf '%s\n' \
        "installed BIOS loader: limine-bios.sys $loader_bytes bytes," \
        "  identical to the pinned 12.3.2 release"
    return 0
}

# assert_boot_log DISK LABEL
#
# ITEM-410: the appliance writes its own boot log to its own disk, so a hardware
# run can be read afterwards instead of photographed.  The log is read here out
# of the raw image by the host reader, independently of the kernel - and the
# install flow attached it to the ESP role it staged *before* the reboot, so one
# sequence holds the records of the install and the records of the boot that
# followed it.  That is what the INSTALLER and FIRST_BOOT stages of the hardware
# acceptance run quote.
assert_boot_log() {
    local disk=$1
    local label=$2
    local output errors

    output=$TEST_DIR/$(basename "$disk").bootlog
    errors=$TEST_DIR/$(basename "$disk").bootlog.stderr

    if ! python3 scripts/hfoc-esp-read.py "$disk" --bootlog \
        >"$output" 2>"$errors"; then
        echo "$label: cannot read the appliance boot log from $disk" >&2
        cat "$errors" >&2
        return 1
    fi
    head -n 1 "$output"
    # The reader warns honestly about the records a wrap left behind; a healthy
    # log is the summary line plus at least the install and the boot.
    #
    # ITEM-496: a disk that has booted through the menu several times fills its
    # 512-record ring, and the reader's own summary then says `wrapped=YES`: the
    # install-era records are evicted by design, so they can only be required
    # while the ring has not wrapped.  The records of the boot just made are
    # required either way - a wrapped log is still a log that must carry them.
    #
    # ITEM-517: a ring that overflowed mid-boot also drops records, and the
    # summary then says `dropped=N`: the gap a drop leaves breaks the reader's
    # contiguous run before the install-era records exactly as a wrap does, so
    # the install record is required only of a log whose run still reaches back
    # to it.  The gate stays on the summary's own numbers - a run that lost its
    # head to a torn slot still has `dropped=0` and still fails here.
    local summary
    summary=$(head -n 1 "$output")
    local -a markers=(
        '[BOOT-CHAIN] verdict=ATTESTED'
        '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK'
        '[APPLIANCE-BOOT] runtime=READY identity=OK'
        '[BOOTLOG] state=READY path=\EFI\HFOC\BOOTLOG.BIN'
    )
    if [[ "$summary" == *wrapped=YES* || "$summary" != *dropped=0* ]]; then
        echo "$label: the boot log evicted the install-era records" \
            "(wrapped or dropped: $summary)"
    else
        markers=('[PRODUCTION-INSTALL] boot-entry state=AVAILABLE' "${markers[@]}")
    fi
    for marker in "${markers[@]}"; do
        grep -Fq "$marker" "$output" || {
            echo "$label: the appliance boot log does not carry: $marker" >&2
            return 1
        }
    done
    local records
    records=$(grep -c '^#' "$output")
    (( records >= 8 )) || {
        echo "$label: the appliance boot log carries only $records records" >&2
        return 1
    }
    echo "$label: appliance boot log: $records records, the install and the" \
        "boot in one sequence"
    return 0
}

# assert_input_verdict DISK LABEL
#
# ITEM-497: the installed appliance's own boot left its input verdict on the
# disk it came up on.  This is the producer, the flush marker and the ESP write
# exercised by a real boot, read back by the host reader - an installer boot
# publishes no verdict, because the input path's own publication is the
# appliance loop's and the installer does not run it.
assert_input_verdict() {
    local disk=$1
    local label=$2
    local output errors

    output=$TEST_DIR/$(basename "$disk").input-verdict
    errors=$TEST_DIR/$(basename "$disk").input-verdict.stderr

    if ! python3 scripts/hfoc-esp-read.py "$disk" --bootlog \
        >"$output" 2>"$errors"; then
        echo "$label: cannot read the boot log for the input verdict" >&2
        cat "$errors" >&2
        return 1
    fi
    if ! grep -Eq \
        '\[INPUT-PATH\] boot=[0-9]+ verdict=(ARMED|NO_REPORTS|NOT_ARMED|NO_DEVICE)' \
        "$output"; then
        echo "$label: the installed boot carries no input verdict" >&2
        return 1
    fi
    grep -oE '\[INPUT-PATH\] boot=[0-9]+ verdict=[A-Z_]+' "$output" | tail -n 1
    return 0
}

# assert_three_boot_verdicts DISK LABEL
#
# ITEM-497's acceptance, on a real boot: the same installed disk is booted three
# times and every boot's input verdict is still readable from BOOTLOG.BIN
# afterwards.  One boot publishes thousands of records into a ring of 511 slots,
# so the first boot's verdict survives only because the log carries it forward
# and re-states it with the loop heartbeat; a log that simply wrapped would show
# the newest verdict and nothing else.
assert_three_boot_verdicts() {
    local disk=$1
    local label=$2
    local output errors boots carries

    output=$TEST_DIR/$(basename "$disk").verdicts
    errors=$TEST_DIR/$(basename "$disk").verdicts.stderr

    if ! python3 scripts/hfoc-esp-read.py "$disk" --bootlog \
        >"$output" 2>"$errors"; then
        echo "$label: cannot read the boot log for the verdict proof" >&2
        cat "$errors" >&2
        return 1
    fi
    boots=$(grep -oE '\[INPUT-PATH\] boot=[0-9]+' "$output" | sort -u | wc -l)
    carries=$(grep -c '\[INPUT-PATH-CARRY\] boot=' "$output" || true)
    printf '%s\n' "input verdicts: $boots boots, $carries carried copies"
    (( boots >= 3 )) || {
        echo "$label: the log carries $boots input verdicts, not three" >&2
        return 1
    }
    # The carried copies are the mechanism that keeps a verdict alive across the
    # thousands of records a long boot publishes; a short proof boot is killed
    # before the loop's ten-second digest, and its three verdicts survive in the
    # ring on their own - the host store proof drives the digest itself.
    return 0
}

# ------------------------------------------------- the stale ESP (ITEM-405) ---
# The acceptance host installed onto a disk that had carried a Windows layout.
# The partition table was replaced by ours, but the 33 MiB ESP role still held
# the old system's FAT32 volume: its boot sector said OEM "MSDOS5.0",
# reserved_sectors=6654, two FATs, sectors_per_fat=769, sectors_per_cluster=2
# and total_sectors=204800 - the *100 MiB* volume the previous table had -
# inside a 67584-sector role, and its directory tree still held a hand-copied
# Limine tree.  The bind compared the boot sector only with the partition it was
# given and never read the volume's own total-sector count, so the stale volume
# bound successfully, was never formatted, and the installed boot set was staged
# against a geometry the volume does not have.
#
# This case puts exactly that volume into the region the whole-disk layout
# declares as its ESP role, runs the declared whole-disk install over it, and
# then requires - independently of the kernel - that the role carries our own
# volume and our own staged set, and that the disk boots on its own.
STALE_ESP_FIRST=2048
STALE_ESP_LENGTH=67584

# prepare_stale_esp_disk DISK_BYTES DISK
#
# A sparse image of the declared size with the foreign volume written into the
# ESP region.  The declared volume extends past the partition (that is the
# defect), but every sector this fixture writes stays inside the 67584-sector
# role, so nothing of the fixture lands in a different role of the layout.
prepare_stale_esp_disk() {
    local disk_bytes=$1
    local disk=$2

    truncate -s "$disk_bytes" "$disk"
    python3 - "$disk" "$STALE_ESP_FIRST" "$STALE_ESP_LENGTH" <<'PY'
import struct
import sys

disk_path, esp_first, esp_length = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

SECTOR = 512
OEM = b"MSDOS5.0"
SECTORS_PER_CLUSTER = 2
RESERVED_SECTORS = 6654
FAT_COUNT = 2
SECTORS_PER_FAT = 769
TOTAL_SECTORS = 204800
ROOT_CLUSTER = 2
DATA_START = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
CLUSTER_BYTES = SECTORS_PER_CLUSTER * SECTOR


def le16(value):
    return struct.pack("<H", value)


def le32(value):
    return struct.pack("<I", value)


def boot_sector():
    sector = bytearray(SECTOR)
    sector[0:3] = b"\xeb\x58\x90"
    sector[3:11] = OEM
    sector[11:13] = le16(SECTOR)
    sector[13] = SECTORS_PER_CLUSTER
    sector[14:16] = le16(RESERVED_SECTORS)
    sector[16] = FAT_COUNT
    sector[19:21] = le16(0)
    sector[21] = 0xF8
    sector[24:26] = le16(32)
    sector[26:28] = le16(64)
    sector[28:32] = le32(esp_first)
    sector[32:36] = le32(TOTAL_SECTORS)      # the stale 100 MiB extent
    sector[36:40] = le32(SECTORS_PER_FAT)
    sector[44:48] = le32(ROOT_CLUSTER)
    sector[48:50] = le16(1)
    sector[50:52] = le16(6)
    sector[64] = 0x80
    sector[66] = 0x29
    sector[67:71] = le32(0x12345678)
    sector[71:82] = b"MSDOS5.0   "
    sector[82:90] = b"FAT32   "
    sector[510:512] = b"\x55\xaa"
    return sector


def fsinfo_sector():
    sector = bytearray(SECTOR)
    sector[0:4] = le32(0x41615252)
    sector[484:488] = le32(0x61417272)
    sector[488:492] = le32(0xFFFFFFFF)
    sector[492:496] = le32(0xFFFFFFFF)
    sector[510:512] = b"\x55\xaa"
    return sector


def short_entry(name, attributes, cluster, size):
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = attributes
    entry[20:22] = le16((cluster >> 16) & 0xFFFF)
    entry[26:28] = le16(cluster & 0xFFFF)
    entry[28:32] = le32(size)
    return entry


ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20

# Cluster allocation: the directories first, then the files, in one run.
next_cluster = ROOT_CLUSTER + 1
directories = {}
files = []


def allocate_directory(name):
    global next_cluster
    cluster = next_cluster
    next_cluster += 1
    directories[name] = cluster
    return cluster


def allocate_file(path, name, size, fill):
    global next_cluster
    clusters = (size + CLUSTER_BYTES - 1) // CLUSTER_BYTES
    first = next_cluster
    next_cluster += clusters
    files.append((path, name, size, fill, first, clusters))
    return first


efi_cluster = allocate_directory("EFI")
efi_boot_cluster = allocate_directory("EFI\\BOOT")
boot_cluster = allocate_directory("BOOT")
limine_cluster = allocate_directory("BOOT\\LIMINE")
bootx64_first = allocate_file("EFI\\BOOT", b"BOOTX64 EFI", 360448,
                              0x42)
old_kernel_first = allocate_file("BOOT", b"HKOC-K~1ELF", 3025224, 0x37)
limine_bios_first = allocate_file("BOOT\\LIMINE", b"LIMINE~1SYS", 4096, 0x5A)
limine_uefi_first = allocate_file("BOOT\\LIMINE", b"LIMINE~2BIN", 4096, 0x6B)

# The FAT: the reserved entries, the root, every directory and every chain.
fat = bytearray(SECTORS_PER_FAT * SECTOR)


def fat_set(cluster, value):
    fat[cluster * 4:cluster * 4 + 4] = le32(value)


fat_set(0, 0x0FFFFFF8)
fat_set(1, 0x0FFFFFFF)
fat_set(ROOT_CLUSTER, 0x0FFFFFFF)
for cluster in directories.values():
    fat_set(cluster, 0x0FFFFFFF)
for _, _, _, _, first, count in files:
    for index in range(count):
        fat_set(first + index,
                0x0FFFFFFF if index + 1 == count else first + index + 1)

# The directories, as one cluster each.
root_directory = bytearray(CLUSTER_BYTES)
root_directory[0:32] = short_entry(b"EFI        ", ATTR_DIRECTORY,
                                   efi_cluster, 0)
root_directory[32:64] = short_entry(b"BOOT       ", ATTR_DIRECTORY,
                                    boot_cluster, 0)

efi_directory = bytearray(CLUSTER_BYTES)
efi_directory[0:32] = short_entry(b".          ", ATTR_DIRECTORY,
                                  efi_cluster, 0)
efi_directory[32:64] = short_entry(b"..         ", ATTR_DIRECTORY,
                                   ROOT_CLUSTER, 0)
efi_directory[64:96] = short_entry(b"BOOT       ", ATTR_DIRECTORY,
                                   efi_boot_cluster, 0)

efi_boot_directory = bytearray(CLUSTER_BYTES)
efi_boot_directory[0:32] = short_entry(b".          ", ATTR_DIRECTORY,
                                       efi_boot_cluster, 0)
efi_boot_directory[32:64] = short_entry(b"..         ", ATTR_DIRECTORY,
                                        efi_cluster, 0)
efi_boot_directory[64:96] = short_entry(b"BOOTX64 EFI", ATTR_ARCHIVE,
                                        bootx64_first, 360448)

boot_directory = bytearray(CLUSTER_BYTES)
boot_directory[0:32] = short_entry(b".          ", ATTR_DIRECTORY,
                                   boot_cluster, 0)
boot_directory[32:64] = short_entry(b"..         ", ATTR_DIRECTORY,
                                    ROOT_CLUSTER, 0)
boot_directory[64:96] = short_entry(b"LIMINE     ", ATTR_DIRECTORY,
                                    limine_cluster, 0)
boot_directory[96:128] = short_entry(b"HKOC-K~1ELF", ATTR_ARCHIVE,
                                     old_kernel_first, 3025224)

limine_directory = bytearray(CLUSTER_BYTES)
limine_directory[0:32] = short_entry(b".          ", ATTR_DIRECTORY,
                                     limine_cluster, 0)
limine_directory[32:64] = short_entry(b"..         ", ATTR_DIRECTORY,
                                      boot_cluster, 0)
limine_directory[64:96] = short_entry(b"LIMINE~1SYS", ATTR_ARCHIVE,
                                      limine_bios_first, 4096)
limine_directory[96:128] = short_entry(b"LIMINE~2BIN", ATTR_ARCHIVE,
                                       limine_uefi_first, 4096)

directory_clusters = {
    ROOT_CLUSTER: root_directory,
    efi_cluster: efi_directory,
    efi_boot_cluster: efi_boot_directory,
    boot_cluster: boot_directory,
    limine_cluster: limine_directory,
}


def cluster_lba(cluster):
    return esp_first + DATA_START + (cluster - ROOT_CLUSTER) * \
        SECTORS_PER_CLUSTER


written = []


def write_at(lba, payload):
    handle.seek(lba * SECTOR)
    handle.write(payload)
    written.append(lba + len(payload) // SECTOR)


with open(disk_path, "r+b") as handle:
    handle.seek(esp_first * SECTOR)
    handle.write(boot_sector())
    written.append(esp_first + 1)
    handle.seek((esp_first + 1) * SECTOR)
    handle.write(fsinfo_sector())
    written.append(esp_first + 2)
    handle.seek((esp_first + 6) * SECTOR)
    handle.write(boot_sector())
    written.append(esp_first + 7)
    for copy in range(FAT_COUNT):
        handle.seek((esp_first + RESERVED_SECTORS +
                     copy * SECTORS_PER_FAT) * SECTOR)
        handle.write(fat)
        written.append(esp_first + RESERVED_SECTORS +
                       (copy + 1) * SECTORS_PER_FAT)
    for cluster, payload in directory_clusters.items():
        write_at(cluster_lba(cluster), payload)
    for _, _, size, fill, first, count in files:
        for index in range(count):
            length = min(CLUSTER_BYTES, size - index * CLUSTER_BYTES)
            write_at(cluster_lba(first + index), bytes([fill]) * length)

# The fixture must stay inside the role the whole-disk layout declares, so the
# proof does not prepare one role by writing into another.
limit = esp_first + esp_length
if max(written) > limit:
    raise SystemExit(
        f"the stale-volume fixture wrote past the ESP role ({max(written)} > "
        f"{limit})")
print(f"stale-esp: foreign FAT32 total_sectors={TOTAL_SECTORS} inside a "
      f"{esp_length}-sector role, {len(written)} sectors written, "
      f"highest LBA {max(written)} < {limit}")
PY
}

# assert_hfoc_esp_geometry DISK
#
# The role has to carry the volume the installer formats: its own OEM name and
# the geometry of the partition it lives in.  Read out of the raw image here,
# with no kernel involvement.
assert_hfoc_esp_geometry() {
    local disk=$1

    python3 - "$disk" "$STALE_ESP_FIRST" "$STALE_ESP_LENGTH" <<'PY'
import struct
import sys

disk_path, esp_first, esp_length = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

with open(disk_path, "rb") as handle:
    handle.seek(esp_first * 512)
    sector = handle.read(512)


def require(condition, message):
    if not condition:
        raise SystemExit(f"stale-esp: {message}")


oem = sector[3:11]
reserved = struct.unpack_from("<H", sector, 14)[0]
fat_count = sector[16]
spc = sector[13]
total = struct.unpack_from("<I", sector, 32)[0]
sectors_per_fat = struct.unpack_from("<I", sector, 36)[0]
root_cluster = struct.unpack_from("<I", sector, 44)[0]

require(oem == b"HFOC    ",
        f"the ESP role does not carry the installer's own volume: OEM {oem!r}")
require(total == esp_length,
        f"the volume declares {total} sectors, the role is {esp_length}")
require(reserved == 32 and fat_count == 2 and spc == 1 and root_cluster == 2,
        f"unexpected formatted geometry: reserved={reserved} "
        f"fat_count={fat_count} spc={spc} root={root_cluster}")
require(sectors_per_fat * 128 >= 2 + (esp_length - reserved -
                                      fat_count * sectors_per_fat) // spc,
        "the formatted FAT does not address the whole role")
require(total <= esp_length,
        "the volume declares an extent larger than the partition")
print(f"stale-esp: ESP role carries OEM={oem.decode()!r} total_sectors={total} "
      f"reserved={reserved} fat_count={fat_count} spc={spc} "
      f"sectors_per_fat={sectors_per_fat}")
PY
}

# assert_stale_tree_unreachable DISK
#
# The old system's tree is not reachable through the volume the install
# produced: its directory entry is not in the formatted root, whatever bytes of
# its file data still sit in unreferenced clusters.
assert_stale_tree_unreachable() {
    local disk=$1

    if python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'boot\HKOC-K~1.ELF' >/dev/null 2>&1; then
        echo "the stale foreign tree is still reachable on the ESP role" >&2
        return 1
    fi
    if python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'boot\limine\limine-bios.sys' >/dev/null 2>&1; then
        echo "the stale foreign loader is still reachable on the ESP role" >&2
        return 1
    fi
    printf '%s\n' \
        "stale-esp: the foreign tree the replaced table left behind is not" \
        "  reachable through the volume the install produced"
    return 0
}

run_boot "management" uefi "$MANAGEMENT_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset
run_boot "authorised" uefi "$AUTHORISED_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset

boot_installed_disk "$TEST_DIR/authorised.raw" authorised \
    "$AUTHORISED_ISO_ROOT" local-only LOCAL_ONLY
# ITEM-582: the same installed system exports its diagnosis to a separate HFCD
# stick, attached before the boot and then (a fresh boot) plugged in after it,
# with a foreign disk in front of the stick on the same hub.
assert_installed_diag_export "$TEST_DIR/authorised.raw" diag-before before || exit 1
assert_installed_diag_export "$TEST_DIR/authorised.raw" diag-after after || exit 1
# ITEM-590: the same installed system, with the diagnostic stick on the
# controller the owner's laptop uses and attached at boot, so the boot walk
# binds it and the machine's telemetry fills the reserved journal before the
# operator's command - the condition the hardware run was refused in.  The save
# must recover the room through the journal's own rewind and the second save
# must be a session of its own.
assert_installed_diag_export "$TEST_DIR/authorised.raw" diag-full full || exit 1
boot_installed_disk "$TEST_DIR/management.raw" management \
    "$MANAGEMENT_ISO_ROOT" management-interface MANAGEMENT_INTERFACE

# ITEM-509 increment 27 (509.7c-2): the medium that declares Russian.  The
# install writes the declared language into the installed state immediately
# after that state exists, reads it back through the installed system's own
# reader and only then publishes `language=ru persisted=VERIFIED` - which is
# the marker the boot below waits for, so the reset that follows a completed
# install is observed exactly as in the English cases.  The record is then read
# from the raw persistence volume independently of the kernel's own claim; an
# English install never writes that store at all, which is why the authorised
# case's zero-journal assertion stays valid and does not move.
run_boot "language" uefi "$LANGUAGE_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" \
    "[PRODUCTION-INSTALL] language=ru persisted=VERIFIED" "" reset
if ! python3 - "$TEST_DIR/language.raw" "$TEST_DIR/language.log" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import sys

disk_path, serial_path = sys.argv[1], sys.argv[2]
def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

text = SERIAL_VIEW.normalise(
    pathlib.Path(serial_path).read_bytes().decode("ascii", errors="replace"))
match = re.search(
    r"\[INSTALLER-LAYOUT\] role=hfoc-persistence first_lba=(\d+) "
    r"length_lba=(\d+)", text)
if match is None:
    raise SystemExit("the layout record does not name the persistence role")
# The first store of the persistence volume is the system configuration: its
# data-region header is block 0 and its journal begins at block 1.  The record
# is read from those blocks, not from the serial transcript.
with open(disk_path, "rb") as handle:
    handle.seek(int(match.group(1)) * 512)
    region = handle.read(64 * 512)
if b"v1:system.language=ru" not in region:
    raise SystemExit("the installed state does not carry the declared language")
print("language: the installed state carries the declared language")
PY
then
    echo "language: the installed state does not carry the declared language" >&2
    exit 1
fi

# ITEM-446: the same shape with a declared network.  The medium carries
# `HFOC-INSTALL-NETWORK=static` with the address, the gateway and the DNS, the
# install plans and applies it, the installed entry states it, and the installed
# disk boots on that address without a DHCP exchange.
run_boot "network" uefi "$NETWORK_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset
grep -Fq "network=static address=$NETWORK_ADDRESS gateway=$NETWORK_GATEWAY dns=$NETWORK_DNS" \
    "$TEST_DIR/network.log" || {
    echo "network: the install record does not name the declared network" >&2
    exit 1
}
boot_installed_disk "$TEST_DIR/network.raw" network \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" \
    "$NETWORK_ADDRESS"

# ITEM-447: the address the medium did not declare, set from the machine's own
# menu.  The gesture is the operator's own - `menu`, DOWN to Network, ENTER and
# the four answers - and the machine leaves through the menu's Reboot entry, so
# the boot after it reads back what the menu wrote.
# ITEM-449: the forward the medium declared.  The same install shape as the
# network medium, and the installed disk publishes the rule it was installed
# with - the declaration survives on the installed boot entry, and an installed
# appliance that declares nothing publishes UNAVAILABLE instead.
run_boot "forward" uefi "$FORWARD_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset
grep -Fq "forward=on target=$FORWARD_TARGET target-port=$FORWARD_TARGET_PORT listen-port=$FORWARD_LISTEN_PORT clients=$FORWARD_CLIENTS" \
    "$TEST_DIR/forward.log" || {
    echo "forward: the install record does not name the declared forward" >&2
    exit 1
}
boot_installed_disk "$TEST_DIR/forward.raw" forward \
    "$FORWARD_ISO_ROOT" management-interface MANAGEMENT_INTERFACE
grep -Fq "[PRODUCTION-FORWARD] state=DECLARED target=$FORWARD_TARGET:$FORWARD_TARGET_PORT listen=$FORWARD_LISTEN_PORT clients=1" \
    "$TEST_DIR/forward-installed-boot.log" || {
    echo "forward: the installed disk does not publish the declared forward" >&2
    exit 1
}
# The disk that declared no forward states the absence rather than leaving the
# question unanswered.
grep -Fq "[PRODUCTION-FORWARD] state=UNAVAILABLE reason=NO_DECLARATION" \
    "$TEST_DIR/network-installed-boot.log" || {
    echo "forward: a disk without a declaration does not publish its absence" >&2
    exit 1
}

# ITEM-496: the three answers the operator can give, each driven through the
# machine's own menu and each measured on the boot that follows it.
#
# 1. The operator's address is refused (no prefix the IPv4 owner accepts): the
#    refusal names the reason and the disk comes back on the address it had -
#    a refused answer changed nothing.
boot_installed_disk "$TEST_DIR/network.raw" network-menu-refused \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" "" \
    menu "" bad-address
boot_installed_disk "$TEST_DIR/network.raw" network-menu-refused-persisted \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" \
    "$NETWORK_ADDRESS"
# 2. The operator's address is accepted: the next boot comes up on it and the
#    address answers ARP and ICMP on the wire.
boot_installed_disk "$TEST_DIR/network.raw" network-menu \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" "" \
    menu "$NETWORK_MENU_ADDRESS"
boot_installed_disk "$TEST_DIR/network.raw" network-menu-persisted \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" \
    "$NETWORK_MENU_ADDRESS" "" "" static icmp
# 3. The operator goes back to discovery: the next boot publishes DHCP, the
#    lease is granted again, and the static address is gone.
boot_installed_disk "$TEST_DIR/network.raw" network-menu-dhcp \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" "" \
    menu "" dhcp
boot_installed_disk "$TEST_DIR/network.raw" network-menu-dhcp-persisted \
    "$NETWORK_ISO_ROOT" management-interface MANAGEMENT_INTERFACE "" "" \
    expect-dhcp

# ITEM-405: the acceptance host's shape.  The target carries the stale foreign
# FAT32 in the region the whole-disk layout declares as its ESP role, the
# declared whole-disk medium installs over it, and the role, the staged set and
# the boot are all read back independently.
prepare_stale_esp_disk "$ACCEPTANCE_BYTES" "$TEST_DIR/stale-esp.raw"
run_boot "stale-esp" uefi "$WHOLE_DISK_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" \
    "[PRODUCTION-INSTALL] layout mode=whole-disk reason=declared-whole-disk declared=1" \
    reset
assert_hfoc_esp_geometry "$TEST_DIR/stale-esp.raw"
assert_stale_tree_unreachable "$TEST_DIR/stale-esp.raw"
assert_installed_configuration "$TEST_DIR/stale-esp.raw" \
    "$WHOLE_DISK_ISO_ROOT/boot/hfoc-installed.conf" local-only
boot_installed_disk "$TEST_DIR/stale-esp.raw" stale-esp \
    "$WHOLE_DISK_ISO_ROOT" local-only LOCAL_ONLY

# ------------------------------------ the machine's other disk (ITEM-409) ---
# The acceptance server's shape: the appliance goes to the dedicated disk and
# the machine's other disk carries Debian.  The install has to offer that system
# in the menu it writes - through Limine's `guid(<GPT partition GUID>)` volume,
# because a firmware drive number is not stable - and it must not write a byte
# of that disk.  The owner's rule is the same record: a machine that carries
# nothing but this appliance still gets a menu that shows this appliance, and a
# machine that carries something else gets a menu that offers both.
OTHER_DISK_BYTES=$((8 * 1024 * 1024 * 1024))
OTHER_DISK_ESP_FIRST=2048
OTHER_DISK_ESP_LENGTH=1048576

# build_other_system_disk DISK STATE
#
# A synthetic Debian-like disk: a protective MBR, a GPT with an EFI System
# Partition carrying a stable unique GUID and one Linux partition, and a FAT32
# ESP in the geometry a real 512 MiB ESP has (32 reserved sectors, two FATs,
# 1022 sectors per FAT, 4 KiB clusters) holding `\EFI\DEBIAN\SHIMX64.EFI`,
# `\EFI\DEBIAN\GRUBX64.EFI`, `\EFI\DEBIAN\grub.cfg` and the removable-media
# fallback `\EFI\BOOT\BOOTX64.EFI`.  Both Debian loaders are there on purpose,
# and GRUB is the *first* entry in `\EFI\DEBIAN`: the shim is what the firmware
# boots, so the chainload entry has to name the shim even though a reader that
# took the first loader it met, or one that preferred GRUB, would not.
#
# Every file occupies a real cluster chain - one FAT entry per cluster, the last
# one end-of-chain, and the clusters a multiple of the volume's 4 KiB cluster -
# because a fixture that wrote one sector per cluster would be describing a
# volume no formatter produces and the reader would be right to refuse it.
build_other_system_disk() {
    local disk=$1
    local state=$2

    truncate -s "$OTHER_DISK_BYTES" "$disk"
    python3 - "$disk" "$state" "$((OTHER_DISK_BYTES / 512))" \
        "$OTHER_DISK_ESP_FIRST" "$OTHER_DISK_ESP_LENGTH" <<'PY'
import binascii
import json
import pathlib
import struct
import sys

(disk_path, state_path, media_lba, esp_first, esp_length) = (
    sys.argv[1], sys.argv[2], *[int(value) for value in sys.argv[3:]])
SECTOR = 512
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ARRAY_SECTORS = ENTRY_COUNT * ENTRY_SIZE // SECTOR
ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
LINUX_TYPE = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
ESP_UNIQUE = bytes.fromhex("11111111222233334444555555555555")
ROOT_UNIQUE = bytes.fromhex("66666666777788889999aaaaaaaaaaaa")
ESP_RESERVED = 32
ESP_FAT_COUNT = 2
ESP_SECTORS_PER_FAT = 1022
ESP_SECTORS_PER_CLUSTER = 8
ESP_ROOT = 2
ESP_EFI = 3
ESP_DEBIAN = 4
ESP_BOOT = 5
ESP_FIRST_FILE_CLUSTER = 6
SHIM_LENGTH = 900000
GRUB_LENGTH = 700000
GRUB_CFG_LENGTH = 27
FALLBACK_LENGTH = 800000
ROOT_FIRST = esp_first + esp_length
ROOT_LENGTH = 2000000
ESP_DATA_START = ESP_RESERVED + ESP_FAT_COUNT * ESP_SECTORS_PER_FAT
CLUSTER_BYTES = ESP_SECTORS_PER_CLUSTER * SECTOR


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def guid_text(raw):
    return "%08x-%04x-%04x-%s-%s" % (
        struct.unpack_from("<I", raw, 0)[0],
        struct.unpack_from("<H", raw, 4)[0],
        struct.unpack_from("<H", raw, 6)[0],
        raw[8:10].hex(), raw[10:16].hex())


def entry(slot, type_guid, unique, first, last, name):
    out = bytearray(ENTRY_SIZE)
    out[0:16] = type_guid
    out[16:32] = unique
    struct.pack_into("<Q", out, 32, first)
    struct.pack_into("<Q", out, 40, last)
    for index, character in enumerate(name[:35]):
        out[56 + index * 2] = ord(character)
    return bytes(out)


def header(my_lba, alternate_lba, entry_lba, array, seed):
    out = bytearray(SECTOR)
    out[0:8] = b"EFI PART"
    out[10] = 0x01
    struct.pack_into("<I", out, 12, 92)
    struct.pack_into("<Q", out, 24, my_lba)
    struct.pack_into("<Q", out, 32, alternate_lba)
    struct.pack_into("<Q", out, 40, 34)
    struct.pack_into("<Q", out, 48, media_lba - 34)
    out[56:72] = bytes((seed + index) & 0xFF for index in range(16))
    struct.pack_into("<Q", out, 72, entry_lba)
    struct.pack_into("<I", out, 80, ENTRY_COUNT)
    struct.pack_into("<I", out, 84, ENTRY_SIZE)
    struct.pack_into("<I", out, 88, crc32(array))
    struct.pack_into("<I", out, 16, crc32(bytes(out[0:92])))
    return bytes(out)


array = bytearray(ARRAY_SECTORS * SECTOR)
array[0:ENTRY_SIZE] = entry(0, ESP_TYPE, ESP_UNIQUE, esp_first,
                            esp_first + esp_length - 1,
                            "EFI System Partition")
array[ENTRY_SIZE:2 * ENTRY_SIZE] = entry(1, LINUX_TYPE, ROOT_UNIQUE,
                                         ROOT_FIRST,
                                         ROOT_FIRST + ROOT_LENGTH - 1,
                                         "Debian GNU/Linux")
array = bytes(array)

mbr = bytearray(SECTOR)
for index in range(446):
    mbr[index] = 0x40 + (index % 5)
mbr[446 + 4] = 0xEE
struct.pack_into("<I", mbr, 446 + 8, 1)
struct.pack_into("<I", mbr, 446 + 12, 0xFFFFFFFF)
mbr[510] = 0x55
mbr[511] = 0xAA

boot_sector = bytearray(SECTOR)
boot_sector[3:11] = b"mkfs.fat"
boot_sector[11:13] = struct.pack("<H", SECTOR)
boot_sector[13] = ESP_SECTORS_PER_CLUSTER
boot_sector[14:16] = struct.pack("<H", ESP_RESERVED)
boot_sector[16] = ESP_FAT_COUNT
boot_sector[21] = 0xF8
boot_sector[24:26] = struct.pack("<H", 63)
boot_sector[26:28] = struct.pack("<H", 255)
boot_sector[28:32] = struct.pack("<I", esp_first)
boot_sector[32:36] = struct.pack("<I", esp_length)
struct.pack_into("<I", boot_sector, 36, ESP_SECTORS_PER_FAT)
boot_sector[44:48] = struct.pack("<I", ESP_ROOT)
struct.pack_into("<H", boot_sector, 48, 0xFFFF)
struct.pack_into("<H", boot_sector, 50, 0xFFFF)
boot_sector[64] = 0x80
boot_sector[66] = 0x29
struct.pack_into("<I", boot_sector, 67, 0x12345678)
boot_sector[71:82] = b"NO NAME    "
boot_sector[82:90] = b"FAT32   "
boot_sector[510] = 0x55
boot_sector[511] = 0xAA


def payload(length, seed):
    return bytes((seed + (index % 13)) & 0xFF for index in range(length))


shim_bytes = payload(SHIM_LENGTH, 0x90)
grub_bytes = payload(GRUB_LENGTH, 0xA0)
grub_cfg_bytes = b"search.fs_uuid 1234 root\n" + b"\x00" * (
    GRUB_CFG_LENGTH - 27)
fallback_bytes = payload(FALLBACK_LENGTH, 0xB0)


def cluster_span(length):
    return (length + CLUSTER_BYTES - 1) // CLUSTER_BYTES


# One chain per file, in the order they are allocated.
next_cluster = ESP_FIRST_FILE_CLUSTER
chains = {}


def allocate(name, data):
    global next_cluster
    count = cluster_span(len(data))
    chains[name] = list(range(next_cluster, next_cluster + count))
    next_cluster += count


allocate("shim", shim_bytes)
allocate("grub", grub_bytes)
allocate("grub_cfg", grub_cfg_bytes)
allocate("fallback", fallback_bytes)
ESP_EOC = 0x0FFFFFFF

fat = bytearray(ESP_SECTORS_PER_FAT * SECTOR)
fat[0:4] = struct.pack("<I", 0x0FFFFFF8)
fat[4:8] = struct.pack("<I", 0x0FFFFFFF)
for cluster in (ESP_ROOT, ESP_EFI, ESP_DEBIAN, ESP_BOOT):
    struct.pack_into("<I", fat, cluster * 4, ESP_EOC)
for chain in chains.values():
    for position, cluster in enumerate(chain):
        following = chain[position + 1] if position + 1 < len(chain) else ESP_EOC
        struct.pack_into("<I", fat, cluster * 4, following)


def fat_name(name, extension):
    out = bytearray(b" " * 11)
    for index, character in enumerate(name[:8]):
        out[index] = ord(character)
    for index, character in enumerate(extension[:3]):
        out[8 + index] = ord(character)
    return bytes(out)


def dir_entry(name, extension, attributes, first_cluster, size, case_flags=0):
    out = bytearray(32)
    out[0:11] = fat_name(name, extension)
    out[11] = attributes
    out[12] = case_flags
    struct.pack_into("<H", out, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", out, 26, first_cluster & 0xFFFF)
    struct.pack_into("<I", out, 28, size)
    return bytes(out)


ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
LOWER_BASE = 0x08
LOWER_EXTENSION = 0x10

root_dir = bytearray(CLUSTER_BYTES)
root_dir[0:32] = dir_entry("EFI", "", ATTR_DIRECTORY, ESP_EFI, 0, LOWER_BASE)
efi_dir = bytearray(CLUSTER_BYTES)
efi_dir[0:32] = dir_entry("DEBIAN", "", ATTR_DIRECTORY, ESP_DEBIAN, 0,
                          LOWER_BASE)
efi_dir[32:64] = dir_entry("BOOT", "", ATTR_DIRECTORY, ESP_BOOT, 0)
debian_dir = bytearray(CLUSTER_BYTES)
debian_dir[0:32] = dir_entry("GRUBX64", "EFI", ATTR_ARCHIVE,
                             chains["grub"][0], GRUB_LENGTH,
                             LOWER_BASE | LOWER_EXTENSION)
debian_dir[32:64] = dir_entry("SHIMX64", "EFI", ATTR_ARCHIVE,
                              chains["shim"][0], SHIM_LENGTH,
                              LOWER_BASE | LOWER_EXTENSION)
debian_dir[64:96] = dir_entry("GRUB", "CFG", ATTR_ARCHIVE,
                              chains["grub_cfg"][0], GRUB_CFG_LENGTH,
                              LOWER_BASE | LOWER_EXTENSION)
boot_dir = bytearray(CLUSTER_BYTES)
boot_dir[0:32] = dir_entry("BOOTX64", "EFI", ATTR_ARCHIVE,
                           chains["fallback"][0], FALLBACK_LENGTH)


def cluster_sectors(cluster):
    return [esp_first + ESP_DATA_START +
            (cluster - 2) * ESP_SECTORS_PER_CLUSTER + offset
            for offset in range(ESP_SECTORS_PER_CLUSTER)]


def chain_sectors(chain, length):
    sectors = []
    for cluster in chain:
        sectors.extend(cluster_sectors(cluster))
    return sectors[:((length + SECTOR - 1) // SECTOR)]


def chain_bytes(data):
    return b"".join(data[index:index + SECTOR].ljust(SECTOR, b"\x00")
                    for index in range(0, len(data), SECTOR))


with open(disk_path, "r+b") as handle:
    def put(lba, data):
        handle.seek(lba * SECTOR)
        handle.write(data)

    def put_cluster(cluster, data):
        padded = bytes(data).ljust(CLUSTER_BYTES, b"\x00")
        put(esp_first + ESP_DATA_START +
            (cluster - 2) * ESP_SECTORS_PER_CLUSTER, padded)

    def put_chain(name, data):
        for position, cluster in enumerate(chains[name]):
            chunk = data[position * CLUSTER_BYTES:
                         (position + 1) * CLUSTER_BYTES]
            put_cluster(cluster, chunk)

    handle.seek(0)
    handle.write(mbr)
    handle.write(header(1, media_lba - 1, 2, array, 0x50))
    handle.write(array)
    handle.seek((media_lba - 1 - ARRAY_SECTORS) * SECTOR)
    handle.write(array)
    handle.seek((media_lba - 1) * SECTOR)
    handle.write(header(media_lba - 1, 1, media_lba - 1 - ARRAY_SECTORS,
                        array, 0x60))
    put(esp_first, bytes(boot_sector))
    for index in range(ESP_SECTORS_PER_FAT):
        put(esp_first + ESP_RESERVED + index,
            bytes(fat[index * SECTOR:(index + 1) * SECTOR]))
        put(esp_first + ESP_RESERVED + ESP_SECTORS_PER_FAT + index,
            bytes(fat[index * SECTOR:(index + 1) * SECTOR]))
    put_cluster(ESP_ROOT, root_dir)
    put_cluster(ESP_EFI, efi_dir)
    put_cluster(ESP_DEBIAN, debian_dir)
    put_cluster(ESP_BOOT, boot_dir)
    put_chain("shim", shim_bytes)
    put_chain("grub", grub_bytes)
    put_chain("grub_cfg", grub_cfg_bytes)
    put_chain("fallback", fallback_bytes)

state = {
    "guid": guid_text(ESP_UNIQUE),
    "esp_first": esp_first,
    "esp_length": esp_length,
    "entry_bytes": [array[index * ENTRY_SIZE:(index + 1) * ENTRY_SIZE].hex()
                    for index in range(2)],
    "mbr": bytes(mbr).hex(),
    "files": {
        name: chain_sectors(chains[name], len(data))
        for name, data in (("shim", shim_bytes), ("grub", grub_bytes),
                           ("grub_cfg", grub_cfg_bytes),
                           ("fallback", fallback_bytes))
    },
    "file_bytes": {
        name: chain_bytes(data).hex()
        for name, data in (("shim", shim_bytes), ("grub", grub_bytes),
                           ("grub_cfg", grub_cfg_bytes),
                           ("fallback", fallback_bytes))
    },
}
pathlib.Path(state_path).write_text(json.dumps(state))
print("other-disk: Debian-like GPT and FAT32 ESP prepared, esp guid "
      + state["guid"] + ", clusters " + str(ESP_FIRST_FILE_CLUSTER) + ".." +
      str(next_cluster - 1))
PY
}

# assert_other_disk_untouched DISK STATE_FINGERPRINT_BEFORE
#
# The disk the machine's other system lives on is read and only read: every
# foreign GPT entry, the protective MBR and every foreign ESP file is byte for
# byte what it was, and the image's own fingerprint (size, allocated blocks and
# the first 4 MiB, which carry the table and the volume metadata) is unchanged.
assert_other_disk_untouched() {
    local disk=$1
    local state=$2
    local before=$3
    local after

    after=$(disk_fingerprint "$disk")
    [[ "$before" == "$after" ]] || {
        echo "the other disk changed: $before -> $after" >&2
        return 1
    }
    python3 - "$disk" "$state" <<'PY'
import hashlib
import json
import pathlib
import sys

disk_path, state_path = sys.argv[1], sys.argv[2]
state = json.loads(pathlib.Path(state_path).read_text())
with open(disk_path, "rb") as handle:
    disk = handle.read()
SECTOR = 512


def require(condition, message):
    if not condition:
        raise SystemExit("other-disk: " + message)


entry_lba = int.from_bytes(disk[SECTOR + 72:SECTOR + 80], "little")
entry_count = int.from_bytes(disk[SECTOR + 80:SECTOR + 84], "little")
entry_size = int.from_bytes(disk[SECTOR + 84:SECTOR + 88], "little")
require(disk[0:SECTOR].hex() == state["mbr"],
        "the protective MBR is not byte-identical")
for index, expected in enumerate(state["entry_bytes"]):
    start = entry_lba * SECTOR + index * entry_size
    require(disk[start:start + entry_size].hex() == expected,
            f"foreign GPT entry {index} is not byte-identical")
require(entry_count >= 2 and entry_size == 128,
        "the foreign table lost its entries")
for name, sectors in state["files"].items():
    data = b"".join(disk[lba * SECTOR:(lba + 1) * SECTOR] for lba in sectors)
    require(hashlib.sha256(data).hexdigest() ==
            hashlib.sha256(bytes.fromhex(state["file_bytes"][name]))
            .hexdigest(),
            f"the foreign ESP file {name} is not byte-identical")
print("other-disk: MBR, both foreign GPT entries and all four foreign ESP "
      "files are byte-identical after the install")
PY
}

OTHER_STATE=$TEST_DIR/other-disk.json
build_other_system_disk "$TEST_DIR/other.raw" "$OTHER_STATE"
OTHER_BEFORE=$(disk_fingerprint "$TEST_DIR/other.raw")
OTHER_GUID=$(python3 -c "import json,sys;print(json.load(open('$OTHER_STATE'))['guid'])")

RUN_BOOT_EXTRA=(
    -drive "if=none,id=other,file=$TEST_DIR/other.raw,format=raw,cache=writeback,file.locking=off"
    -device "ide-hd,drive=other,bus=ide.1,serial=HFOC-OTHER-DISK"
)
# Three entries: the appliance, the system on the other disk (through its
# shim), and the removable-media fallback loader the same ESP carries under
# `\EFI\BOOT` - which no discovered system claims, so it keeps its own entry
# rather than disappearing.
RUN_BOOT_MENU_EXPECT='state=GENERATED reason=chainload-entry stage=chainload-entry|entries=3|loaders=2|missing=0|unreadable=0|disks=2|target=1|d1=NONE/esp0/NO_ESP/0|d2=GPT/esp1/OK/2'
RUN_BOOT_ENTRY_EXPECT='menu=\EFI\BOOT\limine.conf entries=3 timeout=10s source=GENERATED payload_set=7'
# ITEM-411: a completed install reboots the machine, so this run must reset.
run_boot "other-disk" uefi "$AUTHORISED_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset
RUN_BOOT_EXTRA=()
RUN_BOOT_MENU_EXPECT='state=GENERATED reason=appliance-only stage=no-other-disk|entries=1|loaders=0|disks=1|target=1|d1=NONE/esp0/NO_ESP/0'
RUN_BOOT_ENTRY_EXPECT='menu=\EFI\BOOT\limine.conf entries=1 timeout=10s source=GENERATED payload_set=7'


python3 - "$TEST_DIR/other-disk.log" <<'PY'
import pathlib
import sys

log_path = sys.argv[1]
text = pathlib.Path(log_path).read_bytes().decode("ascii", errors="replace")


def require(condition, message):
    if not condition:
        raise SystemExit("other-disk: " + message)


require("[PRODUCTION-INSTALL] layout mode=whole-disk "
        "reason=no-existing-system declared=0" in text,
        "the run did not take the whole-disk path over an empty target disk")
require("[PRODUCTION-INSTALL] candidate disk=2 " in text,
        "the machine's other disk is not published as a candidate")
menu = [line for line in text.splitlines()
        if line.startswith("[PRODUCTION-INSTALL] boot-menu ")][0]
record = [line for line in text.splitlines()
          if line.startswith("[PRODUCTION-INSTALL] boot-entry ")][0]
require("existing_loader=FREE" in record,
        f"the target ESP was not free: {record}")
print("other-disk: the flow published the machine's other disk as "
      + [fragment for fragment in menu.split() if fragment.startswith("d2=")][0])
import re as _re

_stated = _re.search(r"menu=\\EFI\\BOOT\\limine.conf entries=(\d+) "
                     r"timeout=(\d+)s source=(\w+)", record)
require(_stated is not None,
        f"the boot-entry record does not state the configuration: {record}")
print("other-disk: the flow published the boot-entry record with "
      f"menu=\\EFI\\BOOT\\limine.conf entries={_stated.group(1)} "
      f"timeout={_stated.group(2)}s source={_stated.group(3)}")
PY

assert_other_disk_untouched "$TEST_DIR/other.raw" "$OTHER_STATE" "$OTHER_BEFORE"
assert_installed_configuration "$TEST_DIR/other-disk.raw" \
    "$AUTHORISED_ISO_ROOT/boot/hfoc-installed.conf" local-only \
    "guid($OTHER_GUID):/EFI/DEBIAN/SHIMX64.EFI"
boot_installed_disk "$TEST_DIR/other-disk.raw" other-disk \
    "$AUTHORISED_ISO_ROOT" local-only LOCAL_ONLY

# ------------------------------------------------- re-installing (ITEM-413) ---
# The acceptance host was installed onto twice: the first run left our own ESP
# on the disk, and the next medium installed over it.  The format reset the
# boot sector, both FATs and the root directory - but not the volume's *data
# area*, which still held the previous install's directory clusters.  A
# directory this writer created kept those bytes (`create_directory` cleared
# its cluster, the nested-directory path did not), so the writer's own lookup
# found a stale entry, took the cluster that entry named, and left it
# unallocated in the freshly formatted FAT: the file allocator then handed that
# very cluster out for file data, and the staged boot set no longer matched the
# sealed digest the media publishes.  The flow refused with
# `PRODUCTION_INSTALLER_VERIFICATION_FAILED` at `stage=esp-payload`, so nothing
# rebooted, no boot log was ever created, and the machine stayed on the stick.
#
# This case installs the declared whole-disk medium twice onto the same disk -
# the second run writes over the ESP the first run produced, which is exactly
# what the acceptance host did - and then reads the volume, the staged set and
# the boot log back out of the finished image.
#
# The two runs publish different menu stages, and both are honest: the first
# install finds a disk with nothing else on it, the second finds the layout the
# first one left and says so (`stage=target-erased`, and the target's own ESP is
# the one it classifies: `d1=GPT/esp1/NO_DEVICE/0`).
run_boot "reinstall" uefi "$WHOLE_DISK_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" \
    "[PRODUCTION-INSTALL] layout mode=whole-disk reason=declared-whole-disk declared=1" \
    reset
RUN_BOOT_MENU_EXPECT='state=GENERATED reason=appliance-only stage=target-erased|entries=1|loaders=0|disks=1|target=1|d1=GPT/esp1/NO_DEVICE/0'
run_boot "reinstall" uefi "$WHOLE_DISK_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" \
    "[PRODUCTION-INSTALL] layout mode=whole-disk reason=declared-whole-disk declared=1" \
    reset
RUN_BOOT_MENU_EXPECT='state=GENERATED reason=appliance-only stage=no-other-disk|entries=1|loaders=0|disks=1|target=1|d1=NONE/esp0/NO_ESP/0'
assert_installed_configuration "$TEST_DIR/reinstall.raw" \
    "$WHOLE_DISK_ISO_ROOT/boot/hfoc-installed.conf" local-only
boot_installed_disk "$TEST_DIR/reinstall.raw" reinstall \
    "$WHOLE_DISK_ISO_ROOT" local-only LOCAL_ONLY
# ITEM-497: two more boots of the same installed disk, then the acceptance the
# item exists for - three boots, three input verdicts, all of them readable from
# the log after the third one.  Each boot's own `assert_boot_log` already
# requires that boot's verdict on the disk it came up on.
boot_installed_disk "$TEST_DIR/reinstall.raw" verdict-2 \
    "$WHOLE_DISK_ISO_ROOT" local-only LOCAL_ONLY
boot_installed_disk "$TEST_DIR/reinstall.raw" verdict-3 \
    "$WHOLE_DISK_ISO_ROOT" local-only LOCAL_ONLY
assert_three_boot_verdicts "$TEST_DIR/reinstall.raw" reinstall

run_boot "unauthorised" uefi "$PLAIN_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "refused reason=TOKEN_MISSING" "" no-reset
run_boot "target-missing" uefi "$AUTHORISED_IMAGE" "HFOC-NOT-PRESENT" \
    "$ACCEPTANCE_BYTES" "refused reason=TARGET_NOT_FOUND" "" no-reset
run_boot "disk-too-small" uefi "$AUTHORISED_IMAGE" "$AUTHORISED_TARGET" \
    "$SMALL_BYTES" "refused reason=CONTROLLER status=PRODUCTION_INSTALLER_MEDIA_TOO_SMALL" \
    "" no-reset

# ------------------------------------------------- the legacy BIOS (ITEM-517) --
# The machine without UEFI: SeaBIOS (QEMU's default, no OVMF pflash drives in
# the `bios` branch of `run_boot()`), the authorised medium, and the same
# declared target disk class.  The flow installs the BIOS layout - a
# `Hah!IdontNeedEFI` BIOS boot partition, a FAT boot volume and the three HFOC
# roles - writes the measured Limine stage1/stage2 recipe, and reboots.  The
# run's records and the raw image are asserted by the BIOS branch of the
# verifier inside `run_boot()`, the staged boot volume is read back here by
# `hfoc-esp-read.py`, and the installed disk is then booted on its own, still
# without UEFI, and must attest its chain and reach readiness.
run_boot "bios" bios "$AUTHORISED_IMAGE" "$AUTHORISED_TARGET" \
    "$ACCEPTANCE_BYTES" "[PRODUCTION-INSTALL] applied" "" reset
assert_bios_boot_volume "$TEST_DIR/bios.raw" bios "$AUTHORISED_ISO_ROOT"
boot_installed_bios_disk "$TEST_DIR/bios.raw" bios LOCAL_ONLY

echo "QEMU production install flow test: OK"
