#!/usr/bin/env bash
# ITEM-598: the installer carries a host-provisioned seed, asks the operator for
# the normal login/password, seals the seed in the persistence partition, and
# the installed appliance unlocks it through the same login on later boots.
#
# The proof runs on a CPU profile without RDRAND/RDSEED.  It performs a real
# UEFI install, boots the installed disk twice, requires
# `source=HOST_PROVISIONED state=READY`, a live TLS panel, the persisted
# self-signed certificate, and durable two-slot epoch advancement.  It never
# enters `entropy seed`.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate xorriso \
    sha256sum; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_ITEM598_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ITEM598_TIMEOUT:-900}s" \
        env HFOC_ITEM598_INNER=1 "$0" "$@"
fi

INSTALLER_KERNEL=${INSTALLER_KERNEL:-build/production-installer/hfoc-kernel.elf}
INSTALLED_KERNEL=${INSTALLED_KERNEL:-build/hfoc-kernel.elf}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
TARGET=${HFOC_ITEM598_TARGET:-HOSTSEEDDEMO}
ACCOUNT_LOGIN=${HFOC_ITEM598_LOGIN:-operator}
ACCOUNT_PASSWORD=${HFOC_ITEM598_PASSWORD:-hfocinstallpass}
UPDATED_PASSWORD=${HFOC_ITEM598_UPDATED_PASSWORD:-hfocinstallpass2}
WRONG_PASSWORD=${HFOC_ITEM598_WRONG_PASSWORD:-definitely-wrong-password}
BUILD_ID=${HFOC_ITEM598_BUILD_ID:-1}
MANAGEMENT=${HFOC_ITEM598_MANAGEMENT:-management-interface}
NETWORK=${HFOC_ITEM598_NETWORK:-dhcp}

for path in "$INSTALLER_KERNEL" "$INSTALLED_KERNEL" "$OVMF_CODE" \
    "$OVMF_VARS" deps/limine/limine-bios-cd.bin \
    deps/limine/limine-uefi-cd.bin deps/limine/BOOTX64.EFI \
    scripts/hfoc-host-seed-provision.py \
    scripts/hfoc-install-media-stage.sh; do
    [[ -f "$path" ]] || {
        echo "Missing required file: $path" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/installed.raw
IMAGE=$TEST_DIR/host-provisioned.iso
SEED_RECORD=$TEST_DIR/host-seed.bin
QEMU_PID=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$QEMU_PID" ]]; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    # The test directory contains the personalised medium and its seed record.
    # It is never copied into a CI artifact.
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

python3 scripts/hfoc-host-seed-provision.py \
    --output "$SEED_RECORD" --target "$TARGET" --build-id "$BUILD_ID"

HFOC_INSTALL_MEDIA_ENTRY=console \
HFOC_INSTALL_MEDIA_HOST_SEED="$SEED_RECORD" \
    bash scripts/hfoc-install-media-stage.sh \
        "$TEST_DIR/iso-root" "$INSTALLER_KERNEL" "$INSTALLED_KERNEL" \
        ITEM-394-UNATTENDED-INSTALL "$TARGET" "$MANAGEMENT" whole-disk \
        "$NETWORK"

xorriso -as mkisofs -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$TEST_DIR/iso-root" -o "$IMAGE" >/dev/null
python3 scripts/normalize-iso-gpt.py "$IMAGE" "$(git rev-parse HEAD)"
deps/limine/limine bios-install "$IMAGE" >/dev/null
python3 scripts/normalize-iso-gpt.py "$IMAGE" "$(git rev-parse HEAD)"
python3 scripts/hfoc-install-media-prealloc.py "$IMAGE"
chmod 600 "$IMAGE"

truncate -s "${HFOC_ITEM598_DISK_BYTES:-120G}" "$DISK"

print_diagnostics() {
    local label=$1
    local log=$2
    python3 - "$label" "$log" "$ACCOUNT_PASSWORD" "$UPDATED_PASSWORD" \
        "$WRONG_PASSWORD" <<'PY' >&2 || true
import importlib.util
import pathlib
import sys

label, path, password, updated_password, wrong_password = sys.argv[1:]
spec = importlib.util.spec_from_file_location(
    "serial_view", pathlib.Path("scripts/hfoc-serial-view.py"))
serial_view = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serial_view)
text = serial_view.normalise(
    pathlib.Path(path).read_bytes().decode("ascii", errors="replace"))
text = text.replace(password, "<redacted>")
text = text.replace(updated_password, "<redacted>")
text = text.replace(wrong_password, "<redacted>")
interesting = (
    "PRODUCTION-INSTALL", "[RNG]", "Login:", "Password:", "Select installation",
    "Choose the installation", "WARNING:", "Confirm the installation",
    "[PRODUCTION-SERVING]", "KERNEL PANIC")
print(f"{label}: diagnostics")
for line in text.splitlines():
    if any(marker in line for marker in interesting):
        print(line)
PY
}

run_phase() {
    local phase=$1
    local log=$TEST_DIR/$phase.log
    local monitor=$TEST_DIR/$phase.monitor
    local vars=$TEST_DIR/$phase-vars.fd
    local firmware=()
    local installed=()

    cp "$OVMF_VARS" "$vars"
    : >"$log"
    firmware=(
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$vars")
    if [[ "$phase" == install ]]; then
        qemu-system-x86_64 \
            -machine q35 -m 3072M -smp 1 -cpu qemu64 -display none \
            -drive "file=$IMAGE,if=none,id=cd,format=raw,media=cdrom,readonly=on" \
            -device "ide-cd,drive=cd,bus=ide.2,unit=0" \
            "${firmware[@]}" \
            -drive "file=$DISK,if=none,id=target,format=raw,cache=writeback,file.locking=off" \
            -device "ide-hd,drive=target,bus=ide.0,unit=0,serial=$TARGET" \
            -serial "file:$log" \
            -monitor "unix:$monitor,server,nowait" \
            -no-reboot >"$TEST_DIR/$phase.qemu" 2>&1 &
    else
        installed=(
            '-netdev' 'user,id=net0'
            -device 'virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on')
        qemu-system-x86_64 \
            -machine q35 -m 3072M -smp 1 -cpu qemu64 -display none \
            "${firmware[@]}" \
            -drive "file=$DISK,if=none,id=target,format=raw,cache=writeback,file.locking=off" \
            -device "ide-hd,drive=target,bus=ide.0,unit=0,serial=$TARGET" \
            "${installed[@]}" \
            -serial "file:$log" \
            -monitor "unix:$monitor,server,nowait" \
            -no-reboot -no-shutdown >"$TEST_DIR/$phase.qemu" 2>&1 &
    fi
    QEMU_PID=$!

    if ! python3 - "$phase" "$log" "$monitor" "$ACCOUNT_LOGIN" \
        "$ACCOUNT_PASSWORD" "$UPDATED_PASSWORD" "$WRONG_PASSWORD" \
        "$TARGET" <<'PY'
import importlib.util
import pathlib
import re
import socket
import sys
import time

(
    phase, log_path, monitor_path, login, original_password,
    updated_password, wrong_password, target
) = sys.argv[1:]
password = (
    updated_password
    if phase in ("boot2", "seed-corrupt")
    else original_password
)
spec = importlib.util.spec_from_file_location(
    "serial_view", pathlib.Path("scripts/hfoc-serial-view.py"))
serial_view = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serial_view)


def serial_text():
    try:
        return serial_view.normalise(
            pathlib.Path(log_path).read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def wait_for(markers, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit(f"{phase}: panic while waiting for {what}")
        missing = [marker for marker in markers if marker not in text]
        if not missing:
            return
        time.sleep(0.1)
    marker = ", ".join(markers)
    sys.stderr.write(
        "HFOC-QEMU-MARKER-TIMEOUT "
        f'marker="{marker}" deadline={int(timeout)}s\n')
    sys.stderr.flush()
    raise SystemExit(f"{phase}: missing {missing} while waiting for {what}")


def open_monitor():
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        if pathlib.Path(monitor_path).exists():
            try:
                monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                monitor.settimeout(5.0)
                monitor.connect(monitor_path)
                received = b""
                while b"(qemu)" not in received:
                    received += monitor.recv(4096)
                return monitor
            except OSError:
                pass
        time.sleep(0.1)
    raise SystemExit(f"{phase}: QEMU monitor never answered")


def login_traces():
    return re.findall(r"\[LOGIN-TRACE\][^\r\n]*", serial_text())


def require_login_trace():
    traces = login_traces()
    if not traces:
        raise SystemExit(f"{phase}: no [LOGIN-TRACE] record")
    for line in traces:
        for field in (" v=1 ", " build=", " available=1 ", " attempt=",
                      " ok=", " stage=",
                      " account_us=", " account_iterations=", " seed_us=",
                      " seed_reads=", " seed_opens=", " seed_seals=",
                      " seed_writes=", " seed_readbacks=",
                      " seed_kdf_calls=", " seed_kdf_iterations=",
                      " seed_read_us=", " seed_open_us=", " seed_seal_us=",
                      " seed_write_us=", " seed_readback_us=",
                      " publish_us=", " entropy_us=", " total_us="):
            if field not in line:
                raise SystemExit(
                    f"{phase}: trace lacks {field}: {line}")
        if " ok=1 " in line:
            metrics = {
                name: int(value)
                for name, value in re.findall(
                    r" (seed_kdf_calls|seed_kdf_iterations|seed_opens|"
                    r"seed_seals|account_iterations)=(\d+)", line)
            }
            if metrics.get("seed_kdf_calls", 0) < 3 or \
                    metrics.get("seed_kdf_iterations", 0) < 60000 or \
                    metrics.get("seed_opens", 0) < 2 or \
                    metrics.get("seed_seals", 0) < 1:
                raise SystemExit(
                    f"{phase}: successful trace lost a required seed stage: "
                    f"{line}")
            if metrics.get("account_iterations", 0) != 200000:
                raise SystemExit(
                    f"{phase}: trace did not report the stored account "
                    f"iterations: {line}")
    return traces


def press(monitor, key):
    monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        received += monitor.recv(4096)
    time.sleep(0.08)


def type_text(monitor, value):
    names = {" ": "spc", "-": "minus", ".": "dot"}
    for character in value:
        press(monitor, names.get(character, character))


def type_line(monitor, value):
    type_text(monitor, value)
    press(monitor, "ret")


# ITEM-610: the credential echo is deliberately framebuffer-only, so the QEMU
# proof reads the rendered cells instead of the serial transcript.  A
# screendump before and after typing isolates exactly the cells the echo added;
# the login's cells differ from each other, and every password cell must be the
# same asterisk glyph.
def screendump(monitor, path):
    monitor.sendall(("screendump " + str(path) + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        received += monitor.recv(4096)
    time.sleep(0.05)


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{phase}: the screendump is not a binary PPM")
    fields = data.split(b"\n", 3)
    if len(fields) < 4:
        raise SystemExit(f"{phase}: the screendump header is short")
    width, height = (int(value) for value in fields[1].split())
    pixels = fields[3]
    if width <= 0 or height <= 0 or len(pixels) < width * height * 3:
        raise SystemExit(f"{phase}: the screendump has no pixels")
    return width, height, pixels


def console_cell():
    match = re.search(
        r"\[FB\].*?scale=(\d+) cell_px=(\d+)x(\d+)", serial_text())
    if match is None:
        raise SystemExit(f"{phase}: the console published no cell geometry")
    return int(match.group(2)), int(match.group(3))


def wait_quiet(seconds=0.4, timeout=8.0):
    deadline = time.monotonic() + timeout
    last = len(serial_text())
    stable = 0.0
    while time.monotonic() < deadline:
        time.sleep(0.05)
        current = len(serial_text())
        if current == last:
            stable += 0.05
            if stable >= seconds:
                return
        else:
            last = current
            stable = 0.0


# The echo is framebuffer-only, so the proof isolates the cells it added with a
# screendump before and after typing.  The row with the most changed pixels is
# the answer line; the changed columns are grouped on the console grid.  If a
# periodic record lands in the same window, the check clears the typed cells and
# retries once the screen is quiet instead of accepting a wrong count.
def echo_cells(before_path, after_path, cell_width):
    width, height, before = read_ppm(before_path)
    width_after, height_after, after = read_ppm(after_path)
    if (width, height) != (width_after, height_after):
        raise SystemExit(f"{phase}: the two frames have different sizes")
    changed_rows = {}
    changed_columns = {}
    for y in range(height):
        row = y * width * 3
        count = 0
        for x in range(width):
            offset = row + x * 3
            if before[offset:offset + 3] != after[offset:offset + 3]:
                count += 1
                changed_columns.setdefault(y, []).append(x)
        if count:
            changed_rows[y] = count
    if not changed_rows:
        return 0, []
    best_row = max(changed_rows, key=changed_rows.get)
    first_row = best_row
    while first_row - 1 in changed_rows:
        first_row -= 1
    last_row = best_row
    while last_row + 1 in changed_rows:
        last_row += 1
    columns = set()
    for row in range(first_row, last_row + 1):
        columns.update(changed_columns.get(row, ()))
    indices = sorted({(x - 12) // cell_width for x in columns})
    bitmaps = []
    for index in indices:
        cells = []
        for y in range(first_row, last_row + 1):
            start = y * width * 3 + (12 + index * cell_width) * 3
            cells.append(after[start:start + cell_width * 3])
        bitmaps.append(b"".join(cells))
    return len(indices), bitmaps


def wait_echo(monitor, label, value, secret):
    cell_width, _ = console_cell()
    before_path = pathlib.Path(log_path).with_name(phase + "-" + label + "-before.ppm")
    after_path = pathlib.Path(log_path).with_name(phase + "-" + label + "-after.ppm")
    observed = 0
    bitmaps = []
    for attempt in range(3):
        wait_quiet()
        screendump(monitor, before_path)
        type_text(monitor, value)
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            screendump(monitor, after_path)
            observed, bitmaps = echo_cells(before_path, after_path,
                                           cell_width)
            if observed == len(value):
                break
            time.sleep(0.1)
        if observed == len(value):
            break
        # A periodic record moved the screen inside the window: remove the
        # typed cells, let the screen settle and take a fresh pair of frames.
        for _ in range(len(value)):
            press(monitor, "backspace")
        wait_quiet()
    if observed != len(value):
        raise SystemExit(
            f"{phase}: {label} echo painted {observed} cells, "
            f"expected {len(value)}")
    if secret and (len(bitmaps) <= 1 or any(
            bitmap != bitmaps[0] for bitmap in bitmaps)):
        raise SystemExit(
            f"{phase}: {label} cells are not one repeated asterisk glyph")
    if not secret and len(set(bitmaps)) <= 1:
        raise SystemExit(f"{phase}: {label} painted no distinguishable login")
    print(f"host-provisioned entropy {phase}: {label} pixels=OK cells={observed}")


if phase == "install":
    wait_for(["Select installation disk"], 300.0, "disk screen")
    monitor = open_monitor()
    press(monitor, "ret")
    wait_for(["Choose the installation scheme"], 120.0, "scheme screen")
    press(monitor, "ret")
    wait_for(
        ["WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"],
        120.0, "erase warning")
    press(monitor, "ret")
    wait_for(["Confirm the installation plan"], 120.0, "summary")
    press(monitor, "ret")
    wait_for(["New login:"], 120.0, "new login")
    type_line(monitor, login)
    wait_for(["New password:"], 120.0, "new password")
    type_line(monitor, password)
    wait_for(["Repeat new password:"], 120.0, "password confirmation")
    type_line(monitor, password)
    wait_for(
        ["[PRODUCTION-INSTALL] host-seed=PROVISIONED epoch=1"],
        600.0, "sealed host seed")
    wait_for(
        [f"[PRODUCTION-INSTALL] complete disk=1 identity={target}"],
        600.0, "completed install")
else:
    wait_for(
        [
            "reason=no_hardware_entropy_source",
            "entropy=NONE",
            "Login:",
        ],
        300.0, "no-hardware-RNG login")
    monitor = open_monitor()
    if phase == "boot1":
        wait_echo(monitor, "wrong-login", login, False)
        press(monitor, "ret")
        wait_for(["Password:"], 120.0, "wrong-login password")
        wait_echo(monitor, "wrong-password", wrong_password, True)
        press(monitor, "ret")
        wait_for(
            ["[RNG] host unlock stage=ACCOUNT status=DENIED"],
            120.0,
            "account-stage refusal",
        )
        type_line(monitor, "entropy unlock")
        wait_for(["Login:"], 120.0, "retry login")
    wait_echo(monitor, "login", login, False)
    press(monitor, "ret")
    wait_for(["Password:"], 120.0, "login password")
    wait_echo(monitor, "password", password, True)
    press(monitor, "ret")
    if phase == "seed-corrupt":
        wait_for(
            ["[RNG] host unlock stage=SEED status=WRONG_PASSWORD"],
            300.0,
            "seed-stage refusal",
        )
        if "[PRODUCTION-SERVING] v=1 state=LISTENING" in serial_text():
            raise SystemExit(
                f"{phase}: listener started despite the seed refusal"
            )
        text = serial_text()
        if (
            original_password in text
            or updated_password in text
            or wrong_password in text
        ):
            raise SystemExit(f"{phase}: hidden password reached the serial log")
        for line in require_login_trace():
            print("TRACE " + line)
        print(f"host-provisioned entropy {phase}: OK")
        sys.exit(0)
    wait_for(
        [
            "[RNG] host seed status=OK stage=COMPLETE "
            "source=HOST_PROVISIONED state=READY credited=32",
            "[PRODUCTION-SERVING] v=1 state=LISTENING",
            "listener=live",
            "entropy=HOST_PROVISIONED",
            "certificate=persisted_self_signed",
            "panel=published",
        ],
        300.0, "host-provisioned panel",
    )
    if phase == "boot1":
        # ITEM-614: the completed login trace must survive into a real
        # snapshot composition, not only the serial stream.  This installed
        # boot has no diagnostic journal, so `diag save` composes the RAM
        # snapshot and prints the same records; the export path is the real
        # composer, and the record must still say available=1 after the
        # login that just succeeded.
        diag_mark = len(serial_text())
        type_line(monitor, "diag save")
        diag_deadline = time.monotonic() + 60.0
        while time.monotonic() < diag_deadline:
            snapshot = serial_text()[diag_mark:]
            # The snapshot is printed one record at a time.  Waiting for the
            # bare "[LOGIN-TRACE]" marker returns while the last fields are
            # still on their way (the 8x16 console paints the record more
            # slowly than the 6x8 one), so require the record's final field
            # before reading it as evidence.  A record cut before total_us= is
            # a real timeout, not a partial read.
            trace_start = snapshot.find("[LOGIN-TRACE]")
            if ("DIAG SNAPSHOT records=" in snapshot and
                    trace_start >= 0 and
                    re.search(r"\[LOGIN-TRACE\][^\r\n]*"
                              r"total_us=[0-9]+", snapshot[trace_start:])):
                break
            if "*** HFOC KERNEL PANIC ***" in snapshot:
                raise SystemExit("kernel panic during the ITEM-614 snapshot")
            time.sleep(0.05)
        else:
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="DIAG SNAPSHOT records=" '
                'deadline=60s\n')
            sys.stderr.flush()
            raise SystemExit(
                f"{phase}: the failed `diag save` printed no RAM snapshot "
                f"with [LOGIN-TRACE]")
        snapshot = serial_text()[diag_mark:]
        joined = []
        for line in snapshot.splitlines():
            if line.startswith("[DIAG+] ") and joined:
                joined[-1] += " " + line[len("[DIAG+] "):]
            else:
                joined.append(line)
        trace = next(
            (line for line in joined if line.startswith("[LOGIN-TRACE]")),
            None)
        if trace is None:
            raise SystemExit(
                f"{phase}: the diag snapshot lacks [LOGIN-TRACE]")
        for field in (" available=1 ", " ok=1 ", " attempt=", " stage=",
                      " account_us=", " account_iterations=", " seed_us=",
                      " seed_reads=", " seed_writes=", " seed_opens=",
                      " seed_seals=", " seed_readbacks=", " seed_kdf_calls=",
                      " seed_kdf_iterations=", " seed_read_us=",
                      " seed_open_us=", " seed_seal_us=", " seed_write_us=",
                      " seed_readback_us=", " publish_us=", " entropy_us=",
                      " total_us="):
            if field not in trace:
                raise SystemExit(
                    f"{phase}: the diag snapshot trace lacks {field}: {trace}")
        if "state=NOT_ATTEMPTED" in trace or "state=IN_PROGRESS" in trace:
            raise SystemExit(
                f"{phase}: the snapshot trace is not the finished attempt: "
                f"{trace}")
        print("SNAPSHOT TRACE " + trace)
        # ITEM-620: the shared status carries the selected console geometry,
        # so the next SSH read needs no photo and no new command.  The boot
        # [FB] record is the independent source for the same numbers.
        fb_boot = re.search(
            r"\[FB\] width=(\d+) height=(\d+) pitch=(\d+) cells=(\d+)x(\d+)"
            r" bpp=(\d+) ", serial_text())
        fb_status = re.search(
            r"\[FB-GEOMETRY\] state=READY width=(\d+) height=(\d+) bpp=(\d+)"
            r" scale=(\d+) cell_px=(\d+)x(\d+) cells=(\d+)x(\d+)",
            snapshot)
        if fb_boot is None or fb_status is None:
            raise SystemExit(
                f"{phase}: the shared snapshot lacks the [FB-GEOMETRY] record "
                f"or the boot [FB] record it repeats")
        if (int(fb_status.group(1)), int(fb_status.group(2)),
                int(fb_status.group(3))) != (
                int(fb_boot.group(1)), int(fb_boot.group(2)),
                int(fb_boot.group(6))):
            raise SystemExit(
                f"{phase}: [FB-GEOMETRY] disagrees with the boot [FB] record")
        scale = int(fb_status.group(4))
        cell = (int(fb_status.group(5)), int(fb_status.group(6)))
        cells = (int(fb_status.group(7)), int(fb_status.group(8)))
        if cell != (8 * scale, 16 * scale):
            raise SystemExit(
                f"{phase}: [FB-GEOMETRY] cell {cell} is not 8x16 at "
                f"scale={scale}")
        if cells != (int(fb_boot.group(1)) // cell[0],
                     int(fb_boot.group(2)) // cell[1]):
            raise SystemExit(
                f"{phase}: [FB-GEOMETRY] grid {cells} does not follow {cell}")
        print(f"SNAPSHOT FB-GEOMETRY {fb_status.group(0)}")
        type_line(monitor, "diag snapshot")
        repeat_deadline = time.monotonic() + 30.0
        while time.monotonic() < repeat_deadline:
            repeated = serial_text()[diag_mark:]
            if (repeated.count("DIAG SNAPSHOT records=") >= 2 and
                    repeated.count("[LOGIN-TRACE]") >= 2):
                break
            if "*** HFOC KERNEL PANIC ***" in repeated:
                raise SystemExit(
                    "kernel panic during the repeated ITEM-614 snapshot")
            time.sleep(0.05)
        else:
            raise SystemExit(
                f"{phase}: the repeated `diag snapshot` printed no trace")
        type_line(monitor, "credentials")
        wait_for(["Current password:"], 120.0, "current password prompt")
        wait_echo(monitor, "current-password", original_password, True)
        press(monitor, "ret")
        wait_for(["New login:"], 120.0, "new login prompt")
        wait_echo(monitor, "new-login", login, False)
        press(monitor, "ret")
        wait_for(["New password:"], 120.0, "new password prompt")
        wait_echo(monitor, "new-password", updated_password, True)
        press(monitor, "ret")
        wait_for(
            ["Repeat new password:"], 120.0,
            "new password confirmation prompt")
        wait_echo(monitor, "confirm-password", updated_password, True)
        press(monitor, "ret")
        wait_for(
            ["[ADMIN-ENROLLMENT] credentials changed login="],
            120.0, "password change")

text = serial_text()
if (
    original_password in text
    or updated_password in text
    or wrong_password in text
):
    raise SystemExit(f"{phase}: hidden password reached the serial log")
if phase != "install":
    for line in require_login_trace():
        print("TRACE " + line)
print(f"host-provisioned entropy {phase}: OK")
PY
    then
        print_diagnostics "$phase" "$log"
        return 1
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

# The persistence store is appended after the 512-block state layout.  Reading
# only the public sequence fields proves that each successful login advanced the
# durable epoch before the panel was published.
slot_sequences() {
    python3 - "$DISK" <<'PY'
import pathlib
import struct
import sys

disk = pathlib.Path(sys.argv[1])
with disk.open("rb") as source:
    source.seek(512)
    header = source.read(512)
    entry_lba, entries, entry_size, entry_crc = struct.unpack_from(
        "<QIII", header, 72)
    if entry_size != 128 or entries < 3:
        raise SystemExit("the installed GPT has no persistence entry")
    source.seek(entry_lba * 512 + 2 * entry_size)
    entry = source.read(entry_size)
    first_lba = struct.unpack_from("<Q", entry, 32)[0]
    partition_bytes = struct.unpack_from("<Q", entry, 40)[0] * 512
    base = first_lba * 512 + 512 * 512
    sequences = []
    for slot in range(2):
        source.seek(base + slot * 512)
        record = source.read(512)
        if record[:8] == b"HFOCES01":
            sequences.append(struct.unpack_from("<Q", record, 16)[0])
        elif record == bytes(512):
            sequences.append(0)
        else:
            raise SystemExit(f"slot {slot} is neither valid nor empty")
    if base + 1024 > first_lba * 512 + partition_bytes:
        raise SystemExit("the seed slots are outside the persistence partition")
print(f"{sequences[0]} {sequences[1]}")
PY
}

expect_slots() {
    local expected=$1
    local actual
    actual=$(slot_sequences)
    [[ "$actual" == "$expected" ]] || {
        echo "host seed slots: expected '$expected', observed '$actual'" >&2
        return 1
    }
}

corrupt_seed_tags() {
    python3 - "$DISK" <<'PY'
import pathlib
import struct
import sys

disk = pathlib.Path(sys.argv[1])
with disk.open("r+b") as image:
    image.seek(512)
    header = image.read(512)
    entry_lba, entries, entry_size, entry_crc = struct.unpack_from(
        "<QIII", header, 72)
    if entry_size != 128 or entries < 3:
        raise SystemExit("the installed GPT has no persistence entry")
    image.seek(entry_lba * 512 + 2 * entry_size)
    entry = image.read(entry_size)
    first_lba = struct.unpack_from("<Q", entry, 32)[0]
    base = first_lba * 512 + 512 * 512
    changed = 0
    for slot in range(2):
        offset = base + slot * 512
        image.seek(offset)
        record = bytearray(image.read(512))
        if record[:8] == b"HFOCES01":
            record[128] ^= 0x01
            image.seek(offset)
            image.write(record)
            changed += 1
    if changed == 0:
        raise SystemExit("the persistence store has no sealed slot")
PY
}

run_phase install
expect_slots "1 0"
run_phase boot1
expect_slots "3 2"
run_phase boot2
expect_slots "3 4"
corrupt_seed_tags
run_phase seed-corrupt
expect_slots "3 4"

echo "Host-provisioned entropy QEMU proof: OK"
echo "QEMU proof != physical hardware acceptance: the Toshiba run remains the owner's hardware stage"
