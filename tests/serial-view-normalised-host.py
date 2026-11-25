#!/usr/bin/env python3
"""ITEM-526/ITEM-527 host witness: every shell assertion reads the normalised
console.

ITEM-525 fixed one boot of one proof.  The same defect - a shell assertion that
greps the raw serial file while an interrupt-context diagnostic can land inside
the record the console prints one character at a time - remained in the other
nine guest boots of `scripts/test-qemu-production-service-https.sh`, in that
proof's own first boot (`-serial "file:$SERIAL_LOG"`), and in the five proofs
that had no normaliser at all
(`test-qemu-production-sni-selection.sh`, the four `test-qemu-acme-*.sh`).
ITEM-526 fixed those sixteen boots and this witness was written for them;
ITEM-527 extended it to the ten remaining proofs that write a QEMU serial file
(platform-power, irqs, production-first-boot, production-forward,
production-recovery-reset, production-storage-boot, usb-mouse-menu,
usb-two-controllers, installed-boot - the two stdio-only EFI proofs are named
in the PROOFS comment and deliberately excluded).  ITEM-528 narrowed the rule
itself: it removes the periodic lines the kernel writes from interrupt context
and nothing else, so the records that begin with the same prefixes - and that
proofs require - survive the view.  ITEM-529 converted the last five embedded
python readers of these proofs, so both halves of every covered proof read the
normalised stream.  ITEM-530 extended the python-half rule to the panic/fault
proofs (doublefault, nmi, pagefault, panic), whose shell halves have no serial
assertion of their own and so are listed in PYTHON_HALF_PROOFS; ITEM-531 added
the input/console group (keyboard-line-editing, local-control,
network-foundation, nic-discovery, physical-sata-warm-reset); ITEM-532…536
added the https-matrix, installer, https-client/drill, install-flow/
physical-storage and heap/pmm/vmm groups.  ITEM-537 made the class control
honest: a bare mention of an interrupt prefix no longer counts as a normaliser,
so five already-covered proofs whose python halves still carried ITEM-526's
wide rule (platform-power, production-storage-boot, usb-mouse-menu,
usb-two-controllers, installed-boot) had to convert, and the interrupt-
controller group (acpi, ioapic-irq3-7, ioapic-irq8-15, ioapic-probe) joined
them.

This witness pins the class, not one instance:

  * the shared renderer `scripts/hfoc-serial-view.py` strips the diagnostics and
    recovers the record from the bytes run 38264136090 recorded;
  * its rule is byte-identical to the rule the embedded python helpers already
    carry in the https, forward and dhcp-dns proofs, so the python and shell
    halves of a proof cannot disagree about what a diagnostic is;
  * every proof ships a `serial_view` helper that renders through a normaliser
    into a file and prints that file, so no reader can SIGPIPE the renderer
    (ITEM-437);
  * no `grep`/`sed` assertion in those proofs reads a raw serial variable; only
    failure diagnostics may print the raw stream (`>&2`).  A serial log is any
    variable the proof feeds to `-serial "file:..."` - the QEMU argument is the
    ground truth, so a log named `SERIAL_LOG` is covered like one named
    `*_SERIAL`;
  * the shipped helper of every proof recovers the record when run against the
    recorded bytes - the behavioural half, not just the text.

The static half is the contract; the behavioural half is the negative control
that keeps the fixture able to bite.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHARED = ROOT / "scripts" / "hfoc-serial-view.py"
HTTPS = ROOT / "scripts" / "test-qemu-production-service-https.sh"

REPORTED_RECORD = "[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET"

# The bytes run 38264136090 left in account-durability-boot.log: two [IOAPIC]
# diagnostics split the record after its leading `[`.
FIXTURE = (
    b"[PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET\r\n"
    b"ADMIN SESSION: Authorization: Bearer Xn1rbY8lGy2R3yQBo1t22dkg3XdrEnhZ\n"
    b"[PRODUCTION-SECURITY] gate=PENDING action=open"
    b" confirmation_required\n"
    b"[[IOAPIC] irq1 vector=0x41\r\n"
    b"[IOAPIC] irq1 lapic_eoi\r\n"
    b"PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET\n"
)

# Two more recorded splits, each carried by the proof that hit it and each
# restored by that proof's own normaliser (test-qemu-heap.sh and
# test-qemu-scheduler-stress.sh): the periodic lines are the ones that land
# inside a record, and the shared rule must restore both records byte for byte.
SPLIT_FIXTURES = [
    (
        b"[HEAP] ready=y[LAPIC-TIMER] irq vector=0x42\r\n"
        b"[LAPIC-TIMER] irq lapic_eoi\r\n"
        b"es capacity=4194304 used=0\r\n"
        b"hfoc> ",
        b"[HEAP] ready=yes capacity=4194304 used=0",
    ),
    (
        b"[STRESS] complete workers=7 [LAPIC-TIMER] irq vector=0x42\r\n"
        b"[LAPIC-TIMER] irq lapic_eoi\r\nreclaimed pages=28\r\n",
        b"[STRESS] complete workers=7 reclaimed pages=28",
    ),
]

# The one normalising rule.  The shared renderer and the embedded helpers must
# all carry this literal, so a record one half can see is a record the other
# half can see.
#
# ITEM-526 removed every line that began with an interrupt prefix.  ITEM-528
# narrowed the rule to the periodic lines the kernel really writes from
# interrupt context, because the broad form also deleted records proofs
# require: "[IRQ ] IF enabled; entering HLT event loop."
# (scripts/test-qemu-irqs.sh, test-qemu-integrated-appliance-boot-acceptance.sh),
# "[PIC ] Master mask: ..." (test-qemu-ioapic-irq0.sh, test-qemu-ioapic-irq1.sh)
# and "[PIC ] disabled; APIC runtime active." (test-qemu-ioapic-irq3-7.sh,
# test-qemu-ioapic-irq8-15.sh) are console records, not diagnostics.
RULE = (
    '    r"\\[(?:IOAPIC|LAPIC-TIMER)\\] irq[0-9]*"\n'
    '    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\\[\\r\\n]*\\r?\\n")'
)

# The periodic lines the kernel writes from interrupt context
# (kernel/src/arch/x86_64/interrupts.c).  They repeat at timer and keyboard
# rate and are the ones that can land inside a record; the check below derives
# this list from the kernel source, so a new periodic line fails the witness
# until the rule covers it.
PERIODIC_PREFIX = r'serial_write\("(\[(?:IOAPIC|LAPIC-TIMER)\] irq[^"]*)"\)'

# Records that begin with the same prefixes and must survive the view.  Each
# one is a marker a proof requires and each was recorded in the queue's own
# captured serial logs (qemu-diag-logs).
RECORD_MARKERS = [
    "[IRQ ] IF enabled; entering HLT event loop.",
    "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF",
    "[PIC ] disabled; APIC runtime active.",
    "[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.",
    "[PIC ] IRQ1 unmasked; legacy PIC fallback retained.",
    "[LAPIC] SVR configured: vector=239; legacy PIC retained",
    "[LAPIC-TIMER] oneshot armed: vector=0x43",
    "[IOAPIC] route: irq=0 vector=0x40 gsi=0 dest=0",
]

# The loader every converted python half carries, byte-identical, so the two
# halves of a proof cannot drift from the shared renderer.
LOADER = '''def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
'''

# Every proof the normalised-view rule covers.  ITEM-526 did the https proof's
# ten boots, the sni-selection proof and the four acme proofs; ITEM-527 did the
# ten remaining proofs that write a QEMU serial file.  Each one's serial_view
# renders through the shared repository renderer - including the https proof,
# whose earlier call to its embedded helper was written too late in the script
# to exist during the first boot (CI run 38408474798 failed exactly there,
# which is why the witness also runs every helper with no admin-enroll.py
# present at all).
#
# test-qemu-efi-ovmf.sh and test-qemu-hfoc-efi-installer.sh are not listed:
# they run QEMU with `-serial stdio` and their *_LOG files capture host tool
# output, not a guest console written character by character, so an
# interrupt-context diagnostic cannot split a record in them.
PROOFS = [
    "scripts/test-qemu-production-service-https.sh",
    "scripts/test-qemu-production-sni-selection.sh",
    "scripts/test-qemu-acme-order.sh",
    "scripts/test-qemu-acme-boot-order.sh",
    "scripts/test-qemu-acme-http01.sh",
    "scripts/test-qemu-acme-directory.sh",
    "scripts/test-qemu-platform-power.sh",
    "scripts/test-qemu-irqs.sh",
    "scripts/test-qemu-production-first-boot.sh",
    "scripts/test-qemu-production-forward.sh",
    "scripts/test-qemu-production-recovery-reset.sh",
    "scripts/test-qemu-production-storage-boot.sh",
    "scripts/test-qemu-usb-mouse-menu.sh",
    "scripts/test-qemu-usb-two-controllers.sh",
    "scripts/test-qemu-installed-boot.sh",
]

# Serial logs whose name does not say so and which are not handed to `-serial`
# directly: storage-boot's per-label logs are created and written by
# run_boot_machine, and the callers keep the path under their own name.
EXTRA_SERIAL_NAMES = {
    "scripts/test-qemu-production-storage-boot.sh": {
        "usb_loss_log", "usb_second_log", "usb_empty_log",
    },
    "scripts/test-qemu-pmm.sh": {
        "HIGHMEM_SERIAL_LOG", "UEFI_SERIAL_LOG",
    },
}

# Serial logs a proof hands to its embedded python by a literal path rather
# than a declared serial variable: install-flow's language boot writes
# $TEST_DIR/language.log, and install-flow-preserve's assertions read
# $TEST_DIR/red.log, green.log, installed.log and release.log.  Matching the
# exact literals keeps every other $TEST_DIR path out of the serial set.
EXTRA_SERIAL_LITERALS = {
    "scripts/test-qemu-production-install-flow.sh": (
        "$TEST_DIR/language.log",
    ),
    "scripts/test-qemu-production-install-flow-preserve.sh": (
        "$TEST_DIR/red.log",
        "$TEST_DIR/green.log",
        "$TEST_DIR/installed.log",
        "$TEST_DIR/release.log",
    ),
}

# Proofs whose *python* half must read the normalised stream even though their
# shell half has no serial assertion at all (so they need no `serial_view`
# helper): their embedded readers wait for the panic, fault and console records.
# ITEM-530 did the panic/fault group, ITEM-531 the input/console group,
# ITEM-532 the https matrix group, ITEM-533 the installer group, ITEM-534 the
# https client, production-network and drill group, ITEM-535 the install-flow
# and physical-storage group, ITEM-536 the heap, pmm and vmm group, ITEM-537
# the interrupt-controller group and ITEM-538 the storage and configuration
# group; the remaining groups are queued.
PYTHON_HALF_PROOFS = [
    "scripts/test-qemu-doublefault.sh",
    "scripts/test-qemu-nmi.sh",
    "scripts/test-qemu-pagefault.sh",
    "scripts/test-qemu-panic.sh",
    "scripts/test-qemu-keyboard-line-editing.sh",
    "scripts/test-qemu-local-control.sh",
    "scripts/test-qemu-network-foundation.sh",
    "scripts/test-qemu-nic-discovery.sh",
    "scripts/test-qemu-physical-sata-warm-reset.sh",
    "scripts/test-qemu-https-exhaustion-matrix.sh",
    "scripts/test-qemu-https-malformed-request-matrix.sh",
    "scripts/test-qemu-https-response-matrix.sh",
    "scripts/test-qemu-https-size-boundary-matrix.sh",
    "scripts/test-qemu-installer-disk-screen.sh",
    "scripts/test-qemu-installer-display.sh",
    "scripts/test-qemu-production-installer.sh",
    "scripts/test-qemu-https-client.sh",
    "scripts/test-qemu-production-network.sh",
    "scripts/test-qemu-trust-failure-drill.sh",
    "scripts/test-qemu-update-failure-drill.sh",
    "scripts/test-qemu-network-partition-drill.sh",
    "scripts/test-qemu-integration-replay-outage-drill.sh",
    "scripts/test-qemu-supervisor-composition.sh",
    "scripts/test-qemu-production-install-flow.sh",
    "scripts/test-qemu-production-install-flow-preserve.sh",
    "scripts/test-qemu-physical-sata-acceptance.sh",
    "scripts/test-qemu-physical-storage.sh",
    "scripts/test-qemu-boot-chain.sh",
    "scripts/test-qemu-heap.sh",
    "scripts/test-qemu-pmm.sh",
    "scripts/test-qemu-vmm.sh",
    "scripts/test-qemu-vmm-as.sh",
    "scripts/test-qemu-acpi.sh",
    "scripts/test-qemu-ioapic-irq3-7.sh",
    "scripts/test-qemu-ioapic-irq8-15.sh",
    "scripts/test-qemu-ioapic-probe.sh",
    "scripts/test-qemu-accounting-boundary.sh",
    "scripts/test-qemu-backup-restore.sh",
    "scripts/test-qemu-block-cache.sh",
    "scripts/test-qemu-block-device.sh",
    "scripts/test-qemu-boot-policy-regression.sh",
    "scripts/test-qemu-config-journal.sh",
    "scripts/test-qemu-data-region.sh",
    "scripts/test-qemu-persistent-config.sh",
    "scripts/test-qemu-persistent-config-reboot-acceptance.sh",
    "scripts/test-qemu-persistent-network-config.sh",
    "scripts/test-qemu-power-cycle-continuity-drill.sh",
    "scripts/test-qemu-storage-interruption.sh",
    "scripts/test-qemu-update-lifecycle.sh",
    "scripts/test-qemu-appliance-boot-console.sh",
    "scripts/test-qemu-appliance-console.sh",
    "scripts/test-qemu-install-notice.sh",
    "scripts/test-qemu-integrated-appliance-boot-acceptance.sh",
    "scripts/test-qemu-local-control-outage-drill.sh",
    "scripts/test-qemu-network-config.sh",
    "scripts/test-qemu-service-cascade-drill.sh",
    "scripts/test-qemu-supportability.sh",
    "scripts/test-qemu-syscall.sh",
    "scripts/test-qemu-trust-policy.sh",
    "scripts/test-qemu-user-init.sh",
    "scripts/test-qemu-user-iretq.sh",
]

# Proofs whose embedded python carries the same rule and must keep the literal.
EMBEDDED_RULE = [
    "scripts/test-qemu-production-service-https.sh",
    "scripts/test-qemu-production-forward.sh",
    "scripts/test-qemu-production-dhcp-dns.sh",
]

SERIAL_VAR = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*)")
SERIAL_TARGET = re.compile(r'-serial\s+"file:\$([A-Za-z_][A-Za-z0-9_]*)"')

checks = 0
failures = 0


def check(name, condition, detail=""):
    global checks, failures
    checks += 1
    if condition:
        print(f"  ok   {name}")
    else:
        failures += 1
        print(f"  FAIL {name}" + (f" -- {detail}" if detail else ""))


def logical_lines(text):
    """Join backslash continuations into the statements bash executes."""
    logical = []
    pending = ""
    for line in text.splitlines():
        pending += line + "\n"
        if line.rstrip().endswith("\\"):
            continue
        logical.append(pending)
        pending = ""
    if pending:
        logical.append(pending)
    return logical


def python_blocks(text):
    """(invocation, source) of every embedded python heredoc of a proof.

    The invocation is the `python3 - ...` statement, including continuation
    lines, because that is where the arguments live; the source is the heredoc
    body.  A python half that is handed a serial log reads it there.
    """
    lines = text.splitlines()
    blocks = []
    index = 0
    while index < len(lines):
        if "python3 -" not in lines[index]:
            index += 1
            continue
        marker = index
        while marker < len(lines) and "<<'" not in lines[marker]:
            marker += 1
        if marker >= len(lines):
            break
        end = marker + 1
        while end < len(lines) and lines[end].strip() != \
                re.search(r"<<'([A-Z_]+)'", lines[marker]).group(1):
            end += 1
        blocks.append(("\n".join(lines[index:marker + 1]),
                       "\n".join(lines[marker + 1:end])))
        index = end + 1
    return blocks


def declared_serial_names(text, extra=()):
    """Every shell variable of a proof that holds a QEMU serial log.

    A serial log is not always named `*_SERIAL`: the https proof's own boot
    writes `-serial "file:$SERIAL_LOG"`.  The QEMU argument is the ground
    truth, so the names it feeds are collected here together with the helper
    parameters the proofs pass those logs into and the per-proof extras
    (storage-boot's per-label logs).
    """
    names = set(extra)
    for name in SERIAL_VAR.findall(text):
        if name == "serial" or name.endswith("SERIAL"):
            names.add(name)
    names.update(SERIAL_TARGET.findall(text))
    return names


def serial_names(statement, declared):
    """Shell serial variables a statement reads."""
    return [name for name in SERIAL_VAR.findall(statement)
            if name in declared]


def extract_helper(text):
    """The shipped `serial_view()` function body."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("serial_view() {"):
            for end in range(index, len(lines)):
                if lines[end] == "}":
                    return "\n".join(lines[index:end + 1]) + "\n"
    return ""


def extract_heredoc(text, marker):
    """One embedded python heredoc, between its marker lines."""
    lines = text.splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.strip() == marker:
            start = index + 1
            break
    if start is None:
        return ""
    for end in range(start, len(lines)):
        if lines[end].strip() == marker.split("<<")[-1].strip("'\""):
            return "\n".join(lines[start:end]) + "\n"
    return ""


def run_serial_view(helper, work, name):
    """Run a proof's shipped serial_view with no embedded helper present.

    The TEST_DIR deliberately holds nothing but the log: the https proof's own
    first boot runs before its `admin-enroll.py` is written, so a helper that
    depends on that file renders an empty view there (CI run 38408474798).
    """
    test_dir = work / f"view-{name}"
    test_dir.mkdir()
    fixture = work / f"{name}-boot.log"
    fixture.write_bytes(FIXTURE)
    harness = work / f"harness-{name}.sh"
    harness.write_text(
        "set -Eeuo pipefail\n"
        f"ROOT={str(ROOT)!r}\n"
        f"TEST_DIR={str(test_dir)!r}\n"
        + helper
        + f'serial_view {str(fixture)!r}\n')
    completed = subprocess.run(
        ["bash", str(harness)], capture_output=True, text=True)
    if completed.returncode != 0:
        return completed, ""
    view = pathlib.Path(completed.stdout.strip())
    return completed, view.read_text(errors="replace")


def main():
    check("the shared renderer exists", SHARED.is_file())
    check("the shared renderer carries the one rule",
          SHARED.is_file() and RULE in SHARED.read_text())

    for proof in EMBEDDED_RULE:
        text = (ROOT / proof).read_text()
        check(f"{proof} keeps the same rule",
              RULE in text)

    interrupts = (ROOT / "kernel/src/arch/x86_64/interrupts.c").read_text()
    periodic_literals = re.findall(PERIODIC_PREFIX, interrupts)
    check("the kernel writes the periodic IRQ lines from interrupt context",
          len(periodic_literals) >= 6, str(sorted(periodic_literals)))

    with tempfile.TemporaryDirectory(prefix="hfoc-item-526-") as tmp:
        work = pathlib.Path(tmp)
        fixture = work / "recorded.log"
        fixture.write_bytes(FIXTURE)

        check("the raw recorded bytes still hide the record",
              REPORTED_RECORD.encode() not in FIXTURE)

        shared_run = subprocess.run(
            ["python3", str(SHARED), str(fixture)], capture_output=True)
        check("the shared renderer recovers the record",
              shared_run.returncode == 0 and
              REPORTED_RECORD.encode() in shared_run.stdout,
              shared_run.stdout[-200:])
        check("the shared renderer removed the diagnostics",
              b"[IOAPIC" not in shared_run.stdout)

        for index, (split_bytes, split_record) in enumerate(SPLIT_FIXTURES):
            split_file = work / f"split-{index}.log"
            split_file.write_bytes(split_bytes)
            check(f"the raw split fixture {index} still hides its record",
                  split_record not in split_bytes)
            split_run = subprocess.run(
                ["python3", str(SHARED), str(split_file)],
                capture_output=True)
            check(f"the shared renderer restores the recorded split {index}",
                  split_run.returncode == 0 and
                  split_record in split_run.stdout,
                  split_run.stdout[-200:])

        https_text = HTTPS.read_text()
        python_source = extract_heredoc(
            https_text, "cat >\"$TEST_DIR/admin-enroll.py\" <<'ENROLLEOF'")
        check("the https proof still ships its embedded python",
              bool(python_source))
        embedded = subprocess.run(
            ["python3", "-c", python_source, "normalise", str(fixture)],
            capture_output=True)
        check("the embedded python and the shared renderer agree byte for byte",
              embedded.returncode == 0 and
              embedded.stdout == shared_run.stdout,
              f"embedded exit={embedded.returncode} "
              f"stderr={embedded.stderr.decode(errors='replace')[-200:]}")

        periodic_file = work / "periodic.log"
        periodic_text = "".join(
            literal.replace("\\n", "\n")
            for literal in periodic_literals)
        periodic_file.write_text(periodic_text, encoding="utf-8")
        periodic_run = subprocess.run(
            ["python3", str(SHARED), str(periodic_file)], capture_output=True)
        check("the rule removes every periodic line the kernel writes",
              periodic_run.returncode == 0 and not periodic_run.stdout,
              periodic_run.stdout[:200])

        records_file = work / "records.log"
        records_text = "".join(line + "\n" for line in RECORD_MARKERS)
        records_file.write_text(records_text, encoding="utf-8")
        records_run = subprocess.run(
            ["python3", str(SHARED), str(records_file)], capture_output=True)
        check("the rule keeps every record a proof requires",
              records_run.returncode == 0 and
              records_run.stdout.decode() == records_text,
              records_run.stdout.decode()[:200])

        for proof in PROOFS:
            text = (ROOT / proof).read_text()
            helper = extract_helper(text)
            check(f"{proof} ships serial_view", bool(helper))
            if not helper:
                continue
            check(f"{proof} renders into a file and prints it",
                  '>"$view"' in helper and 'printf' in helper and
                  '"$view"' in helper.split('printf', 1)[-1])
            check(f"{proof} renders through the shared renderer",
                  'hfoc-serial-view.py" "$1"' in helper)
            check(f"{proof} serial_view needs no late helper file",
                  "admin-enroll.py" not in helper)
            completed, view = run_serial_view(
                helper, work, pathlib.Path(proof).stem)
            check(f"{proof} serial_view recovers the record",
                  completed.returncode == 0 and REPORTED_RECORD in view,
                  f"exit={completed.returncode} "
                  f"stderr={completed.stderr.strip()[-200:]!r}")

    for proof in PROOFS + PYTHON_HALF_PROOFS:
        text = (ROOT / proof).read_text()
        declared = declared_serial_names(text, EXTRA_SERIAL_NAMES.get(proof, ()))
        check(f"{proof} names the serial logs it asserts on", bool(declared),
              str(sorted(declared)))
        raw = []
        for statement in logical_lines(text):
            if not re.search(r"(^|[|;&(]\s*|\s)(grep|sed)\b", statement):
                continue
            if ">&2" in statement:
                continue
            for name in serial_names(statement, declared):
                if "serial_view" not in statement:
                    raw.append(f"${name}: {' '.join(statement.split())[:100]}")
                break
        check(f"{proof} has no raw serial assertion", not raw, str(raw[:3]))

        piped = [statement for statement in logical_lines(text)
                 if re.search(r"serial_view[^\n]*\)\s*\|", statement)]
        check(f"{proof} never pipes serial_view into a reader", not piped,
              str(piped[:2]))

        # The python half must read the same stream as the shell half: either
        # the shell hands it a rendered view, or the block loads the shared
        # renderer itself through the one loader.  ITEM-537 removed the loose
        # `IOAPIC|LAPIC-TIMER` mention test: a block that merely asserts on a
        # periodic line, or that still carries ITEM-526's wide prefix rule,
        # does not normalise the way the shared renderer does, and counting it
        # as converted is what let five covered proofs drift (ITEM-537).
        raw_python = []
        for position, (invocation, source) in enumerate(python_blocks(text)):
            names = [name for name in SERIAL_VAR.findall(invocation)
                     if name in declared]
            literals = [literal for literal in
                        EXTRA_SERIAL_LITERALS.get(proof, ())
                        if literal in invocation]
            if not names and not literals:
                continue
            normalised = ("serial_view" in invocation or LOADER in source)
            if not normalised:
                raw_python.append(
                    f"block {position + 1}: "
                    + (f"${names[0]}" if names else literals[0]))
        check(f"{proof} python halves read the normalised stream",
              not raw_python, str(raw_python[:3]))

    print(f"\n{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
