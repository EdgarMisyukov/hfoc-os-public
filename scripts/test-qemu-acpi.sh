#!/usr/bin/env bash
# Test that ACPI RSDP/RSDT(MADT) parsing succeeds on real q35 hardware
# with SeaBIOS (RSDP revision 0, RSDT). XSDT coverage is provided by
# host tests.
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

if [[ "${HFOC_ACPI_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ACPI_TEST_TIMEOUT:-30}s" \
        env HFOC_ACPI_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
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
    "cpu=$QEMU_CPU" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/acpi-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-ACPI MADT test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "ACPI test artifacts: $artifact_dir" >&2
}

wait_for_exit() {
    local deadline=$((SECONDS + 1))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
}

stop_qemu() {
    if [[ -z "$QEMU_PID" ]] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        return 0
    fi
    python3 - "$MONITOR_SOCKET" <<'PY' >/dev/null 2>&1 || true
import socket, sys
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(0.5)
        monitor.connect(sys.argv[1])
        monitor.sendall(b"quit\n")
except OSError:
    pass
PY
    if wait_for_exit; then return 0; fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    if wait_for_exit; then return 0; fi
    kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait_for_exit
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    stop_qemu
    local qemu_status=$?
    if (( status == 0 )); then
        if (( qemu_status != 0 )); then
            FAILURE_MESSAGE="QEMU did not terminate after quit/TERM/KILL"
            save_failure_artifacts
            status=1
        fi
    else
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# QEMU: APIC enabled, single vCPU.
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -cpu "$QEMU_CPU" -smp 1 \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int,guest_errors -D "$QEMU_LOG" \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!

# Validation: read COM1 output and verify all ACPI markers.
validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os, pathlib, re, sys, time

serial = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
deadline = time.monotonic() + 15.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def read_serial():
    try:
        return SERIAL_VIEW.normalise(
            serial.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

# Wait for ACPI success markers (before any hfoc> prompt).
while time.monotonic() < deadline:
    text = read_serial()
    if "[ACPI] Parsed successfully; legacy PIC retained" in text:
        break
    if "[ACPI] unavailable:" in text:
        raise SystemExit("ACPI unavailable on q35")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during ACPI probe")
    time.sleep(0.1)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="ACPI success markers" '
        'deadline=15s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for ACPI success markers")

# Verify root table (XSDT or RSDT).
if "[ACPI] Root: XSDT" in text:
    root_source = "XSDT"
elif "[ACPI] Root: RSDT" in text:
    root_source = "RSDT"
else:
    raise SystemExit("expected [ACPI] Root: XSDT or RSDT on q35")

# Verify MADT line with non-zero counts.
madt_match = re.search(
    r"\[ACPI\] MADT: lapic=([0-9]+) ioapic=([0-9]+) "
    r"iso=([0-9]+) lapic-nmi=([0-9]+) "
    r"address=(0x[0-9a-fA-F]{16})", text)
if not madt_match:
    raise SystemExit("MADT summary line not found or malformed")

lapic_n = int(madt_match.group(1))
ioapic_n = int(madt_match.group(2))
lapic_addr_str = madt_match.group(5)

if lapic_n == 0:
    raise SystemExit("expected at least one LAPIC")
if ioapic_n == 0:
    raise SystemExit("expected at least one IOAPIC")

lapic_addr = int(lapic_addr_str, 16)
if lapic_addr == 0:
    raise SystemExit("LAPIC address must be non-zero")
if lapic_addr % 4096 != 0:
    raise SystemExit(f"LAPIC address not page-aligned: {lapic_addr:#x}")

# Verify no kernel panic line.
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("unexpected kernel panic")

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

print(f"Found {root_source}: yes")
print(f"LAPIC count: {lapic_n}")
print(f"IOAPIC count: {ioapic_n}")
print(f"LAPIC address: {lapic_addr_str}")
print("Parsed successfully: yes")
print("No kernel panic: confirmed")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if stop_qemu; then
    QEMU_PID=
    echo "QEMU ACPI MADT test: OK"
else
    FAILURE_MESSAGE="QEMU did not terminate after quit/TERM/KILL"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
