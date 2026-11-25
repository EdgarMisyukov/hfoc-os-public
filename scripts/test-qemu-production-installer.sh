#!/usr/bin/env bash
# Macro-gated QEMU proof for the ITEM-351 production installer controller.
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

if [[ "${HFOC_PRODUCTION_INSTALLER_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_INSTALLER_TEST_TIMEOUT:-60}s" \
        env HFOC_PRODUCTION_INSTALLER_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production-installer test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production-installer test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Production-installer test must not set QEMU_CPU" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$QEMU_LOG"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        mkdir -p build/test-artifacts
        artifact_dir="build/test-artifacts/production-installer-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-Production-installer QEMU proof failed}" \
            >"$FAILURE_REASON"
        cp -a "$SERIAL_LOG" "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/"
        echo "Production-installer artifacts: $artifact_dir" >&2
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

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
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
deadline = time.monotonic() + 45.0


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


while time.monotonic() < deadline:
    text = serial_text()
    if "[PRODUCTION-INSTALLER] proof OK" in text:
        break
    if "[PRODUCTION-INSTALLER] failed" in text:
        raise SystemExit("Production-installer proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before production-installer proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="production-installer proof markers" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for production-installer proof markers")

required = [
    "[PRODUCTION-INSTALLER] proof init",
    "[PRODUCTION-INSTALLER] unconfirmed no-write proof OK",
    "[PRODUCTION-INSTALLER] unverified media no-write proof OK",
    "[PRODUCTION-INSTALLER] unsupported hardware no-write proof OK",
    "[PRODUCTION-INSTALLER] clean GPT layout proof OK",
    "[PRODUCTION-INSTALLER] state erase proof OK",
    "[PRODUCTION-INSTALLER] persistent state proof OK",
    "[PRODUCTION-INSTALLER] state erase reinstall proof OK",
    "[PRODUCTION-INSTALLER] ESP payload proof OK",
    "[PRODUCTION-INSTALLER] UEFI boot entry proof OK",
    "[PRODUCTION-INSTALLER] unrelated boot entries preserved",
    "[PRODUCTION-INSTALLER] BootOrder preservation proof OK",
    "[PRODUCTION-INSTALLER] complete install verification proof OK",
    "[PRODUCTION-INSTALLER] layout contract proof OK",
    "[PRODUCTION-INSTALLER] foreign disk prepared",
    "[PRODUCTION-INSTALLER] shared ESP inventory proof OK",
    "[PRODUCTION-INSTALLER] preserve preview proof OK",
    "[PRODUCTION-INSTALLER] preserved table proof OK",
    "[PRODUCTION-INSTALLER] dual-boot refusal proof OK",
    "[PRODUCTION-INSTALLER] shared ESP reuse proof OK",
    "[PRODUCTION-INSTALLER] boot menu proof OK",
    "[PRODUCTION-INSTALLER] cross-disk menu proof OK",
    "[PRODUCTION-INSTALLER] unreadable second disk proof OK",
    "[PRODUCTION-INSTALLER] uninstall dry-run preview proof OK",
    "[PRODUCTION-INSTALLER] uninstall previous-OS restoration proof OK",
    "[PRODUCTION-INSTALLER] injected failure proof OK",
    "[PRODUCTION-INSTALLER] rollback restoration proof OK",
    "[PRODUCTION-INSTALLER] clean shutdown",
    "[PRODUCTION-INSTALLER] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

# ITEM-391: the installed footprint is published as the [INSTALLER-LAYOUT]
# record, and the numbers here are the contract.  The static check suite pins
# the same values against the header-derived constants, so a layout change has
# to move this proof, the header and the contract together.
plan_lines = [line for line in text.splitlines()
              if line.startswith("[INSTALLER-LAYOUT] plan ")]
role_lines = [line for line in text.splitlines()
              if line.startswith("[INSTALLER-LAYOUT] role=")]
if len(plan_lines) != 1:
    raise SystemExit(
        f"Expected 1 layout plan record, got {len(plan_lines)}")
if len(role_lines) != 4:
    raise SystemExit(
        f"Expected 4 layout role records, got {len(role_lines)}")
plan = plan_lines[0]
for fragment in (
        "mode=clean-whole-disk",
        "disk=1",
        "media_lba=234375000",
        "media_mib=114440",
        "minimum_media_lba=331810",
        "minimum_media_mib=162",
        "declared_lba=329728",
        "declared_mib=161",
        "first_lba=2048",
        "footprint_lba=329728",
        "footprint_mib=161",
        "free_first_lba=331776",
        "free_lba=234043190",
        "free_mib=114278",
        "existing_partitions=0",
        "esp_reused=0",
):
    if fragment not in plan:
        raise SystemExit(
            f"Layout plan record is missing {fragment}: {plan}")
for role_line in (
        "[INSTALLER-LAYOUT] role=efi-system first_lba=2048 length_lba=67584 length_mib=33 reused=0",
        "[INSTALLER-LAYOUT] role=hfoc-system first_lba=69632 length_lba=131072 length_mib=64 reused=0",
        "[INSTALLER-LAYOUT] role=hfoc-persistence first_lba=200704 length_lba=65536 length_mib=32 reused=0",
        "[INSTALLER-LAYOUT] role=hfoc-recovery first_lba=266240 length_lba=65536 length_mib=32 reused=0",
):
    if text.count(role_line) != 1:
        raise SystemExit(
            f"Expected 1 layout role record, got "
            f"{text.count(role_line)}: {role_line}")

# ITEM-396: the dual-boot install on the foreign disk publishes what it keeps
# and what it adds, and the menu it writes.  The record, the menu and the disk
# are compared here; the kernel-side proof compares the bytes.  The serial line
# ends CRLF, so the analysis below runs on the LF-normalised text.
#
# Three preserving installs run now: ITEM-396 Case A (one disk, a second
# system), Case B (the second system on another disk, chainloaded through
# `guid()`) and the ITEM-398 shared-ESP pass that proves the two paths the
# installed boot set takes over.  Every one of them publishes the same plan.
text = text.replace("\r\n", "\n")
plan_lines = [line for line in text.splitlines()
              if line.startswith("[INSTALLER-PRESERVE] plan ")]
if len(plan_lines) != 3:
    raise SystemExit(
        f"Expected 3 preservation plan records, got {len(plan_lines)}")
for plan in plan_lines:
    for fragment in (
            "mode=dual-boot",
            "table=gpt",
            "entry_count=128",
            "preserved=4",
            "added=3",
            "esp_reused=1",
            "esp_index=0",
            "preserved_first_lba=2048",
            "menu_entries=",
    ):
        if fragment not in plan:
            raise SystemExit(
                f"Preservation plan record is missing {fragment}: {plan}")
if "origin=preserved" not in text:
    raise SystemExit("Preservation record lists no preserved entry")
if "origin=added role=hfoc-system" not in text:
    raise SystemExit("Preservation record lists no added role")

menu_lines = [line for line in text.splitlines()
              if line.startswith("[INSTALLER-BOOT-MENU] entry ")]
# Every install publishes its menu, and the cross-disk pass publishes a second
# record for the disk it could not read.  Four installs publish menus now:
# ITEM-396 Case A (three entries), ITEM-396 Case B (four), the ITEM-398
# shared-ESP pass (two) and the ITEM-398 whole-disk cross-disk pass (two).  The
# per-menu fragments below are the contract; this keeps a menu that lost its
# entries from passing.
if len(menu_lines) < 11:
    raise SystemExit(
        f"Expected at least 11 published boot menu entries, got "
        f"{len(menu_lines)}")
for fragment in (
        # Case A: the appliance and Debian share one disk, so the ESP it reads
        # and writes is the one Limine booted from.  The shared ESP carries two
        # loaders (Debian's GRUB and the removable-media fallback), but only
        # Debian is offered as a system: ITEM-398 excludes the path this install
        # takes over on the target disk, because after the install it holds this
        # appliance's own loader.
        "[INSTALLER-BOOT-MENU] menu firmware=uefi entries=3 loaders=2 "
        "missing=0 unreadable_disks=0",
        # Case B: Debian's loader on the other disk is an extra entry, and the
        # other disk is reported per disk.
        "[INSTALLER-BOOT-MENU] menu firmware=uefi entries=4 loaders=3 "
        "missing=0 unreadable_disks=0",
        "protocol=limine label=HFOC OS path=boot():/boot/hfoc-kernel.elf",
        "protocol=efi label=Debian GNU/Linux "
        "path=boot():/EFI/DEBIAN/GRUBX64.EFI",
        "protocol=limine label=HFOC OS (recovery) "
        "path=boot():/boot/hfoc-recovery.elf",
):
    if fragment not in text:
        raise SystemExit(
            f"Published boot menu is missing {fragment}")

# ITEM-398: the path the install takes over on the target disk is not offered as
# a system anywhere - an entry for it would chainload the menu into itself once
# the install has written this appliance's loader there.
if "boot():/EFI/BOOT/BOOTX64.EFI" in text:
    raise SystemExit(
        "The menu offers the removable-media path the install takes over")

# The Limine configuration the install writes is published verbatim, once per
# install: the same-disk one chains through boot(), the cross-disk one through
# the other ESP's GPT GUID.
if text.count("[INSTALLER-BOOT-MENU] config-begin") < 2 or \
        text.count("[INSTALLER-BOOT-MENU] config-end") < 2:
    raise SystemExit(
        "The Limine configuration was not published for both installs")
for configuration in text.split("[INSTALLER-BOOT-MENU] config-begin")[1:]:
    configuration = configuration.split(
        "[INSTALLER-BOOT-MENU] config-end", 1)[0]
    for fragment in ("/HFOC OS\n", "/Debian GNU/Linux\n",
                     "    protocol: efi\n"):
        if fragment not in configuration:
            raise SystemExit(
                f"Published Limine configuration is missing {fragment!r}")
    if "    path: boot():/EFI/DEBIAN/GRUBX64.EFI\n" not in configuration \
            and "    path: guid(" not in configuration:
        raise SystemExit(
            "The published menu offers no chainload path for Debian")

# ITEM-396 Case B (owner refinement, 2026-09-13): Debian on the machine's other
# disk is offered through Limine's guid() resource, that disk is reported per
# disk, and it is never written (the kernel-side proof asserts the write count).
if "[INSTALLER-BOOT-MENU] disk id=2 esp=2048+4096 volume=guid(" not in text:
    raise SystemExit(
        "The other disk's ESP is not published with a guid() volume")
if "[INSTALLER-BOOT-MENU] loader disk=2 vendor=DEBIAN " \
        "path=\\EFI\\DEBIAN\\SHIMX64.EFI size=640" not in text:
    raise SystemExit("The other disk's Debian loader is not published")
if "path=guid(" not in text or ":/EFI/DEBIAN/SHIMX64.EFI" not in text:
    raise SystemExit("No cross-disk chainload entry for the Debian loader")
if "scan=CORRUPT" not in text and "scan=UNREADABLE" not in text:
    raise SystemExit(
        "The unreadable second disk was not published with its reason")

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before test completion")

print("Production-installer proof init: found")
print("Production-installer proof OK: found")
print("Kernel panic: none")
print("QEMU terminated after proof: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU production-installer test: OK"
