#!/usr/bin/env bash
# ITEM-492: the machine's network controllers, discovered and described through
# the real UEFI path, in QEMU/OVMF.
#
# The item's claim is that the product describes whatever NIC the machine has
# instead of asking one vendor's driver whether it is there.  This proof boots
# the installer medium on machines that differ only in their network hardware
# and requires the records to follow the hardware, not the driver list:
#
#   1. an Intel e1000 is discovered and described with its address and ids;
#      since ITEM-494 the image carries the 8254x driver, so the capability
#      reading marks it `state=SUPPORTED reason=DRIVER_PRESENT` - a driver in
#      the image is not a device that works, and the reading says so;
#   2. the same machine with a virtio-net function as well reports *both*
#      controllers, each claimed by its own driver, so one device does not hide
#      the other;
#   3. a machine with no network controller reports `devices=0 ... absent=1`,
#      which is a reading, not a failure;
#   4. in all three cases the install continues: `[INSTALLER-NIC] ...
#      install=CONTINUES` and the run reaches its own records.  The network is
#      not a precondition of installing;
#   5. the capability reading never reports `available=`: the installer kernel
#      carries no network stack, and a driver in the image is not a device that
#      works.  Nothing here is a fake PASS.
#
# This is a firmware proof on virtual hardware:
# QEMU/OVMF proof != physical hardware acceptance.  QEMU cannot emulate the
# acceptance host's RTL8168 (0x10EC:0x8168); that part is covered by the
# registry's matching proof on the host and by the physical acceptance run.
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

if [[ "${HFOC_ITEM492_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ITEM492_TIMEOUT:-900}s" \
        env HFOC_ITEM492_INNER=1 "$0" "$@"
fi

VERSION=$(<VERSION)
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$VERSION-production-installer-authorised.iso}
OVMF=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
QEMU_MACHINE=${QEMU_MACHINE:-q35}
DISK_BYTES=${HFOC_ITEM492_DISK_BYTES:-4294967296}

[[ -f "$IMAGE" ]] || {
    echo "Missing install medium: $IMAGE" >&2
    exit 1
}
[[ -f "$OVMF" ]] || {
    echo "Missing UEFI firmware: $OVMF" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

# run_case <label> <expectations file> [extra qemu arguments...]
run_case() {
    local label=$1
    local expectations=$2
    shift 2
    local log=$TEST_DIR/$label.log
    local pid

    truncate -s "$DISK_BYTES" "$TEST_DIR/$label.img"
    : >"$log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 4096M -smp 1 \
        -display none -cdrom "$IMAGE" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
        -drive "file=$TEST_DIR/$label.img,if=ide,format=raw" \
        -serial "file:$log" \
        -no-reboot -no-shutdown \
        "$@" >"$TEST_DIR/$label-qemu.log" 2>&1 &
    pid=$!

    if ! python3 - "$log" "$label" "$expectations" \
        "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import sys
import time


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
log = pathlib.Path(sys.argv[1])
label = sys.argv[2]
expectations = pathlib.Path(sys.argv[3]).read_text().splitlines()

required = [line for line in expectations if line and not line.startswith("#")]
required.append("[INSTALLER-NIC]")
required.append("install=CONTINUES")

deadline = time.monotonic() + 300.0
missing = list(required)
while time.monotonic() < deadline:
    try:
        body = SERIAL_VIEW.normalise(
            log.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        body = ""
    if "*** HFOC KERNEL PANIC ***" in body:
        raise SystemExit(f"{label}: the kernel panicked instead of describing the NICs")
    missing = [marker for marker in required if marker not in body]
    if not missing:
        break
    time.sleep(0.1)
if missing:
    raise SystemExit(f"{label}: markers never appeared: {missing}")

# The rule the item exists for: the install carries on without a usable NIC.
if " install=CONTINUES" not in body:
    raise SystemExit(f"{label}: the installer did not state that it continues")
if "[PRODUCTION-INSTALL] media token=PRESENT" not in body:
    raise SystemExit(f"{label}: the install flow never reached its own records")
# A capability reading must never claim an initialised device.
for line in body.splitlines():
    if line.startswith("[NIC] devices=") and "available=0" not in line:
        raise SystemExit(f"{label}: a capability reading reported an available NIC: {line}")
    if line.startswith("[NIC] bdf=") and "state=AVAILABLE" in line:
        raise SystemExit(f"{label}: a capability reading claimed AVAILABLE: {line}")
# A storage controller is not a network controller.
if "vendor=8086 device=2829" in body:
    raise SystemExit(f"{label}: a storage controller was described as a NIC")
print(f"{label}: the NIC records followed the hardware")
PY
    then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "NIC discovery proof ($label): FAILED" >&2
        tail -c 3000 "$log" >&2 || true
        exit 1
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "NIC discovery proof ($label): OK"
}

intel=$TEST_DIR/intel.txt
cat >"$intel" <<'EOF'
# One Intel 8254x controller: the image carries its driver, so the capability
# reading is SUPPORTED - not AVAILABLE, because nothing bound it yet.
[NIC] bdf=00:02.00 vendor=8086 device=100E driver=E1000 mac=NONE state=SUPPORTED reason=DRIVER_PRESENT
[NIC] devices=1 supported=1 available=0 unsupported=0 error=0 absent=0
EOF
run_case intel "$intel" \
    -device e1000,netdev=n0 -netdev user,id=n0

supported=$TEST_DIR/supported.txt
cat >"$supported" <<'EOF'
# Two controllers, two drivers: one does not hide the other.
[NIC] bdf=00:02.00 vendor=8086 device=100E driver=E1000 mac=NONE state=SUPPORTED reason=DRIVER_PRESENT
[NIC] bdf=00:03.00 vendor=1AF4 device=1000 driver=VIRTIO-NET mac=NONE state=SUPPORTED reason=DRIVER_PRESENT
[NIC] devices=2 supported=2 available=0 unsupported=0 error=0 absent=0
EOF
run_case supported "$supported" \
    -device e1000,netdev=n0 -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n1 -netdev user,id=n1

none=$TEST_DIR/none.txt
cat >"$none" <<'EOF'
# No controller at all: a reading, not a failure.
[NIC] devices=0 supported=0 available=0 unsupported=0 error=0 absent=1
EOF
# QEMU gives a machine a default NIC (an e1000e on q35) unless it is told
# not to; without this the "no controller" case would describe that NIC and
# the reading would never be tested on a machine that has none.
run_case none "$none" -nic none

echo "NIC discovery QEMU proof: OK"
echo "QEMU/OVMF proof != physical hardware acceptance: the acceptance host's RTL8168 is NOT TESTED by this proof"
