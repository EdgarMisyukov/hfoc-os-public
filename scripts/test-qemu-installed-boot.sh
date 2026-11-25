#!/usr/bin/env bash
# ITEM-397: prove that the installed disk boots.
#
# The ITEM-394 install flow published an honest refusal instead of a bootable
# disk:
#
#   [PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE \
#       reason=PRODUCTION_INSTALLER_EFI_UNAVAILABLE
#
# because the running installer kernel has no EFI runtime and the kernel and
# its boot configuration were never staged.  This proof closes the loop:
#
#   1. it builds an installed disk with the production install entry points on
#      a file-backed block device - contracted layout, persistent state volume
#      and the ITEM-397 installed boot payload set;
#   2. it re-verifies the staged payload set on the finished image against the
#      sealed media source digest and the configuration's kernel reference;
#   3. it boots QEMU from that image alone (no installer media) under OVMF and
#      requires the ordinary appliance readiness records with persistent state,
#      the management publication and no panic;
#   4. it proves the refusal: the same install with an ESP that cannot hold the
#      set is refused with PRODUCTION_INSTALLER_ESP_TOO_SMALL before the first
#      write.
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

if [[ "${HFOC_INSTALLED_BOOT_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALLED_BOOT_TEST_TIMEOUT:-300}s" \
        env HFOC_INSTALLED_BOOT_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
KERNEL=${KERNEL:-build/hfoc-kernel.elf}
ISO_ROOT=${ISO_ROOT:-build/iso_root}
BUILDER=${BUILDER:-build/test-installed-boot-disk-host}
MEDIA_LBA=${MEDIA_LBA:-400000}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Installed-disk boot test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Installed-disk boot test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Installed-disk boot test must not set QEMU_CPU" >&2
    exit 1
}
for required in "$KERNEL" "$BUILDER" \
    "$ISO_ROOT/boot/limine/limine.conf" "$ISO_ROOT/boot/hfoc-boot.man" \
    "$ISO_ROOT/boot/hfoc-rootfs.arc" \
    deps/limine/BOOTX64.EFI "$OVMF_CODE" "$OVMF_VARS"; do
    test -f "$required" || {
        echo "Missing installed-disk boot input: $required" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/installed.img
REFUSAL_DISK=$TEST_DIR/refusal.img
SERIAL_LOG=$TEST_DIR/com1.log
QEMU_LOG=$TEST_DIR/qemu.log
VARS=$TEST_DIR/vars.fd
QEMU_PID=
FAILURE_MESSAGE=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/installed-boot-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-Installed-disk boot proof failed}" \
            >"$artifact_dir/failure-reason.txt"
        cp -a "$SERIAL_LOG" "$QEMU_LOG" "$TEST_DIR"/*.build.log \
            "$artifact_dir/" 2>/dev/null || true
        echo "Installed-disk boot artifacts: $artifact_dir" >&2
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
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

# ---------------------------------------------------------------- install ----
# The image is written by the production install code (contracted layout, state
# volume, installed boot payload set) against a file-backed block device.
BUILD_OUTPUT=$TEST_DIR/build.log
if ! "$BUILDER" build \
    --disk "$DISK" --media-lba "$MEDIA_LBA" \
    --identity installed-boot-disk \
    --kernel "$KERNEL" \
    --config "$ISO_ROOT/boot/limine/limine.conf" \
    --manifest "$ISO_ROOT/boot/hfoc-boot.man" \
    --loader deps/limine/BOOTX64.EFI \
    --payload deps/limine/BOOTX64.EFI \
    --rootfs "$ISO_ROOT/boot/hfoc-rootfs.arc" >"$BUILD_OUTPUT" 2>&1; then
    cat "$BUILD_OUTPUT" >&2
    FAILURE_MESSAGE="the production install did not produce a bootable disk"
    exit 1
fi
cat "$BUILD_OUTPUT"
for marker in \
    '[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH' \
    'verify=SEALED_MEDIA' \
    '[INSTALLED-DISK] build '; do
    if ! grep -Fq "$marker" "$BUILD_OUTPUT"; then
        FAILURE_MESSAGE="the install record is missing: $marker"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
done
if ! grep -Fq 'result=OK' "$BUILD_OUTPUT"; then
    FAILURE_MESSAGE="the install record does not report success"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

# The staged payload set is re-verified from the finished image: every file is
# compared byte for byte with the declared component and the SHA-256 over the
# sealed prefix is compared with the sealed media source digest.
VERIFY_OUTPUT=$TEST_DIR/verify.log
if ! "$BUILDER" verify \
    --disk "$DISK" --media-lba "$MEDIA_LBA" \
    --kernel "$KERNEL" \
    --config "$ISO_ROOT/boot/limine/limine.conf" \
    --manifest "$ISO_ROOT/boot/hfoc-boot.man" \
    --loader deps/limine/BOOTX64.EFI \
    --payload deps/limine/BOOTX64.EFI \
    --rootfs "$ISO_ROOT/boot/hfoc-rootfs.arc" >"$VERIFY_OUTPUT" 2>&1; then
    cat "$VERIFY_OUTPUT" >&2
    FAILURE_MESSAGE="the staged payload set does not verify on the image"
    exit 1
fi
cat "$VERIFY_OUTPUT"
grep -Fq 'digest_match=1' "$VERIFY_OUTPUT" || {
    FAILURE_MESSAGE="the staged set does not match the sealed media digest"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
grep -Fq 'result=OK' "$VERIFY_OUTPUT" || {
    FAILURE_MESSAGE="the staged set verification did not succeed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}

# ----------------------------------------------------------------- refusal ---
# An ESP that cannot hold the payload set is refused before the first write.
REFUSAL_OUTPUT=$TEST_DIR/refuse.log
if ! "$BUILDER" refuse \
    --disk "$REFUSAL_DISK" --media-lba "$MEDIA_LBA" --esp-lba 1024 \
    --identity installed-boot-disk \
    --kernel "$KERNEL" \
    --config "$ISO_ROOT/boot/limine/limine.conf" \
    --manifest "$ISO_ROOT/boot/hfoc-boot.man" \
    --loader deps/limine/BOOTX64.EFI \
    --payload deps/limine/BOOTX64.EFI \
    --rootfs "$ISO_ROOT/boot/hfoc-rootfs.arc" >"$REFUSAL_OUTPUT" 2>&1; then
    cat "$REFUSAL_OUTPUT" >&2
    FAILURE_MESSAGE="the small-ESP refusal did not succeed"
    exit 1
fi
cat "$REFUSAL_OUTPUT"
grep -Fq \
    '[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE reason=PRODUCTION_INSTALLER_ESP_TOO_SMALL' \
    "$REFUSAL_OUTPUT" || {
    FAILURE_MESSAGE="the refusal record does not name the ESP-too-small reason"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
grep -Fq 'writes=0 result=OK' "$REFUSAL_OUTPUT" || {
    FAILURE_MESSAGE="the refusal wrote to the disk"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}

# -------------------------------------------------------------------- boot ---
# Boot the installed disk alone: no installer media, no ISO, and the ESP has no
# platform boot entry, so the firmware has to find \EFI\BOOT\BOOTX64.EFI.
cp "$OVMF_VARS" "$VARS"
: >"$SERIAL_LOG"
: >"$QEMU_LOG"

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none \
    -vga none -device 'VGA,edid=on,xres=1920,yres=1080' \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$VARS" \
    -drive "if=none,id=hfoc-disk,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc-disk,bus=ide.0' \
    -netdev user,id=hfoc-net \
    -device virtio-net-pci-non-transitional,netdev=hfoc-net,ioeventfd=off,\
event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,\
ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,\
ctrl_guest_offloads=off,gso=off \
    -serial "file:$SERIAL_LOG" -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import signal
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
deadline = time.monotonic() + 240.0

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


readiness = re.compile(
    r"\[APPLIANCE-BOOT\] runtime=READY identity=OK config=[A-Z_]+ "
    r"local_management=READY storage=READY local_input=(?:AVAILABLE|UNAVAILABLE)"
)

# Records that must appear exactly once: the boot the installer staged, the
# attestation over the staged payload set, the storage verdict and the
# appliance readiness record.
exactly_once = [
    r"\[BOOT\] Bootloader: Limine ",
    r"\[BOOT-CHAIN\] verdict=ATTESTED",
    r"\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK "
    r"discovered=1 valid=1 selected=A[HC]CI storage=READY",
    readiness.pattern,
]

# Records the ordinary boot publishes: the acceptance stages quote them.
required = [
    r"\[PMM \] status=OK",
    # ITEM-622/623: the ordinary boot creates the bounded writable RAM
    # namespace and mounts the installed initramfs as `/`, so the installed
    # disk publishes a populated read-only root and a non-empty data count.
    r"\[VFS\] state=READY root=/ nodes=(?P<nodes>\d+) open=0 "
    r"data_bytes=(?P<data_bytes>\d+) data_capacity=393216 mounts=3 "
    r"initramfs=READY",
    r"\[KBD \] state=",
    r"\[PRODUCTION-MGMT-BIND\] scope=",
    r"\[PRODUCTION-SERVING\] v=1 state=",
    r"\[PRODUCTION-SERVICE\] production_service_activation v=1 state=",
    r"\[UPDATE-ATOMICITY\] update_lifecycle v=1 state=IDLE",
    r"\[AUDIT-INTEGRITY\] audit_chain v=1 state=READY",
    # ITEM-410: the installed disk writes its own boot log and says so.  The log
    # itself is read back out of the raw image below, after QEMU has stopped -
    # and the completion record is published *after* the device flush, so
    # waiting for it is what makes that read-back deterministic.
    r"\[BOOTLOG\] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN",
    r"\[BOOTLOG\] state=READY .*reason=readiness .*flushed=[1-9]",
    # ITEM-466 (round 5): the installed system's own display record and the loop
    # budget are part of the proof.  The entry asks for no mode of its own: the
    # firmware's preferred mode is what boots, so with the attached 1920x1080
    # EDID the record must name that preference, the actual mode must be it (no
    # fallback), and the automatic font scale must be 2 on it.
    r"\[FB\] width=\d+ height=\d+ pitch=\d+ cells=\d+x\d+ bpp=32 "
    r"modes=\d+ chosen=\d+x\d+ requested=\d+x\d+ actual=\d+x\d+ "
    r"fallback=\d+ scale=\d+ cell_px=\d+x\d+ preferred=\d+x\d+ "
    r"available_count=\d+ available=\S+",
    r"\[PRODUCTION-LOOP\] .*iter_max_us=\d+",
]

all_patterns = exactly_once + required

while time.monotonic() < deadline:
    text = serial_text()
    if all(re.search(pattern, text) for pattern in all_patterns):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit(
            "kernel panic while booting the installed disk:\n" +
            text[-4000:])
    time.sleep(0.1)
else:
    missing = [pattern for pattern in all_patterns
               if not re.search(pattern, serial_text())]
    raise SystemExit(
        "timed out waiting for the installed-disk boot records; missing: " +
        "; ".join(missing) + "\n" + serial_text()[-4000:])

text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("kernel panic detected")

for pattern in exactly_once:
    count = len(re.findall(pattern, text))
    if count != 1:
        raise SystemExit(
            f"expected exactly one record matching {pattern!r}, got {count}")
for pattern in required:
    if not re.search(pattern, text):
        raise SystemExit(f"missing record matching {pattern!r}")

# ITEM-623: the installed root is the populated initramfs, not the empty RAM
# fallback.  Both the node count and the data count have to say so.
vfs = re.search(
    r"\[VFS\] state=READY root=/ nodes=(\d+) open=0 "
    r"data_bytes=(\d+) data_capacity=393216 mounts=3 initramfs=READY",
    text)
if vfs is None:
    raise SystemExit("the installed boot published no populated [VFS] record")
if int(vfs.group(1)) < 5:
    raise SystemExit(
        f"the installed initramfs has only {vfs.group(1)} nodes")
if int(vfs.group(2)) == 0:
    raise SystemExit("the installed initramfs imported no data bytes")

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
width, height = int(geometry.group(1)), int(geometry.group(2))
cells = (int(geometry.group(4)), int(geometry.group(5)))
mode_count = int(geometry.group(7))
chosen = (int(geometry.group(8)), int(geometry.group(9)))
requested = (int(geometry.group(10)), int(geometry.group(11)))
actual = (int(geometry.group(12)), int(geometry.group(13)))
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
if (width, height) != actual:
    raise SystemExit(
        f"the [FB] actual {actual} is not the initialised {width}x{height}")
if actual != (1920, 1080):
    raise SystemExit(
        f"the installed system booted {actual}, not the native 1920x1080")
if fallback != 0:
    raise SystemExit(
        f"the native mode was not honoured: fallback={fallback}")
if mode_count == 0 or available_count == 0:
    raise SystemExit("the installed firmware mode list was empty")
if chosen != actual:
    raise SystemExit(
        f"the selection rule chose {chosen}, not the actual {actual}")
if scale != 2:
    raise SystemExit(f"the automatic font scale is {scale}, not 2 at 1920x1080")
if cell_px != (8 * scale, 16 * scale):
    raise SystemExit(
        f"the cell {cell_px} does not follow scale={scale}")
if cells != (width // cell_px[0], height // cell_px[1]):
    raise SystemExit(
        f"the grid {cells} does not follow cell_px={cell_px}")

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

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before the proof completed")

serving = re.search(r"\[PRODUCTION-SERVING\] [^\n]*", text).group(0)
print("Installed-disk boot records: exact")
print("Installed-disk readiness with persistent state: READY")
print(f"Installed display: actual={actual[0]}x{actual[1]} "
      f"preferred={preferred[0]}x{preferred[1]} modes={mode_count} "
      f"chosen={chosen[0]}x{chosen[1]} fallback={fallback} scale={scale}")
print(f"Installed loop worst: {iter_worst}us budget={ITER_US_BUDGET}us")
print("Management publication: " + serving)
print("Kernel panic: none")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
grep -E '\[PRODUCTION-BOOT\]|\[APPLIANCE-BOOT\]|\[BOOT-CHAIN\]|\[PRODUCTION-SERVING\]' \
    "$(serial_view "$SERIAL_LOG")" | head -n 8

# ------------------------------------------------------------- the boot log ---
# ITEM-410: the disk that just booted carries the appliance's own boot log, and
# the log is read out of the finished image by the host reader - the same bytes
# a human reads after a hardware run with `scripts/hfoc-esp-read.py --bootlog`,
# or with a plain mount.  Nothing here comes from the kernel or from the serial
# stream: this is the evidence the FIRST_BOOT stage quotes instead of a
# photograph of the screen.
BOOT_LOG=$TEST_DIR/bootlog.txt
BOOT_LOG_ERRORS=$TEST_DIR/bootlog.stderr
if ! python3 scripts/hfoc-esp-read.py "$DISK" --bootlog \
    >"$BOOT_LOG" 2>"$BOOT_LOG_ERRORS"; then
    FAILURE_MESSAGE="cannot read the appliance boot log from $DISK"
    cat "$BOOT_LOG_ERRORS" >&2
    exit 1
fi
cat "$BOOT_LOG_ERRORS" >&2
head -n 1 "$BOOT_LOG"
for marker in \
    '[BOOT-CHAIN] verdict=ATTESTED' \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK' \
    '[APPLIANCE-BOOT] runtime=READY identity=OK' \
    '[PRODUCTION-SERVING] v=1 state=' \
    '[FB] width=1920 height=1080' \
    '[BOOTLOG] state=READY path=\EFI\HFOC\BOOTLOG.BIN'; do
    if ! grep -Fq "$marker" "$BOOT_LOG"; then
        FAILURE_MESSAGE="the appliance boot log does not carry: $marker"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
done
BOOT_LOG_RECORDS=$(grep -c '^#' "$BOOT_LOG")
if (( BOOT_LOG_RECORDS < 8 )); then
    FAILURE_MESSAGE="the appliance boot log carries only $BOOT_LOG_RECORDS records"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "Appliance boot log on the installed disk: $BOOT_LOG_RECORDS records"
tail -n 3 "$BOOT_LOG"
echo "QEMU installed-disk boot test: OK"
