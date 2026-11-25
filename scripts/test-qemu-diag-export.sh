#!/usr/bin/env bash
# ITEM-552: the whole chain, on a machine whose xHCI fails while the boot
# medium is reachable over the independent EHCI path.
#
# ITEM-556: the machine carries two mass-storage devices on the controller - a
# foreign one first, the boot stick second - so the walk's continuation and the
# boot-medium selection are executed by the driver, not assumed.  The rate
# matching hub of the owner's laptop is NOT modelled here (QEMU's usb-hub is a
# full-speed device, not an equivalent); that topology is covered by the host
# walk proof, and this proof is not claimed as an HS-hub proof.
#
# The proof boots `build/diag-export/...-diag-export.iso` from a QEMU EHCI
# usb-storage device (SeaBIOS and Limine read it, exactly as the laptop does)
# and adds a `qemu-xhci` controller whose ENABLE_SLOT command the proof kernel
# never rings a doorbell for: the driver's own wait runs its real budget, the
# real failure is captured, and the machine publishes
# `reason=ENUMERATION_FAILED detail=P1_ENABLE_SLOT_TIMEOUT` - the verdict the
# owner read off the laptop.
#
# It then types the operator's one command (`diag save`), requires the SAVED
# line with a session identifier, stops the guest, and carries the report off
# the resulting image with `scripts/hfoc-diag-read.py` - the same command the
# operator runs on the Debian machine - and requires the extracted file to
# contain the fresh failure snapshot: the `[XHCI-CMD] result=TIMEOUT` record the
# driver captured, the EHCI medium answer, the session END, and the session
# identifier the console printed.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

VERSION_VALUE=$(cat VERSION)
IMAGE=${IMAGE:-build/diag-export/hfoc-os-$VERSION_VALUE-diag-export.iso}
# ITEM-557: the build this proof is supposed to be about.  The media used to be
# trusted blindly, and a stale object tree answered `build=2ef08db50f50` for a
# kernel built from 35b2188d; the build now passes its own identity and the
# proof refuses anything else.
EXPECTED_BUILD_ID=${EXPECTED_BUILD_ID:-}
EXPECTED_VERSION=${EXPECTED_VERSION:-$VERSION_VALUE}
EXPECTED_KERNEL=${EXPECTED_KERNEL:-}
test -f "$IMAGE" || {
    echo "Missing proof medium: $IMAGE (run make diag-export-qemu-media)" >&2
    exit 1
}
if [[ -z $EXPECTED_BUILD_ID ]]; then
    echo "EXPECTED_BUILD_ID is empty: the proof cannot tell which build it boots" >&2
    exit 1
fi
if [[ -n $EXPECTED_KERNEL ]]; then
    test -f "$EXPECTED_KERNEL" || {
        echo "Missing proof kernel: $EXPECTED_KERNEL" >&2
        exit 1
    }
    # One process reads the kernel: grep -q must not terminate a strings
    # producer with SIGPIPE and turn a present identity into refusal (141).
    bash scripts/hfoc-file-marker.sh "$EXPECTED_KERNEL" "$EXPECTED_BUILD_ID" || {
        echo "the proof kernel $EXPECTED_KERNEL does not carry build id $EXPECTED_BUILD_ID" >&2
        exit 1
    }
fi
# A medium that already carries a session would let the extractor answer with a
# report this run did not write: the freshness check below would then be about
# another boot.  The reserved journal starts empty in a freshly built medium -
# read through the same reader the Debian extractor uses, not through a grep of
# the image (the kernel's own format strings contain the marker text).
journal_json=$(python3 scripts/hfoc-install-log-read.py --json "$IMAGE") || {
    echo "the proof medium $IMAGE has no readable reserved journal" >&2
    exit 1
}
JOURNAL_JSON=$journal_json python3 - <<'PY' || exit 1
import json
import os
import sys

state = json.loads(os.environ["JOURNAL_JSON"])
if state.get("records"):
    raise SystemExit(
        "the proof medium already carries %d record(s)" % len(state["records"]))
if int(state.get("slots", 0)) > 1:
    raise SystemExit(
        "the proof medium already carries a journal stream (%s slots)"
        % state.get("slots"))
PY

TEST_DIR=$(mktemp -d)
STICK=$TEST_DIR/stick.img
FOREIGN=$TEST_DIR/foreign.img
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
REPORT_DIR=$TEST_DIR/report
QEMU_PID=
: >"$SERIAL_LOG"
: >"$QEMU_LOG"
cp "$IMAGE" "$STICK"
mkdir -p "$REPORT_DIR"
# ITEM-556: a second mass-storage device the machine does not boot from (the
# owner's laptop has one: a card reader on the same controller).  Its MBR disk
# id differs from the boot medium's, so the guard answers NO_MATCH.
python3 - "$FOREIGN" <<'PYEOF'
import struct
import sys

image = bytearray(1024 * 1024)
image[440:444] = struct.pack("<I", 0x11111111)
image[446:462] = bytes([0x80, 0, 0, 0, 0x0B, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0])
image[510:512] = b"\x55\xaa"
with open(sys.argv[1], "wb") as handle:
    handle.write(image)
PYEOF

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/diag-export-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        cp -a "$SERIAL_LOG" "$QEMU_LOG" "$artifact_dir/" 2>/dev/null || true
        cp -a "$REPORT_DIR"/. "$artifact_dir/" 2>/dev/null || true
        echo "diag-export artifacts: $artifact_dir" >&2
    fi
    if [[ -n "$QEMU_PID" ]]; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# One persistent monitor connection: a connection that is opened, written to
# and closed at once can lose the command before QEMU has parsed it (the first
# two runs of this proof did exactly that, and the refusal page stayed up).
monitor_commands() {
    python3 - "$MONITOR_SOCKET" "$@" <<'PY'
import socket
import sys
import time

deadline = time.monotonic() + 10.0
while True:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        sock.connect(sys.argv[1])
        break
    except OSError:
        if time.monotonic() > deadline:
            raise SystemExit(0)
        time.sleep(0.2)
buffer = b""
for command in sys.argv[2:]:
    sock.sendall(command.encode("ascii") + b"\n")
    waited = time.monotonic() + 2.0
    while time.monotonic() < waited:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buffer += chunk
        if b"(qemu)" in buffer:
            buffer = b""
            break
    time.sleep(0.05)
sock.close()
PY
}

serial_text() {
    python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    print(module.normalise(pathlib.Path(sys.argv[1]).read_bytes()
                           .decode("ascii", errors="replace")))
except FileNotFoundError:
    print("")
PY
}

wait_for() {
    local marker=$1
    local budget=${2:-90}
    python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" "$marker" \
        "$budget" <<'PY'
import importlib.util
import pathlib
import sys
import time

spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
path = pathlib.Path(sys.argv[1])
marker = sys.argv[3]
deadline = time.monotonic() + float(sys.argv[4])
while time.monotonic() < deadline:
    try:
        text = module.normalise(path.read_bytes().decode("ascii",
                                                         errors="replace"))
    except FileNotFoundError:
        text = ""
    if marker in text:
        raise SystemExit(0)
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic before the marker")
    time.sleep(0.25)
# ITEM-412: a proof that gives up on a guest marker declares the class, so the
# suite can tell a timing failure from a failed assertion.
sys.stderr.write(
    'HFOC-QEMU-MARKER-TIMEOUT marker="' + marker + '" deadline='
    + str(int(float(sys.argv[4]))) + 's\n')
sys.stderr.flush()
raise SystemExit(f"timed out waiting for {marker!r}")
PY
}

echo "diag-export proof: booting $IMAGE from EHCI with a failing xHCI"
qemu-system-x86_64 \
    -machine pc -m 512M -smp 1 -display none -no-reboot \
    -drive "file=$STICK,format=raw,if=none,id=stick" \
    -drive "file=$FOREIGN,format=raw,if=none,id=foreign" \
    -device usb-ehci,id=ehci \
    -device usb-storage,drive=foreign,bus=ehci.0,port=1 \
    -device usb-storage,drive=stick,bootindex=0,bus=ehci.0,port=2 \
    -device qemu-xhci,id=xhci \
    -device usb-mouse,bus=xhci.0 \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

# The machine must diagnose the xHCI and see the medium on EHCI.  The failure
# snapshot itself is not on the serial stream: the driver publishes it through
# the journal funnel (that is what makes it survive the boot), so it is required
# from the *extracted report* below - which is exactly the claim this proof
# exists to make.
wait_for "[XHCI] state=UNAVAILABLE" 120
wait_for "ENABLE_SLOT_TIMEOUT" 120
wait_for "[INSTALL-MEDIUM] controller=ehci " 120
# The foreign device must be reached, attached and rejected first, and the walk
# must continue to the boot stick on the second port.
wait_for "state=BOUND reason=NOT_MEDIUM" 120
wait_for "state=NO_MATCH" 120
wait_for "state=MATCH reason=OK" 120

# The console's own prompt is printed before the installer flow finishes; the
# machine then paints the refusal page of a machine with no disk and waits for a
# key.  Dismiss it first, or the operator's command would be eaten by the
# "press any key" screen - which is exactly what the first run of this proof
# showed, and why the wait is on the page's own last line.
wait_for "Press any key to continue." 120
monitor_commands "sendkey ret"
sleep 2
keys=()
for key in d i a g spc s a v e ret; do
    keys+=("sendkey $key")
done
monitor_commands "${keys[@]}"

saved_line=$(python3 - "$SERIAL_LOG" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import importlib.util
import pathlib
import re
import sys
import time

spec = importlib.util.spec_from_file_location("hfoc_serial_view", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
path = pathlib.Path(sys.argv[1])
deadline = time.monotonic() + 90.0
pattern = re.compile(r"DIAG SAVED id=([0-9A-F]{8}) records=(\d+)")
while time.monotonic() < deadline:
    try:
        text = module.normalise(path.read_bytes().decode("ascii",
                                                         errors="replace"))
    except FileNotFoundError:
        text = ""
    match = pattern.search(text)
    if match is not None:
        print(match.group(0))
        raise SystemExit(0)
    if "DIAG FAILED" in text:
        for line in text.splitlines():
            if "DIAG FAILED" in line:
                raise SystemExit("save refused: " + line)
    time.sleep(0.25)
sys.stderr.write(
    'HFOC-QEMU-MARKER-TIMEOUT marker="DIAG SAVED" deadline=90s\n')
sys.stderr.flush()
raise SystemExit("timed out waiting for DIAG SAVED")
PY
)
echo "diag-export proof: $saved_line"
session=$(printf '%s\n' "$saved_line" | sed -n 's/^DIAG SAVED id=\([0-9A-F]\{8\}\).*/\1/p')

monitor_commands quit
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

extract_output=$(python3 scripts/hfoc-diag-read.py --disk "$STICK" \
    --output "$REPORT_DIR" 2>&1) || {
    echo "diag-export proof: extraction failed: $extract_output" >&2
    exit 1
}
printf '%s\n' "$extract_output"
report=$(find "$REPORT_DIR" -name "HFOC-DIAG-*.txt" -print -quit)
test -n "$report" || {
    echo "diag-export proof: no report file was written" >&2
    exit 1
}
test -f "$report" || {
    echo "diag-export proof: the reported path $report does not exist" >&2
    exit 1
}
grep -q "HFOC-DIAG-${session}.txt" <<<"$report" || {
    echo "diag-export proof: the report is not named after session $session" >&2
    exit 1
}
# ITEM-573: the controller the mouse was plugged into is refused and its
# session released, so the extracted file must carry the durable evidence -
# the controller's start verdict and both raw PORTSC words of its ports - and
# the pointer's own verdict, not just the `[XHCI-CMD]` failure of one command.
for marker in \
    "[DIAG] id=${session} state=BEGIN" \
    "[DIAG] medium ehci=" \
    "[EHCI] pci=" \
    "[XHCI-CMD] result=TIMEOUT" \
    "[XHCI-CTRL] pci=" \
    "[XHCI-PORT] pci=" \
    "[XHCI-MOUSE] verdict=" \
    "[NET-STATUS] device=" \
    "[NET-ADDR] " \
    "[PANEL] " \
    "[RNG] source=" \
    "[ADMIN] " \
    "[XHCI-ROUTE] pci=" \
    "[XHCI-LEGACY] pci=" \
    "state=END records=" \
    "[DIAG] id=${session} state=END"; do
    grep -Fq -- "$marker" "$report" || {
        echo "diag-export proof: the extracted report lacks $marker" >&2
        echo "----- report -----" >&2
        cat "$report" >&2
        exit 1
    }
done
# The evidence record must carry the port's own two words and the controller's
# verdict: a record that only names the port is not the preserved evidence.
# ITEM-581: this medium is the diagnostic console profile, which carries no
# production network or TLS owners.  The export must still carry all five
# records, in the same field shape the installed system prints, with the
# reason naming the missing plane - a record that is simply absent would be
# the silence the installed Toshiba produced.
grep -Eq '^\[NET-STATUS\] device=[^ ]+ readiness=[^ ]+ backend=[^ ]+ link=(UP|DOWN) mode=[^ ]+ stage=[0-9]+' "$report" || {
    echo "diag-export proof: no [NET-STATUS] record names the live network" >&2
    grep -F '[NET-STATUS]' "$report" >&2 || true
    exit 1
}
grep -Fq 'reason=PROFILE_NO_PRODUCTION_NETWORK' "$report" || {
    echo "diag-export proof: no record names why this medium has no network" >&2
    grep -F '[NET-' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[NET-ADDR\] state=[^ ]+ lease=(ACQUIRED|NONE) ' "$report" || {
    echo "diag-export proof: no [NET-ADDR] record names the addressing state" >&2
    grep -F '[NET-ADDR]' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[NET-ADDR\].* attempt=[0-9]+/[0-9]+' "$report" || {
    echo "diag-export proof: no [NET-ADDR] record names the DHCP attempt" >&2
    grep -F '[NET-ADDR]' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[RNG\] source=[^ ]+ state=[^ ]+ stage=[^ ]+ cpuid_max=[0-9]+ rdrand=[01] rdseed=[01]' "$report" || {
    echo "diag-export proof: no [RNG] record names the CPU profiles" >&2
    grep -F '[RNG]' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[PANEL\] v=[01] state=[^ ]+' "$report" || {
    echo "diag-export proof: no [PANEL] record names the serving gate" >&2
    grep -F '[PANEL]' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[ADMIN\] state=[^ ]+ last=[^ ]+' "$report" || {
    echo "diag-export proof: no [ADMIN] record names the administrator gate" >&2
    grep -F '[ADMIN]' "$report" >&2 || true
    exit 1
}
grep -Eq '^\[XHCI-PORT\] pci=[^ ]+ port=[0-9]+ was=0x[0-9A-F]+ now=0x[0-9A-F]+ ' "$report" || {
    echo "diag-export proof: no [XHCI-PORT] record carries was= and now=" >&2
    cat "$report" >&2
    exit 1
}
grep -Eq '^\[XHCI-CTRL\] pci=[^ ]+ state=(LIVE|RELEASED) status=[A-Z_]+ ' "$report" || {
    echo "diag-export proof: no [XHCI-CTRL] record carries its state and status" >&2
    cat "$report" >&2
    exit 1
}
# ITEM-574: the routing stage and the BIOS/OS handoff must be in the file even
# though QEMU's controller is not Intel: the registers are read, the stage says
# why it did not write, and the shared connect window is named.
grep -Eq '^\[XHCI-ROUTE\] pci=[^ ]+ phase=BEFORE read=(OK|FAIL) xusb2pr=0x[0-9A-F]+ ' "$report" || {
    echo "diag-export proof: no [XHCI-ROUTE] BEFORE record names the registers" >&2
    cat "$report" >&2
    exit 1
}
grep -Eq '^\[XHCI-ROUTE\] pci=[^ ]+ phase=AFTER read=(OK|FAIL) xusb2pr=0x[0-9A-F]+ .* action=[A-Z_]+ ' "$report" || {
    echo "diag-export proof: no [XHCI-ROUTE] AFTER record names the verdict" >&2
    cat "$report" >&2
    exit 1
}
# ITEM-577: the exported file must carry the DMA geometry - the controller's
# HCSPARAMS2, its scratchpad count, the record size, the 32-record slot area and
# the DCBAA/DCBAA[0] addresses - so the next hardware run can tell a refused
# scratchpad from a slot-layout bug without another guess.
grep -Eq '^\[XHCI-DMA\] pci=[^ ]+ hcsparams2=0x[0-9A-F]+ spb=[0-9]+ page=[0-9]+ ctx=(32|64) slot=(1024|2048) pool=[0-9]+ dcbaa=0x[0-9A-F]{16} dcbaa0=0x[0-9A-F]{16}' "$report" || {
    echo "diag-export proof: no [XHCI-DMA] record names the pool geometry" >&2
    cat "$report" >&2
    exit 1
}
grep -Fq 'spb=0 page=0 ctx=32 slot=1024' "$report" || {
    echo "diag-export proof: the QEMU controller's geometry changed" >&2
    cat "$report" >&2
    exit 1
}
grep -Fq 'action=SKIP_NOT_INTEL' "$report" || {
    echo "diag-export proof: a non-Intel controller was not left unprogrammed" >&2
    cat "$report" >&2
    exit 1
}
grep -Eq '^\[XHCI-LEGACY\] pci=[^ ]+ cap=0x[0-9A-F]+ (bios=0x[0-9A-F]+ )?result=(TAKEN|TIMEOUT|NOT_OWNED|ABSENT)' "$report" || {
    echo "diag-export proof: no [XHCI-LEGACY] record names the handoff result" >&2
    cat "$report" >&2
    exit 1
}
grep -Eq '^\[XHCI-CTRL\] pci=[^ ]+ .* settle_ms=[0-9]+ settle_bound_ms=[0-9]+ settle_pending=[01]' "$report" || {
    echo "diag-export proof: the controller record does not name the shared connect window" >&2
    cat "$report" >&2
    exit 1
}
# ITEM-573: the mouse is on a port of the refused controller.  The extracted
# file must say the device was detected and name the port and the stage - a
# record that only says NOT_DETECTED would hide the very thing the run is for.
grep -Eq '^\[XHCI-MOUSE\] verdict=SEEN detail=[^ ]+ seen=1 bound=[01] controller=[0-9A-F]{2}:[0-9A-F]{2}\.[0-9] port=[1-9]' "$report" || {
    echo "diag-export proof: the extracted [XHCI-MOUSE] does not name the detected device" >&2
    grep -F '[XHCI-MOUSE]' "$report" >&2 || true
    exit 1
}
echo "diag-export proof: $report carries session $session and the fresh failure snapshot"

# ITEM-557: a report is fresh only when it names the build that wrote it and the
# session this run just saved.  An old session on the medium, or a kernel of
# another revision (a stale object tree, a different medium), fails here instead
# of being carried to Debian as "the" answer.
grep -Fq "[DIAG] id=${session} state=BEGIN build=${EXPECTED_BUILD_ID} version=${EXPECTED_VERSION} " "$report" || {
    echo "diag-export proof: the report does not name build ${EXPECTED_BUILD_ID} version ${EXPECTED_VERSION}" >&2
    grep -F "state=BEGIN" "$report" >&2 || true
    exit 1
}
grep -q "^# session: ${session}$" "$report" || {
    echo "diag-export proof: the report header does not name session $session" >&2
    exit 1
}
begins=$(grep -cF '[DIAG] id=' "$report" || true)
begin_markers=$(grep -cF 'state=BEGIN' "$report" || true)
end_markers=$(grep -cF 'state=END' "$report" || true)
sessions=$(grep -oF 'state=BEGIN' "$report" | wc -l)
if [[ $begin_markers -ne 1 || $end_markers -ne 1 || $sessions -ne 1 ]]; then
    echo "diag-export proof: the report carries ${begin_markers} BEGIN and ${end_markers} END records (one session expected, ${begins} DIAG lines)" >&2
    exit 1
fi
if grep -F '[DIAG] id=' "$report" | grep -vF "id=${session} " >/dev/null; then
    echo "diag-export proof: the report mixes session $session with another one" >&2
    exit 1
fi
reported_records=$(grep -F "state=END" "$report" | sed -n 's/.*records=\([0-9]*\).*/\1/p' | head -1)
saved_records=$(printf '%s\n' "$saved_line" | sed -n 's/.*records=\([0-9]*\).*/\1/p')
if [[ -n ${saved_records:-} && ${reported_records:-} != "$saved_records" ]]; then
    echo "diag-export proof: the report's END counts ${reported_records:-?} records, the console saved $saved_records" >&2
    exit 1
fi
echo "diag-export proof: the report names build $EXPECTED_BUILD_ID and only session $session"
# Keep the example the report really produced, so a reader can see the file the
# operator would have carried off the machine.
mkdir -p build/test-artifacts
cp "$report" build/test-artifacts/diag-export-last-report.txt
echo "diag-export proof: example kept at build/test-artifacts/diag-export-last-report.txt"
echo "DIAG-EXPORT qemu proof: OK"
