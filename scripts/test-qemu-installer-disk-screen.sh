#!/usr/bin/env bash
# ITEM-490, ITEM-495: the operator names the disk, and confirms on the screen,
# through the real UEFI path, in QEMU/OVMF.
#
# The item's rule is that the *operator* entry of the install medium shows the
# storage screen, waits for a person, and writes the disk that person named -
# not the disk the medium declared.  ITEM-495 adds the two things that gesture
# was missing: the screen shows the name the hardware reported for each disk
# ("QEMU HARDDISK"), and choosing a disk draws a warning page that says
# everything on it will be deleted - the page ENTER accepts.  The console token
# of ITEM-394 is gone: nothing is typed at the console any more.
#
# This proof boots the medium built for a human
# (`production-installer-console-media`, operator entry first) with two real
# (virtual) AHCI disks and drives the whole gesture through the emulated
# keyboard:
#
#   1. the medium's first Limine entry is the operator one (read out of the ISO,
#      not assumed);
#   2. the screen lists both disks by the name and the identity the hardware
#      reported, with the capacity, the transport and the hint the operator
#      reads;
#   3. DOWN + ENTER chooses the *second* disk - the one the medium did not
#      declare - and the reading says so (`selected=2 state=SELECTABLE`);
#   4. that ENTER draws the warning page, which names the disk and says that
#      everything on it will be erased, and writes nothing (`writes=0`);
#   5. ITEM-514 makes the gesture disk -> scheme -> (erase) warning -> summary:
#      ENTER on the summary page installs, and the records name the chosen disk
#      and the operator's scheme:
#      `confirm source=operator-screen disk=2 identity=ITEM490-CHOSEN-DISK`,
#      then `complete disk=2 identity=ITEM490-CHOSEN-DISK`;
#   6. the other disk is byte-identical afterwards: the operator's choice
#      replaced the declaration, and the declaration's disk was never touched;
#   7. Q on the warning page goes back to the list without confirming, and Q on
#      the list refuses the run with `CANCELLED_BY_OPERATOR writes=0` - neither
#      disk changes, and no install record is published.
#
# ITEM-500 adds the machine the medium was not declared for:
#
#   8. the same medium booted on a machine whose disks are neither the declared
#      serial nor each other: the screen still opens, says `Declared disk
#      QM00001 is not on this machine - choose a disk`, the record says
#      `declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE writes=0`, and the
#      operator's choice installs the disk this machine actually has;
#   9. an operator entry that cannot install at all says why on the machine's
#      own screen and waits for a key: here the medium boots on a legacy-BIOS
#      machine that carries no disk at all, so the plan refuses `NO_DISK`
#      before the storage screen can open.  ITEM-518 makes that page print the
#      observed storage - the controllers the class scan matched, the disks it
#      brought up, and the reason a disk cannot be used - so `NO_DISK` stops
#      being a code the person at the machine cannot act on, and the same facts
#      are published as one `[STORAGE-REPORT]` record;
#  10. ITEM-517: the same operator entry on a legacy-BIOS machine that *does*
#      carry disks opens the screen, reaches the scheme list and the summary,
#      and the summary names the BIOS roles (BIOS boot partition, FAT boot
#      volume) and promises no EFI System Partition; leaving there writes
#      nothing, exactly as leaving on UEFI does.
#
# ITEM-542 proves the ITEM-541 refusal-page facts on the machine itself, in the
# two shapes F-7 left indistinguishable:
#
#  11. the machine carries no USB medium at all (the boot medium is the optical
#      disc): the page must say `[INSTALL-MEDIUM] state=UNAVAILABLE
#      reason=NO_DEVICE` and `[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY`,
#      and print the bounded ring tail;
#  12. the machine boots the operator medium itself from a USB stick whose
#      reserved journal frame is damaged: the medium verdict is `state=MATCH
#      reason=OK` while the journal is `state=UNAVAILABLE reason=HEADER`, so
#      "the kernel did not see the stick" and "the stick is here but its
#      journal did not bind" read differently on the page.
#
# The page's own section lines carry the machine records' prefixes, so both
# cases are read through one shape-based probe: a record is selected by the
# fields only a record has (`pci=`, `scope=`, `loader_id=`) and the page's
# journal line by the counters it appends (`pending=`), and the tail is bounded
# and counted on the machine (`shown=N`, N records, then the way out).
#
# This is a firmware proof on virtual hardware:
# QEMU/OVMF proof != physical hardware acceptance, and a person choosing a disk
# on a real machine remains the item's physical acceptance stage.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate cmp xorriso sha256sum; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_ITEM490_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ITEM500_TIMEOUT:-1800}s" \
        env HFOC_ITEM490_INNER=1 "$0" "$@"
fi

VERSION=$(<VERSION)
IMAGE=${IMAGE:-build/production-installer/hfoc-os-$VERSION-production-installer-console.iso}
OVMF=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
# ITEM-495: the name QEMU's emulated ATA disks report, which the screen must
# show beside the identity.  It is what the proof asserts the operator read.
MODEL=${HFOC_ITEM495_MODEL:-QEMU HARDDISK}
# The declared disk is the one the medium names (QEMU's own ATA serial); the
# chosen disk carries a serial this proof sets, so the two are distinguishable
# in the screen and in the records without guessing QEMU's numbering (the CD-ROM
# takes one of those numbers).
DECLARED=${HFOC_ITEM490_DECLARED:-QM00001}
CHOSEN=${HFOC_ITEM490_CHOSEN:-ITEM490-CHOSEN-DISK}
# ITEM-500: the machine the medium was not declared for.  Neither disk carries
# the declared serial, so the run has to ask the operator instead of refusing.
FOREIGN_A=${HFOC_ITEM500_FOREIGN_A:-ITEM500-FOREIGN-A}
FOREIGN_B=${HFOC_ITEM500_FOREIGN_B:-ITEM500-FOREIGN-B}
QEMU_MACHINE=${QEMU_MACHINE:-q35}
DISK_BYTES=${HFOC_ITEM490_DISK_BYTES:-4294967296}
# ITEM-517: which firmware a case boots.  `uefi` (the default) keeps the OVMF
# pflash drives every pre-item case used; `bios` omits them, so QEMU runs
# SeaBIOS - the firmware of the machine this item is for.
CASE_FIRMWARE=${CASE_FIRMWARE:-uefi}

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

# ------------------------------------------------------ the medium is a human's --
# The operator entry is the first entry, so a machine booted from this medium
# lands on the storage screen without anybody navigating a menu.
xorriso -osirrox on -indev "$IMAGE" -extract /boot/limine/limine.conf \
    "$TEST_DIR/limine.conf" >/dev/null 2>&1 || {
    echo "The medium carries no boot configuration" >&2
    exit 1
}
first_mode=$(awk '
    /^\// { ++entries; if (entries == 1) { in_first = 1 } else { in_first = 0 } }
    in_first && /cmdline:/ { print $2; exit }
' "$TEST_DIR/limine.conf")
[[ "$first_mode" == "hfoc.install=console" ]] || {
    echo "The medium's first entry is $first_mode, not the operator entry" >&2
    exit 1
}
echo "medium: the operator entry ($first_mode) is the default"

# run_case <label> <keys> [extra python assertions...]
# ITEM-500: CASE_A_SERIAL / CASE_B_SERIAL name the two disks of the machine the
# medium is booted on, so the same function proves the declared machine and the
# machine that does not carry the declared disk.
run_case() {
    local label=$1
    local keys=$2
    shift 2
    local log=$TEST_DIR/$label.log
    local socket=$TEST_DIR/$label.monitor
    local a_serial=${CASE_A_SERIAL:-$DECLARED}
    local b_serial=${CASE_B_SERIAL:-$CHOSEN}
    local pid
    # ITEM-517: a BIOS case boots the same machine without the OVMF pflash
    # drives; every UEFI case passes exactly the two drives it always did.
    local firmware_arguments=()
    if [[ "$CASE_FIRMWARE" == uefi ]]; then
        firmware_arguments=(
            -drive
            "if=pflash,format=raw,readonly=on,file=$OVMF")
    fi

    : >"$log"
    # ITEM-500: every IDE device is placed explicitly.  The proof names the
    # serial of each disk now, and q35's AHCI buses carry one unit each, so
    # letting QEMU place two of them on the same bus is a device conflict
    # rather than a proof failure.  The first disk stays on the lowest port so
    # discovery numbers it 1 and the second disk is the row the proof's DOWN
    # selects - the order the screen and the records are asserted in.
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 4096M -smp 1 \
        -display none \
        -drive "file=$IMAGE,if=none,id=item500-cd,format=raw,media=cdrom,readonly=on" \
        -device "ide-cd,drive=item500-cd,bus=ide.2,unit=0" \
        "${firmware_arguments[@]}" \
        -drive "file=$TEST_DIR/$label-a.img,if=none,id=item490-disk-a,format=raw" \
        -device "ide-hd,drive=item490-disk-a,bus=ide.0,unit=0,serial=$a_serial" \
        -drive "file=$TEST_DIR/$label-b.img,if=none,id=item490-disk-b,format=raw" \
        -device "ide-hd,drive=item490-disk-b,bus=ide.1,unit=0,serial=$b_serial" \
        -serial "file:$log" \
        -monitor "unix:$socket,server,nowait" \
        -no-reboot -no-shutdown \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    pid=$!

    if ! python3 - "$log" "$socket" "$label" "$keys" "$DECLARED" "$CHOSEN" \
        "$MODEL" "$a_serial" "$b_serial" "$@" \
        "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
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

log = pathlib.Path(sys.argv[1])
socket_path = sys.argv[2]
label = sys.argv[3]
keys = sys.argv[4]
declared = sys.argv[5]
chosen = sys.argv[6]
model = sys.argv[7]
a_serial = sys.argv[8]
b_serial = sys.argv[9]
# ITEM-542: the shared renderer's path is the last argument, so the caller's
# own assertions stop before it.  ITEM-533 appended the path but left this
# slice open, which made every case assert the path itself as a missing record;
# the standalone proof is not in CI, so only a run catches it.
extra = sys.argv[10:-1]

# The screen the operator reads, and what the run must say afterwards.
required = {
    "choose": [
        "Select installation disk",
        declared,
        chosen,
        model,
        "W/S or arrow keys move",
        "State: SELECTABLE",
    ],
    "cancel": [
        "Select installation disk",
        declared,
        chosen,
        model,
        "W/S or arrow keys move",
    ],
    # ITEM-500: the machine does not carry the declared disk, so the screen
    # names the reason and lists the disks it does have.
    "foreign": [
        "Select installation disk",
        b_serial,
        model,
        "Declared disk " + declared + " is not on this machine - choose a disk",
        "State: SELECTABLE",
    ],
    # ITEM-517: the same operator entry on a legacy-BIOS machine.  Nothing
    # refuses any more, so the case walks the operator's own path: the screen
    # lists both disks, naming the second opens the scheme list, ENTER accepts
    # the destructive scheme and its warning page, and ENTER again draws the
    # summary the operator would confirm.  Q there and Q on the list leave
    # without writing.
    #
    # This first wait is the disk screen and only the disk screen: the scheme
    # list opens on the operator's DOWN+ENTER, which this case sends after the
    # wait returns.  Requiring the scheme markers here meant the wait could
    # never be satisfied - the proof deadlocked on its own first assertion and
    # never reached the BIOS walk (nor the ITEM-518 refusal case after it).
    "bios": [
        "Select installation disk",
        declared,
        chosen,
        model,
        "W/S or arrow keys move",
        "State: SELECTABLE",
    ],
}

KEY_NAMES = {
    " ": "spc",
    "-": "minus",
    "=": "equal",
    "/": "slash",
    ".": "dot",
    "_": "shift-minus",
}
for _digit in "0123456789":
    KEY_NAMES[_digit] = _digit


def body():
    try:
        return SERIAL_VIEW.normalise(
            log.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def wait_for(markers, seconds, what):
    deadline = time.monotonic() + seconds
    missing = list(markers)
    while time.monotonic() < deadline:
        text = body()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit(f"{label}: the kernel panicked while waiting for {what}")
        missing = [marker for marker in markers if marker not in text]
        if not missing:
            return
        time.sleep(0.1)
    raise SystemExit(f"{label}: {what} never appeared: {missing}")


def open_monitor():
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        if pathlib.Path(socket_path).exists():
            try:
                monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                monitor.settimeout(5.0)
                monitor.connect(socket_path)
                received = b""
                while b"(qemu)" not in received:
                    chunk = monitor.recv(4096)
                    if not chunk:
                        raise ConnectionError("the QEMU monitor closed")
                    received += chunk
                return monitor
            except OSError:
                time.sleep(0.1)
                continue
        time.sleep(0.1)
    raise SystemExit(f"{label}: the QEMU monitor never answered")


def key(monitor, name):
    monitor.sendall(("sendkey " + name + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit(f"{label}: the QEMU monitor closed unexpectedly")
        received += chunk
    time.sleep(0.08)


def press(monitor, name):
    key(monitor, name)


def type_text(monitor, text):
    """Type a lower-case/digit/dot/dash line one key at a time."""
    for character in text:
        if character == " ":
            press(monitor, "spc")
        elif character == "-":
            press(monitor, "minus")
        elif character == ".":
            press(monitor, "dot")
        else:
            press(monitor, character)


wait_for(required[label], 240.0, "the storage screen")
print(f"{label}: the storage screen listed both disks by identity")

monitor = open_monitor()

if keys == "choose":
    press(monitor, "down")
    press(monitor, "ret")
    wait_for(
        ["[INSTALLER-STORAGE] disks=2 selectable=2 selected=2"],
        60.0,
        "the reading of the operator's choice",
    )
    print(f"{label}: the operator chose disk 2 ({b_serial}), not the declared {declared}")
    # ITEM-514: naming the disk opens the scheme screen.  The disk is blank, so
    # the discovered default is "erase the whole disk" and the cursor starts
    # there; ENTER draws the ITEM-495 destructive page for exactly that scheme.
    wait_for(
        [
            "Choose the installation scheme",
            "Erase the whole disk and install HFOC",
            "[PRODUCTION-INSTALL] pending source=console",
        ],
        120.0,
        "the scheme screen for the chosen disk",
    )
    print(f"{label}: the scheme screen offered the four schemes and chose nothing")
    press(monitor, "ret")
    # The destructive scheme is read before it is chosen: the page names the
    # disk and the record still says nothing has been written.
    wait_for(
        [
            "WARNING: EVERYTHING ON THIS DISK WILL BE DELETED",
            f"  Disk: {model}",
        ],
        120.0,
        "the erase warning page for the chosen disk",
    )
    print(f"{label}: the erase page named the disk and nothing was written")
    press(monitor, "ret")
    # ITEM-514: the summary is the last page before the write.  It names the
    # plan the writer will execute - the whole disk goes, the four roles are
    # created - and ENTER here is the confirmation the old disk warning carried.
    wait_for(
        [
            "Confirm the installation plan",
            "Will be deleted: everything on this disk",
            "ENTER confirm this plan",
        ],
        120.0,
        "the summary page",
    )
    print(f"{label}: the summary named the plan before the write")
    press(monitor, "ret")
    # ITEM-506 increment 4: the account screen follows the accepted plan.
    # The operator sets the login and the password here, without echo; the
    # install then writes the record and promises admin=SET only after it read
    # the record back.
    account_login = "installer1"
    account_password = "hfocinstallpass"
    wait_for(["New login:"], 60.0, "the installer account screen")
    type_text(monitor, account_login)
    press(monitor, "ret")
    wait_for(["New password:"], 60.0, "the installer account password")
    type_text(monitor, account_password)
    press(monitor, "ret")
    wait_for(["Repeat new password:"], 60.0, "the installer account confirmation")
    type_text(monitor, account_password)
    press(monitor, "ret")
    wait_for(
        [
            f"[PRODUCTION-INSTALL] admin=SET login={account_login} "
            "persisted=VERIFIED"
        ],
        120.0,
        "the committed operator account",
    )
    print(f"{label}: the operator set the account on the install screen")
    wait_for(
        [
            "[PRODUCTION-INSTALL] confirm source=operator-screen",
            f" disk=2 identity={b_serial}",
            f"[PRODUCTION-INSTALL] complete disk=2 identity={b_serial}",
        ],
        600.0,
        "the install of the operator's disk",
    )
    if account_password in body():
        raise SystemExit(
            f"{label}: the hidden account password reached the serial log"
        )
    print(f"{label}: the run installed the disk the operator confirmed")
elif keys == "bios-walk":
    # ITEM-517: the legacy-BIOS operator walk.  The old SeaBIOS case asserted a
    # refusal that no longer exists, so this case proves the opposite: the BIOS
    # console entry opens the same screen and reaches the last page before the
    # write - the summary - and that summary describes the BIOS plan, not a
    # UEFI one.  Q there goes back to the schemes and Q on the list cancels,
    # with both disks byte-identical afterwards.
    press(monitor, "down")
    press(monitor, "ret")
    wait_for(
        ["[INSTALLER-STORAGE] disks=2 selectable=2 selected=2"],
        60.0,
        "the BIOS reading of the operator's choice",
    )
    print(f"{label}: the BIOS screen accepted the operator's disk 2")
    wait_for(
        [
            "Choose the installation scheme",
            "Erase the whole disk and install HFOC",
            "[PRODUCTION-INSTALL] pending source=console",
        ],
        120.0,
        "the BIOS scheme screen",
    )
    press(monitor, "ret")
    wait_for(
        [
            "WARNING: EVERYTHING ON THIS DISK WILL BE DELETED",
            f"  Disk: {model}",
        ],
        120.0,
        "the BIOS erase warning page",
    )
    press(monitor, "ret")
    wait_for(
        [
            "Confirm the installation plan",
            "Will be deleted: everything on this disk",
            "BIOS boot partition",
            "FAT boot volume",
            "ENTER confirm this plan",
        ],
        120.0,
        "the BIOS summary page",
    )
    if "EFI system partition: created" in body() or \
            "EFI system partition: reused" in body():
        raise SystemExit(f"{label}: the BIOS summary promises an ESP")
    print(f"{label}: the BIOS summary named the BIOS roles and no ESP")
    press(monitor, "q")
    wait_for(
        ["Choose the installation scheme"],
        120.0,
        "the scheme list after leaving the BIOS summary",
    )
    press(monitor, "q")
    wait_for(
        ["[PRODUCTION-INSTALL] refused reason=CANCELLED_BY_OPERATOR writes=0"],
        120.0,
        "the BIOS cancellation record",
    )
    if "complete disk=" in body():
        raise SystemExit(f"{label}: a BIOS walk that cancelled installed something")
    print(f"{label}: the BIOS walk wrote nothing")
else:
    # ITEM-514: Q on the destructive page is "not this one", not "not at all" -
    # it goes back to the scheme list with nothing confirmed - and Q on the list
    # refuses the run.  Both are proven here, because the difference between
    # them is the difference between leaving and confirming.
    press(monitor, "down")
    press(monitor, "ret")
    wait_for(
        ["Choose the installation scheme"],
        120.0,
        "the scheme screen before going back",
    )
    press(monitor, "ret")
    wait_for(
        ["WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"],
        120.0,
        "the erase warning page before going back",
    )
    press(monitor, "q")
    time.sleep(1.0)
    if "[PRODUCTION-INSTALL] confirm" in body():
        raise SystemExit(f"{label}: going back from the erase warning confirmed it")
    press(monitor, "q")
    wait_for(
        ["[PRODUCTION-INSTALL] refused reason=CANCELLED_BY_OPERATOR writes=0"],
        120.0,
        "the cancellation record",
    )
    if "disk=2 identity=" in body() and "complete disk=" in body():
        raise SystemExit(f"{label}: a cancelled run installed something")
    print(f"{label}: leaving the screen wrote nothing")

for assertion in extra:
    if assertion.startswith("absent:"):
        needle = assertion[len("absent:") :]
        if needle in body():
            raise SystemExit(f"{label}: unexpected record {needle}")
    else:
        if assertion not in body():
            raise SystemExit(f"{label}: missing record {assertion}")
PY
    then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "installer disk screen proof ($label): FAILED" >&2
        # The tail of a serial log is the timer stream; the records are what a
        # reader needs, so print the lines that carry them.
        grep -aE 'PRODUCTION-INSTALL|INSTALLER-STORAGE|STORAGE-REPORT|Observed storage|controllers:|disks:|Select installation|Declared disk|Installation refused|Reason: ' \
            "$log" | tail -40 >&2 || true
        exit 1
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "installer disk screen proof ($label): OK"
}

# A pristine copy of each disk, so "the other disk was not touched" is measured,
# not assumed.
fresh_disks() {
    truncate -s "$DISK_BYTES" "$TEST_DIR/$1-a.img"
    truncate -s "$DISK_BYTES" "$TEST_DIR/$1-b.img"
}

# refusal_page_probe <label> <log> <reason> <medium state> <medium reason>
#                     <journal state> <journal reason> <medium record 0|1>
# ITEM-542: the shared reading of a refusal page.  The page's own section lines
# carry the same prefixes as the machine's records, so they are told apart by
# their shape and not by position:
#
#   * the page's medium line is the bare `[INSTALL-MEDIUM] state=... reason=...`
#     - a machine record carries `pci=`, `media=`, `loader_id=` and, on a
#     match, `scope=`;
#   * the page's journal line appends the counters (`pending=`, `dropped=`,
#     `refused=`), which the machine's own record never carries.
#
# The probe requires the page and the machine to name the journal exactly once
# each, bounds the tail with the machine's own count (`shown=N`, N <= 12),
# requires exactly N records between the tail header and the way out, and - for
# the medium the machine did boot from - requires the record to compare the
# loader's disk id with the device's.
refusal_page_probe() {
    python3 - "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" \
        "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import sys
import time


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

label = sys.argv[1]
log = pathlib.Path(sys.argv[2])
reason = sys.argv[3]
medium_state, medium_reason = sys.argv[4], sys.argv[5]
journal_state, journal_reason = sys.argv[6], sys.argv[7]
medium_record = sys.argv[8] == "1"

required = [
    "Installation refused",
    "  Reason: " + reason,
    "  This medium cannot install on this machine.",
    "Observed storage",
    "controllers: class_ahci=",
    "disks: found=0 accepted=0",
    "[STORAGE-REPORT] probed=1",
    "  Press any key to continue.",
]
# ITEM-518: the observed section names the finding, and the finding depends on
# the emulated machine: q35 carries an ICH9 AHCI controller with no disk (a
# bring-up failure), a machine without one matches no controller class at all.
# Either is the honest answer; a page that names neither is the defect.
observed_reasons = ("reason=NO_CONTROLLER", "reason=BRINGUP_FAILED")


def body():
    try:
        return SERIAL_VIEW.normalise(
            log.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


deadline = time.monotonic() + 240.0
while True:
    text = body()
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit(f"{label}: the kernel panicked before the refusal page")
    missing = [marker for marker in required if marker not in text]
    if not missing:
        break
    if time.monotonic() >= deadline:
        raise SystemExit(f"{label}: the refusal page never appeared: {missing}")
    time.sleep(0.1)
if not any(marker in text for marker in observed_reasons):
    raise SystemExit(
        f"{label}: the observed storage named neither "
        f"{observed_reasons[0]} nor {observed_reasons[1]}")

lines = text.splitlines()

# The tail is located first, because the ring it prints carries copies of the
# very records asserted below - the page's whole point is to show them.  The
# machine's own records are therefore read outside the tail block, and the tail
# block is bounded, counted and closed by the way out before anything else is
# believed.
tail_lines = [line for line in lines if line.startswith("[INSTALL-LOG-TAIL] ")]
if len(tail_lines) != 1:
    raise SystemExit(
        f"{label}: the page printed {len(tail_lines)} tail headers, expected "
        f"exactly one: {tail_lines!r}")
tail_match = re.fullmatch(r"\[INSTALL-LOG-TAIL\] shown=(\d+)", tail_lines[0])
if tail_match is None:
    raise SystemExit(
        f"{label}: the tail header does not name a count: {tail_lines[0]}")
shown = int(tail_match.group(1))
if shown > 12:
    raise SystemExit(
        f"{label}: the page printed {shown} tail records, more than the "
        f"budget of 12")
header_index = lines.index(tail_lines[0])
press_index = lines.index("  Press any key to continue.")
if press_index != header_index + 1 + shown:
    raise SystemExit(
        f"{label}: the way out stands {press_index - header_index - 1} lines "
        f"after the tail header, expected exactly shown={shown} records and "
        f"then the way out")
tail_indices = set(range(header_index + 1, header_index + 1 + shown))
outside = [line for index, line in enumerate(lines)
           if index not in tail_indices]

# The medium: the page prints the bare verdict, the machine record (when the
# run established a storage session) prints its own fields before it.  Both are
# told apart by shape: only the page's line begins at `state=`, and only the
# record begins at `pci=`.
page_medium = f"[INSTALL-MEDIUM] state={medium_state} reason={medium_reason}"
medium_lines = [
    line for line in outside if line.startswith("[INSTALL-MEDIUM] ")]
page_medium_lines = [
    line for line in medium_lines
    if line.startswith("[INSTALL-MEDIUM] state=")]
if len(page_medium_lines) != 1 or page_medium_lines[0] != page_medium:
    raise SystemExit(
        f"{label}: the page named the medium "
        f"{len(page_medium_lines)} times, expected exactly one "
        f"{page_medium!r}: {medium_lines!r}")
if medium_record:
    record_medium = [
        line for line in medium_lines
        if line.startswith("[INSTALL-MEDIUM] pci=") and
        f" state={medium_state} reason={medium_reason} " in line and
        " scope=WHOLE " in line]
    if len(record_medium) != 1:
        raise SystemExit(
            f"{label}: the machine did not publish exactly one medium record "
            f"beside the page's line: {medium_lines!r}")
    identity = re.search(r" loader_id=([0-9A-F]+) disk_id=([0-9A-F]+)",
                         record_medium[0])
    if identity is None or identity.group(1) != identity.group(2):
        raise SystemExit(
            f"{label}: the medium record did not compare the loader's disk id "
            f"with the device's: {record_medium[0]}")
else:
    # No xHCI storage session: the page's line is the only medium line the run
    # may carry, so a second one would be an unproven claim from nowhere.
    if len(medium_lines) != 1:
        raise SystemExit(
            f"{label}: a machine without a medium published more than the "
            f"page's own line: {medium_lines!r}")

# The journal: one machine record without counters, one page line with them.
page_journal = f"[INSTALL-LOG] state={journal_state} reason={journal_reason}"
journal_lines = [
    line for line in outside if line.startswith("[INSTALL-LOG] ")]
page_journal_lines = [
    line for line in journal_lines
    if line.startswith(page_journal + " pending=")]
record_journal_lines = [line for line in journal_lines if line == page_journal]
if len(page_journal_lines) != 1 or len(record_journal_lines) != 1:
    raise SystemExit(
        f"{label}: the page and the machine must each name the journal once "
        f"({len(record_journal_lines)} records, {len(page_journal_lines)} "
        f"page lines): {journal_lines!r}")
for counter in (" dropped=", " refused="):
    if counter not in page_journal_lines[0]:
        raise SystemExit(
            f"{label}: the page's journal line lost {counter!r}: "
            f"{page_journal_lines[0]}")
if len(journal_lines) != 2:
    raise SystemExit(
        f"{label}: the run carries a journal line that is neither the "
        f"machine's record nor the page's section: {journal_lines!r}")

print(f"{label}: the refusal page named the medium "
      f"({medium_state}/{medium_reason}), the journal "
      f"({journal_state}/{journal_reason}) and the bounded tail "
      f"(shown={shown})")
PY
}

# unbound_medium <source> <destination>
# ITEM-542: copy the operator medium and damage the journal frame the build
# reserved, so the booted medium is still the machine's own - its MBR disk id is
# untouched - while the FAT12 volume no longer carries a journal the kernel may
# adopt.  The frame is found by the documented slot-0 layout (magic, version,
# slot size, header CRC) rather than by FAT walking, and a scan that finds
# anything but exactly one valid frame is a failure, never a silent edit.
unbound_medium() {
    python3 - "$1" "$2" <<'PY'
import pathlib
import struct
import sys
import zlib

MAGIC = b"HFOC-ILOG"
VERSION = 1
SLOT_BYTES = 512
HEADER_BYTES = 40
CRC_BYTES = 36

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
data = bytearray(source.read_bytes())
magic_at = []
start = 0
while True:
    index = data.find(MAGIC, start)
    if index < 0:
        break
    magic_at.append(index)
    start = index + 1
valid = []
for index in magic_at:
    if index + HEADER_BYTES > len(data):
        continue
    header = bytes(data[index:index + HEADER_BYTES])
    if struct.unpack_from("<I", header, 9)[0] != VERSION:
        continue
    if struct.unpack_from("<I", header, 13)[0] != SLOT_BYTES:
        continue
    if (zlib.crc32(header[:CRC_BYTES]) & 0xFFFFFFFF) != \
            struct.unpack_from("<I", header, 36)[0]:
        continue
    valid.append(index)
if len(valid) != 1:
    raise SystemExit(
        "the medium carries %d valid journal frames, expected exactly one"
        % len(valid))
data[valid[0]] = 0x00
destination.write_bytes(bytes(data))
print("unbound medium: the reserved journal frame at byte %d is damaged"
      % valid[0])
PY
}

# run_refusal_case <label> <expected reason>
# ITEM-500: a console entry that cannot install at all paints the reason on the
# machine's own screen and waits.  This case boots the medium through SeaBIOS
# instead of OVMF, on a machine whose only drive is the medium itself.
#
# ITEM-517: the machine used to be a two-disk one booted on legacy BIOS, and
# the run refused `FIRMWARE_UNSUPPORTED`; legacy BIOS installs now, so that
# fixture has no refusal left to prove.  The honest one for a machine with no
# disk at all is the plan's own `NO_DISK`, and it still exercises the page, the
# named reason and the zero-write record.
#
# ITEM-542: this machine has no xHCI controller at all, so the page's medium
# verdict is the bare `UNAVAILABLE reason=NO_DEVICE` and no machine record
# stands beside it; the journal is refused `READ_ONLY` because the boot medium
# is the optical disc.
run_refusal_case() {
    local label=$1
    local reason=$2
    local log=$TEST_DIR/$label.log
    local pid

    : >"$log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 4096M -smp 1 \
        -display none -cdrom "$IMAGE" \
        -serial "file:$log" \
        -monitor "unix:$TEST_DIR/$label.monitor,server,nowait" \
        -no-reboot -no-shutdown \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    pid=$!

    if ! refusal_page_probe "$label" "$log" "$reason" \
        UNAVAILABLE NO_DEVICE UNAVAILABLE READ_ONLY 0
    then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "installer disk screen proof ($label): FAILED" >&2
        # The tail of a serial log is the timer stream; the records are what a
        # reader needs, so print the lines that carry them.
        grep -aE 'PRODUCTION-INSTALL|INSTALLER-STORAGE|STORAGE-REPORT|INSTALL-MEDIUM|INSTALL-LOG|Observed storage|controllers:|disks:|Select installation|Declared disk|Installation refused|Reason: ' \
            "$log" | tail -40 >&2 || true
        exit 1
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "installer disk screen proof ($label): OK"
}

# run_usb_refusal_case <label> <expected reason>
# ITEM-542: the medium is present and recognised, but the journal cannot bind.
# The machine boots the operator medium itself through a USB stick with the
# reserved journal frame damaged, so the page must say `state=MATCH reason=OK`
# for the medium and `state=UNAVAILABLE reason=HEADER` for the journal - the
# distinction between "the kernel did not see the stick" (case 11) and "the
# stick is here, but its journal did not bind" that F-7 left unreadable.
run_usb_refusal_case() {
    local label=$1
    local reason=$2
    local log=$TEST_DIR/$label.log
    local image=$TEST_DIR/$label-unbound.img
    local pid

    unbound_medium "$IMAGE" "$image"
    : >"$log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 4096M -smp 1 \
        -display none -boot order=c,menu=off \
        -device 'qemu-xhci,id=item542-xhci,bus=pcie.0,addr=0x10' \
        -drive "if=none,id=item542-medium,file=$image,format=raw,cache=writeback,file.locking=off" \
        -device 'usb-storage,drive=item542-medium,bus=item542-xhci.0' \
        -serial "file:$log" \
        -monitor "unix:$TEST_DIR/$label.monitor,server,nowait" \
        -no-reboot -no-shutdown \
        >"$TEST_DIR/$label-qemu.log" 2>&1 &
    pid=$!

    if ! refusal_page_probe "$label" "$log" "$reason" \
        MATCH OK UNAVAILABLE HEADER 1
    then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "installer disk screen proof ($label): FAILED" >&2
        grep -aE 'PRODUCTION-INSTALL|INSTALLER-STORAGE|STORAGE-REPORT|INSTALL-MEDIUM|INSTALL-LOG|USB-STORAGE|Observed storage|controllers:|disks:|Select installation|Declared disk|Installation refused|Reason: ' \
            "$log" | tail -40 >&2 || true
        exit 1
    fi
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "installer disk screen proof ($label): OK"
}

fresh_disks choose
run_case choose choose \
    "[PRODUCTION-INSTALL] scheme declared=none chosen=erase source=operator writes=0" \
    "[PRODUCTION-INSTALL] layout mode=whole-disk reason=operator-erase declared=0 chosen=erase" \
    "[INSTALLER-STORAGE] summary disk=2 scheme=ERASE declared=NONE status=OK"
# The declared disk is untouched, the chosen one carries the install.
if cmp -s "$TEST_DIR/choose-a.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "choose: the declared disk is still empty (nothing was written to it)"
else
    echo "choose: FAILED - the declared disk was written" >&2
    exit 1
fi
if cmp -s "$TEST_DIR/choose-b.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "choose: FAILED - the disk the operator chose was never written" >&2
    exit 1
fi
echo "choose: the disk the operator named is the disk that was written"

fresh_disks cancel
run_case cancel cancel
if cmp -s "$TEST_DIR/cancel-a.img" <(head -c "$DISK_BYTES" /dev/zero) &&
    cmp -s "$TEST_DIR/cancel-b.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "cancel: neither disk was written after the operator left the screen"
else
    echo "cancel: FAILED - a cancelled run wrote a disk" >&2
    exit 1
fi

# ITEM-500: the machine the medium was not declared for.  It carries neither the
# declared disk nor the disk the other cases chose, so the run has to ask the
# operator and install the disk this machine actually has.
fresh_disks foreign
CASE_A_SERIAL="$FOREIGN_A" CASE_B_SERIAL="$FOREIGN_B" \
    run_case foreign choose \
    "declared=NOT_ON_THIS_MACHINE" "disk=NONE identity=NONE" "writes=0" \
    "[PRODUCTION-INSTALL] scheme declared=none chosen=erase source=operator writes=0"
if cmp -s "$TEST_DIR/foreign-a.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "foreign: the disk nobody chose is still empty"
else
    echo "foreign: FAILED - a disk nobody chose was written" >&2
    exit 1
fi
if cmp -s "$TEST_DIR/foreign-b.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "foreign: FAILED - the disk the operator named was never written" >&2
    exit 1
fi
echo "foreign: the disk this machine has is the disk the operator installed"

# ITEM-517: the same operator entry on a legacy-BIOS machine that carries two
# disks.  Nothing refuses, so the case walks the operator's path to the last
# page before the write - the summary - and proves the plan it draws is the
# BIOS one (BIOS boot partition plus FAT boot volume, no ESP).  Q there and Q
# on the scheme list leave without writing, and both disks stay byte-zero.
fresh_disks bios
CASE_FIRMWARE=bios run_case bios bios-walk
if cmp -s "$TEST_DIR/bios-a.img" <(head -c "$DISK_BYTES" /dev/zero) &&
    cmp -s "$TEST_DIR/bios-b.img" <(head -c "$DISK_BYTES" /dev/zero); then
    echo "bios: neither disk was written by the BIOS walk"
else
    echo "bios: FAILED - the BIOS walk wrote a disk" >&2
    exit 1
fi

# ITEM-500: a console entry that cannot install at all says so on the screen.
# ITEM-517: the honest refusal for this fixture is the machine with no disk:
# legacy BIOS installs now, so `FIRMWARE_UNSUPPORTED` is not a refusal this
# medium can produce any more.  The page, the named reason and the zero-write
# record are still asserted in full.
#
# ITEM-542: this is also the "no USB medium at all" shape of the ITEM-541 page,
# and the case after it is the "medium recognised, journal unbound" shape.
run_refusal_case refusal NO_DISK
# ITEM-542: the operator medium booted from USB with its reserved journal frame
# damaged.  The medium is still the machine's own (`state=MATCH reason=OK`), the
# journal cannot bind (`state=UNAVAILABLE reason=HEADER`), and the page must
# name both - the difference between "the stick was not seen" and "the stick was
# seen, but its journal did not bind".
run_usb_refusal_case unbound NO_DISK

echo "Installer disk screen QEMU proof: OK"
echo "QEMU/OVMF proof != physical hardware acceptance: a person choosing a disk on real hardware is NOT TESTED by this proof"
