#!/usr/bin/env bash
# Test the PMM console diagnostic on q35 / one vCPU / default CPU, and (ITEM-390)
# boot the same production ISO on a machine above the legacy 16 GiB page bitmap
# cap, which used to panic the boot on real hardware.
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

if [[ "${HFOC_PMM_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PMM_TEST_TIMEOUT:-360}s" \
        env HFOC_PMM_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "APIC baseline must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "APIC baseline must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "APIC baseline must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU APIC baseline: machine=q35 smp=1 cpu=default"

# ITEM-390: the kernel used to refuse to initialise above HFOC_PMM_MAX_PAGES
# (16 GiB) and panicked the boot on any machine with more memory than the
# 512 MiB proofs exercised. This phase boots the production ISO above that
# legacy cap. The default is the largest machine this shared host can start;
# HFOC_PMM_HIGHMEM_QEMU_MEM=32G runs the exact acceptance-host-sized machine on
# a host with enough RAM.
QEMU_PMM_HIGHMEM_MEM=${HFOC_PMM_HIGHMEM_QEMU_MEM:-20G}
LEGACY_CAP_MIB=16384
QEMU_PMM_HIGHMEM_MIB=
case "$QEMU_PMM_HIGHMEM_MEM" in
    *G)
        QEMU_PMM_HIGHMEM_SIZE=${QEMU_PMM_HIGHMEM_MEM%G}
        [[ "$QEMU_PMM_HIGHMEM_SIZE" =~ ^[1-9][0-9]*$ ]] || {
            echo "HFOC_PMM_HIGHMEM_QEMU_MEM must be whole GiB or MiB: $QEMU_PMM_HIGHMEM_MEM" >&2
            exit 1
        }
        QEMU_PMM_HIGHMEM_MIB=$((QEMU_PMM_HIGHMEM_SIZE * 1024))
        ;;
    *M)
        QEMU_PMM_HIGHMEM_SIZE=${QEMU_PMM_HIGHMEM_MEM%M}
        [[ "$QEMU_PMM_HIGHMEM_SIZE" =~ ^[1-9][0-9]*$ ]] || {
            echo "HFOC_PMM_HIGHMEM_QEMU_MEM must be whole GiB or MiB: $QEMU_PMM_HIGHMEM_MEM" >&2
            exit 1
        }
        QEMU_PMM_HIGHMEM_MIB=$QEMU_PMM_HIGHMEM_SIZE
        ;;
    *)
        echo "HFOC_PMM_HIGHMEM_QEMU_MEM must end in G or M: $QEMU_PMM_HIGHMEM_MEM" >&2
        exit 1
        ;;
esac
if (( QEMU_PMM_HIGHMEM_MIB <= LEGACY_CAP_MIB )); then
    echo "The high-memory PMM proof must boot above the legacy 16 GiB cap: $QEMU_PMM_HIGHMEM_MEM" >&2
    exit 1
fi
echo "QEMU PMM high-memory baseline: machine=q35 smp=1 cpu=default mem=$QEMU_PMM_HIGHMEM_MEM legacy_cap_mib=$LEGACY_CAP_MIB"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
HIGHMEM_SERIAL_LOG=$TEST_DIR/highmem-serial.log
HIGHMEM_MONITOR_SOCKET=$TEST_DIR/highmem-monitor.sock
HIGHMEM_MONITOR_LOG=$TEST_DIR/highmem-monitor.log
UEFI_SERIAL_LOG=$TEST_DIR/uefi-serial.log
UEFI_MONITOR_SOCKET=$TEST_DIR/uefi-monitor.sock
UEFI_VARS=$TEST_DIR/OVMF_VARS.fd
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS_SOURCE=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
QEMU_PMM_UEFI_MEM=${HFOC_PMM_UEFI_QEMU_MEM:-16G}
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$HIGHMEM_SERIAL_LOG"
: >"$HIGHMEM_MONITOR_LOG"
: >"$UEFI_SERIAL_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "highmem=$QEMU_PMM_HIGHMEM_MEM" \
    "legacy_cap_mib=$LEGACY_CAP_MIB" \
    "command=pmm" \
    "uefi_mem=$QEMU_PMM_UEFI_MEM" \
    "uefi_firmware=$OVMF_CODE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/pmm-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-PMM test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" \
        "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" "$HIGHMEM_SERIAL_LOG" \
        "$HIGHMEM_MONITOR_LOG" "$UEFI_SERIAL_LOG" "$artifact_dir/"
    echo "PMM test artifacts: $artifact_dir" >&2
}

wait_for_exit() {
    local deadline=$((SECONDS + 5))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
}

send_monitor_quit() {
    local socket=${1:-$MONITOR_SOCKET}
    python3 - "$socket" <<'PY'
import socket
import sys

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

    prompt = b""
    deadline = 5.0
    while b"(qemu)" not in prompt:
        if deadline <= 0.0:
            fail("QEMU HMP prompt timeout")
        monitor.settimeout(deadline)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            fail("QEMU HMP closed before prompt")
        prompt += chunk

    monitor.sendall(b"quit\n")
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

stop_qemu() {
    if [[ -z "$QEMU_PID" ]] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        return 0
    fi
    if send_monitor_quit "$MONITOR_SOCKET" 2>/dev/null; then
        if wait_for_exit; then
            return 0
        fi
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    if wait_for_exit; then
        return 0
    fi
    kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait_for_exit
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        stop_qemu
        save_failure_artifacts
    else
        stop_qemu
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU still running after stop"
            save_failure_artifacts
            status=1
        fi
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# -------------------------------------------------------------- ITEM-390 ---
# Boot the production ISO on a machine above the legacy 16 GiB page bitmap cap
# and require the normal boot markers plus a full PMM coverage publication. On
# the released kernel this boot died with
# "Physical memory manager initialisation failed" before any marker.
highmem_output=
if ! highmem_output=$(python3 - "$IMAGE" "$QEMU_MACHINE" "$QEMU_SMP" \
    "$QEMU_PMM_HIGHMEM_MEM" "$HIGHMEM_SERIAL_LOG" \
    "$HIGHMEM_MONITOR_SOCKET" "$LEGACY_CAP_MIB" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import re
import socket
import subprocess
import sys
import time

(image, machine, smp, memory, serial_path, socket_path, legacy_cap_mib) = sys.argv[1:8]
legacy_cap_mib = int(legacy_cap_mib)
legacy_cap_pages = legacy_cap_mib * 1024 * 1024 // 4096
serial = pathlib.Path(serial_path)
deadline = time.monotonic() + 90.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-machine", machine, "-m", memory, "-smp", str(smp),
    "-display", "none", "-cdrom", image,
    "-serial", f"file:{serial_path}",
    "-monitor", f"unix:{socket_path},server,nowait",
    "-no-reboot", "-no-shutdown",
], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    text = ""
    while time.monotonic() < deadline:
        try:
            text = SERIAL_VIEW.normalise(
                serial.read_bytes().decode("ascii", errors="replace"))
        except FileNotFoundError:
            text = ""
        if "*** HFOC KERNEL PANIC ***" in text:
            detail = [line for line in text.splitlines()
                      if line.strip() and "HFOC KERNEL PANIC" not in line]
            raise SystemExit(
                "high-memory boot panicked on a machine above the legacy cap:\n"
                + "\n".join(detail[-4:]))
        if ("HFOC OS 1.0.0 INITIALISED" in text and "hfoc> " in text
                and "[PMM ] status=" in text):
            break
        if qemu.poll() is not None:
            raise SystemExit(
                f"QEMU exited with {qemu.returncode} before the boot markers")
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="high-memory boot markers" '
            'deadline=90s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the high-memory boot markers")

    match = re.search(
        r"\[PMM \] status=(\S+) coverage=(\S+) pages=(\d+) cap=(\d+) "
        r"usable=(\d+) tracked=(\d+) dropped_pages=(\d+) "
        r"dropped_regions=(\d+) regions=(\d+) bitmap_pages=(\d+) "
        r"largest_region_pages=(\d+) placement_region=(\d+) bitmap_phys=(\d+)",
        text)
    if not match:
        raise SystemExit("the high-memory boot published no PMM coverage line")
    status, coverage = match.group(1), match.group(2)
    pages = int(match.group(3))
    cap = int(match.group(4))
    usable = int(match.group(5))
    tracked = int(match.group(6))
    dropped_pages = int(match.group(7))
    dropped_regions = int(match.group(8))
    bitmap_pages = int(match.group(10))
    bitmap_phys = int(match.group(13))
    if status != "OK":
        raise SystemExit(f"PMM initialisation status is {status}")
    if coverage != "FULL" or dropped_pages != 0 or dropped_regions != 0:
        raise SystemExit(
            f"PMM coverage is {coverage} with dropped_pages={dropped_pages} "
            f"dropped_regions={dropped_regions} on a machine below the cap")
    if pages <= legacy_cap_pages:
        raise SystemExit(
            f"the machine did not present more than the legacy cap: "
            f"pages={pages} legacy_cap_pages={legacy_cap_pages}")
    if usable <= legacy_cap_mib * 1024 * 1024:
        raise SystemExit(
            f"usable memory is not above the legacy cap: {usable} bytes")
    if tracked != pages * 4096:
        raise SystemExit("tracked bytes do not match the tracked page count")
    if cap <= pages:
        raise SystemExit(f"the published page cap {cap} is not above {pages}")
    if bitmap_pages == 0 or bitmap_phys == 0:
        raise SystemExit("the boot published no bitmap placement")

    print(f"legacy_cap_pages={legacy_cap_pages}")
    print(f"pages={pages}")
    print(f"usable_bytes={usable}")
    print(f"bitmap_pages={bitmap_pages}")
    print(f"bitmap_phys={bitmap_phys}")
    print("Boot markers: INITIALISED and hfoc> prompt reached")
    print("PMM coverage: FULL, nothing dropped")
    print("Kernel panic: none")

    # Stop the guest through its monitor, exactly like the 512 MiB phase.
    monitor_deadline = time.monotonic() + 5.0
    prompt = b""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(1.0)
        while True:
            try:
                monitor.connect(socket_path)
                break
            except OSError:
                if time.monotonic() >= monitor_deadline:
                    raise SystemExit("high-memory QEMU monitor never accepted")
                time.sleep(0.05)
        while b"(qemu)" not in prompt:
            if time.monotonic() >= monitor_deadline:
                raise SystemExit("high-memory QEMU monitor prompt timeout")
            try:
                chunk = monitor.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise SystemExit("high-memory QEMU monitor closed early")
            prompt += chunk
        monitor.sendall(b"quit\n")
    quit_deadline = time.monotonic() + 5.0
    while qemu.poll() is None and time.monotonic() < quit_deadline:
        time.sleep(0.05)
finally:
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait(timeout=5.0)
PY
); then
    FAILURE_MESSAGE=$highmem_output
    printf '%s\n' "$highmem_output" >&2
    exit 1
fi
printf '%s\n' "$highmem_output"
echo "QEMU PMM high-memory boot: OK"

# -------------------------------------------------------------- ITEM-393 ---
# The acceptance host boots the production ISO from UEFI firmware with about
# 16 GiB of RAM: low memory, a PCI aperture, and RAM above 4 GiB. The two
# phases above boot SeaBIOS machines, whose map is a handful of flat ranges.
# This phase boots the same production ISO under OVMF, whose UEFI memory map is
# the fragmented shape a real board hands over (memory-mapped IO apertures,
# ACPI NVS, reserved holes), at the host's size, and requires the ordinary boot
# markers plus a PMM line whose numbers prove that shape was reached: usable
# memory above 4 GiB and a tracked region above the aperture.
#
# The defect this phase exists for faulted in memset on physical 0xFC000000,
# inside the host's PCI aperture, so a boot that reaches the markers here also
# proves the PMM tracked - and wrote - only RAM on a UEFI-shaped map.
for ovmf_image in "$OVMF_CODE" "$OVMF_VARS_SOURCE"; do
    [[ -f "$ovmf_image" ]] || {
        echo "Missing OVMF image: $ovmf_image" >&2
        exit 1
    }
done
cp "$OVMF_VARS_SOURCE" "$UEFI_VARS"
echo "QEMU PMM UEFI baseline: machine=$QEMU_MACHINE smp=$QEMU_SMP cpu=default mem=$QEMU_PMM_UEFI_MEM firmware=$OVMF_CODE"

uefi_output=
if ! uefi_output=$(python3 - "$IMAGE" "$QEMU_MACHINE" "$QEMU_SMP" \
    "$QEMU_PMM_UEFI_MEM" "$UEFI_SERIAL_LOG" "$UEFI_MONITOR_SOCKET" \
    "$OVMF_CODE" "$UEFI_VARS" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PYUEFI'
import pathlib
import re
import socket
import subprocess
import sys
import time

(image, machine, smp, memory, serial_path, socket_path, ovmf_code,
 ovmf_vars) = sys.argv[1:9]
serial = pathlib.Path(serial_path)
deadline = time.monotonic() + 240.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-machine", machine, "-m", memory, "-smp", str(smp),
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
    "-drive", f"if=pflash,format=raw,file={ovmf_vars}",
    "-display", "none", "-cdrom", image,
    "-serial", f"file:{serial_path}",
    "-monitor", f"unix:{socket_path},server,nowait",
    "-no-reboot", "-no-shutdown",
], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    text = ""
    while time.monotonic() < deadline:
        try:
            text = SERIAL_VIEW.normalise(
                serial.read_bytes().decode("ascii", errors="replace"))
        except FileNotFoundError:
            text = ""
        if "*** HFOC KERNEL PANIC ***" in text:
            detail = [line for line in text.splitlines()
                      if line.strip() and "HFOC KERNEL PANIC" not in line]
            raise SystemExit(
                "the UEFI machine panicked before the boot markers:\n"
                + "\n".join(detail[-6:]))
        if ("HFOC OS 1.0.0 INITIALISED" in text and "hfoc> " in text
                and "[PMM ] status=" in text):
            break
        if qemu.poll() is not None:
            raise SystemExit(
                f"QEMU exited with {qemu.returncode} before the boot markers")
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="UEFI boot markers" '
            'deadline=240s\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for the UEFI boot markers")

    match = re.search(
        r"\[PMM \] status=(\S+) coverage=(\S+) pages=(\d+) cap=(\d+) "
        r"usable=(\d+) tracked=(\d+) dropped_pages=(\d+) "
        r"dropped_regions=(\d+) regions=(\d+) bitmap_pages=(\d+) "
        r"largest_region_pages=(\d+) placement_region=(\d+) bitmap_phys=(\d+) "
        r"first_region_base=(\d+) first_region_pages=(\d+) "
        r"largest_region_base=(\d+) last_region_base=(\d+) "
        r"bitmap_end_phys=(\d+)",
        text)
    if not match:
        raise SystemExit("the UEFI boot published no PMM coverage line")
    status = match.group(1)
    coverage = match.group(2)
    pages = int(match.group(3))
    usable = int(match.group(5))
    tracked = int(match.group(6))
    regions = int(match.group(9))
    bitmap_pages = int(match.group(10))
    bitmap_phys = int(match.group(13))
    largest_region_base = int(match.group(16))
    last_region_base = int(match.group(17))
    if status != "OK":
        raise SystemExit(f"PMM initialisation status is {status}")
    if coverage == "NONE":
        raise SystemExit("the UEFI boot published coverage=NONE")
    if usable <= 4 * 1024 * 1024 * 1024:
        raise SystemExit(
            "the UEFI machine did not present more than 4 GiB of usable "
            f"memory: {usable} bytes")
    if largest_region_base < 0x100000000:
        raise SystemExit(
            "the UEFI machine presented no usable memory above 4 GiB: "
            f"largest_region_base={largest_region_base:#x}")
    if last_region_base < 0x100000000:
        raise SystemExit(
            "the highest usable region is not above 4 GiB: "
            f"last_region_base={last_region_base:#x}")
    if tracked != pages * 4096:
        raise SystemExit("tracked bytes do not match the tracked page count")
    if bitmap_pages == 0 or bitmap_phys == 0:
        raise SystemExit("the boot published no bitmap placement")

    print(f"status={status}")
    print(f"coverage={coverage}")
    print(f"regions={regions}")
    print(f"pages={pages}")
    print(f"usable_bytes={usable}")
    print(f"bitmap_pages={bitmap_pages}")
    print(f"bitmap_phys={bitmap_phys:#x}")
    print(f"largest_region_base={largest_region_base:#x}")
    print(f"last_region_base={last_region_base:#x}")
    print("Boot markers: INITIALISED and hfoc> prompt on UEFI firmware")
    print("PMM coverage: published, memory above 4 GiB tracked")
    print("Kernel panic: none")

    monitor_deadline = time.monotonic() + 5.0
    prompt = b""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(1.0)
        while True:
            try:
                monitor.connect(socket_path)
                break
            except OSError:
                if time.monotonic() >= monitor_deadline:
                    raise SystemExit("UEFI QEMU monitor never accepted")
                time.sleep(0.05)
        while b"(qemu)" not in prompt:
            if time.monotonic() >= monitor_deadline:
                raise SystemExit("UEFI QEMU monitor prompt timeout")
            try:
                chunk = monitor.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise SystemExit("UEFI QEMU monitor closed early")
            prompt += chunk
        monitor.sendall(b"quit\n")
    quit_deadline = time.monotonic() + 5.0
    while qemu.poll() is None and time.monotonic() < quit_deadline:
        time.sleep(0.05)
finally:
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait(timeout=5.0)
PYUEFI
); then
    FAILURE_MESSAGE=$uefi_output
    printf '%s\n' "$uefi_output" >&2
    exit 1
fi
printf '%s\n' "$uefi_output"
echo "QEMU PMM UEFI host-shaped boot: OK"

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int,guest_errors -D "$QEMU_LOG" \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
deadline = time.monotonic() + 30.0


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def serial_text():
    return SERIAL_VIEW.normalise(
        serial_bytes().decode("ascii", errors="replace"))

while time.monotonic() < deadline:
    text = serial_text()
    if "HFOC OS 1.0.0 INITIALISED" in text and "hfoc> " in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before PMM command")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt" '
        'deadline=30s\n')
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

    def record(value):
        with monitor_log.open("a", encoding="utf-8") as output:
            output.write(value)

    record(receive_prompt())
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")

    for key in ['p', 'm', 'm', 'ret']:
        monitor.sendall(f"sendkey {key}\n".encode())
        record(receive_prompt())
        time.sleep(0.06)
PY
); then
    FAILURE_MESSAGE=$phase1_output
    printf '%s\n' "$phase1_output" >&2
    exit 1
fi

SERIAL_OFFSET=$(<"$SERIAL_OFFSET_FILE")
printf '%s\n' "serial_offset=$SERIAL_OFFSET" >>"$METADATA"

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_LOG" "$QEMU_PID" \
    "$SERIAL_OFFSET" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import sys
import time

serial = pathlib.Path(sys.argv[1])
qemu_log = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
offset = int(sys.argv[4])
deadline = time.monotonic() + 15.0


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def normalise_suffix(text):
    return SERIAL_VIEW.normalise(text)

split_fixture = (
    "[PMM ] usable=[LAPIC-TIMER] irq vector=0x42\n"
    "[LAPIC-TIMER] irq lapic_eoi\n"
    "532725760 free_pages=128971\n"
    "hfoc> "
)
normalised_fixture = normalise_suffix(split_fixture)
if "[PMM ] usable=532725760 free_pages=128971" not in normalised_fixture:
    raise SystemExit("PMM parser fixture split by periodic diagnostics")

negative_fixture = ("[IOAPIC] route failed: irq=1 status=11\n"
                    "*** HFOC KERNEL PANIC ***\n"
                    "arbitrary line must survive\n")
if normalise_suffix(negative_fixture) != negative_fixture:
    raise SystemExit("PMM normaliser removed a non-diagnostic line")

print("PMM parser fixture: exact after normalisation")
print("PMM parser negative lines: preserved")

def read_suffix():
    data = serial.read_bytes()
    return data[offset:].decode("ascii", errors="replace")

while time.monotonic() < deadline:
    suffix = normalise_suffix(read_suffix())
    if "[PMM ] usable=" in suffix and "free_pages=" in suffix and \
            "hfoc> " in suffix:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="PMM diagnostic" '
        'deadline=15s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for PMM diagnostic")

suffix = normalise_suffix(read_suffix())
match = re.search(r"\[PMM \] usable=([0-9]+) free_pages=([0-9]+)", suffix)
if not match:
    raise SystemExit("Missing exact [PMM] diagnostic")
usable = int(match.group(1))
free_pages = int(match.group(2))
if usable == 0:
    raise SystemExit("PMM usable bytes must be nonzero")
if free_pages == 0:
    raise SystemExit("PMM free_pages must be nonzero")

diagnostic_index = suffix.find("[PMM ] usable=")
if diagnostic_index < 0 or "hfoc> " not in suffix[diagnostic_index:]:
    raise SystemExit("No new hfoc> prompt after PMM command")

full = SERIAL_VIEW.normalise(
    serial.read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Unexpected kernel panic")

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

if not qemu_is_running():
    raise SystemExit("QEMU exited before validation completed")

time.sleep(0.5)
if not qemu_is_running():
    raise SystemExit("QEMU exited during observation interval")

print(f"usable={usable}")
print(f"free_pages={free_pages}")
print("Exact diagnostic: found")
print("New hfoc> after command: yes")
print("Kernel panic: none")
print("QEMU alive: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU PMM test: OK"
