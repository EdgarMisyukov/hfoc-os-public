#!/usr/bin/env python3
"""ITEM-525 host witness: the gate assertions read the normalised console.

Branch CI run 38264136090 (2026-10-12) lost
`test-production-service-https-qemu` at
`scripts/test-qemu-production-service-https.sh:3314` with "The console enable
gesture published no gate record", although the record was in the file: an
interrupt-context diagnostic had landed inside it, exactly as the artifact
shows (`production-service-20261012T150914Z-1203298/account-durability-boot.log`):

    [[IOAPIC] irq1 vector=0x41\\r
    [IOAPIC] irq1 lapic_eoi\\r
    PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET

The record is printed one character at a time, so the diagnostic line split it
after the leading `[`.  The proof's embedded python strips diagnostic lines
before searching and so reported success; the shell greps read the raw file and
did not.

This witness extracts the shipped python helper and the shipped gate block from
`scripts/test-qemu-production-service-https.sh` - it cannot drift from the code
under test - and runs the block against those recorded bytes:

  * the raw fixture must still hide the record (the failure mechanism);
  * the shipped python `normalise` mode must recover it (python already did);
  * the shipped block must accept the interleaved stream (the fix);
  * the same block over a helper that renders the raw file must fail with the
    recorded failure message (the negative control: the fixture discriminates);
  * every `grep` in the shipped block must read the normalised view, not the
    raw serial file (only failure diagnostics may print raw text).
"""
import pathlib
import shlex
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROOF = ROOT / "scripts" / "test-qemu-production-service-https.sh"

REPORTED_RECORD = "[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET"
REPORTED_FAILURE = "The console enable gesture published no gate record"

# The bytes run 38264136090 left in account-durability-boot.log, in the order
# the console printed them.  The ENABLED record is split across three lines by
# two IRQ diagnostics, which is why the raw file does not contain it.
FIXTURE = (
    b"[PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET\r\n"
    b"ADMIN SESSION: Authorization: Bearer Xn1rbY8lGy2R3yQBo1t22dkg3XdrEnhZ\n"
    b"The machine will NO LONGER REQUIRE PHYSICAL PRESENCE: the panel answers"
    b" over the network without anyone logging in locally.\n"
    b"[PRODUCTION-SECURITY] gate=PENDING action=open"
    b" confirmation_required\n"
    b"\n"
    b"[[IOAPIC] irq1 vector=0x41\r\n"
    b"[IOAPIC] irq1 lapic_eoi\r\n"
    b"PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET\n"
)

# A view helper that renders the raw file: the negative control, so a passing
# block cannot come from a fixture that never bit.  ITEM-526 moved the shipped
# helper onto the shared renderer, so the control replaces the helper itself
# rather than the python it used to call.
RAW_HELPER = """\
serial_view() {
    local view=$1.view
    cp "$1" "$view"
    printf '%s\\n' "$view"
}
"""

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


def extract_heredoc(lines):
    """The embedded admin-enroll.py, between its heredoc markers."""
    start = None
    for index, line in enumerate(lines):
        if line.strip() == "cat >\"$TEST_DIR/admin-enroll.py\" <<'ENROLLEOF'":
            start = index + 1
            break
    if start is None:
        return ""
    for end in range(start, len(lines)):
        if lines[end].strip() == "ENROLLEOF":
            return "\n".join(lines[start:end]) + "\n"
    return ""


def extract_helper(lines):
    """The shipped normalising view helper, defined before the gate block."""
    start = None
    for index, line in enumerate(lines):
        if line.startswith("serial_view() {"):
            start = index
            break
    if start is None:
        return ""
    for end in range(start, len(lines)):
        if lines[end] == "}":
            return "\n".join(lines[start:end + 1]) + "\n"
    return ""


def extract_block(lines):
    """The shipped gate assertion block, from its records to its success line."""
    start = None
    for index, line in enumerate(lines):
        if line.startswith(
                'GATE_CLOSED_RECORD="[PRODUCTION-SECURITY] gate=CLOSED'):
            start = index
            break
    if start is None:
        raise SystemExit("the gate assertion block is not in the proof")
    for end in range(start, len(lines)):
        if lines[end].startswith('echo "TLS-PROOF gate enable:'):
            return "\n".join(lines[start:end]) + "\n"
    raise SystemExit("the gate assertion block has no success line")


def run_block(work, name, block, helper):
    """Run the shipped block in its own bash with the given view helper."""
    test_dir = work / f"view-{name}"
    test_dir.mkdir()
    fixture = work / "account-durability-boot.log"
    fixture.write_bytes(FIXTURE)
    harness = work / "harness.sh"
    harness.write_text(
        "ACCOUNT_DURABILITY_SERIAL=" + shlex.quote(str(fixture)) + "\n"
        "ROOT=" + shlex.quote(str(ROOT)) + "\n"
        "TEST_DIR=" + shlex.quote(str(test_dir)) + "\n"
        "FAILURE_MESSAGE=\n"
        "dump_gate_diagnostics() { :; }\n"
        + helper + block)
    completed = subprocess.run(
        ["bash", str(harness)], cwd=work, capture_output=True, text=True)
    return completed


def main():
    lines = PROOF.read_text().splitlines()
    python_source = extract_heredoc(lines)
    helper = extract_helper(lines)
    block = extract_block(lines)

    check("the proof ships its embedded python helper", bool(python_source))
    check("the proof ships the gate assertion block", bool(block))

    raw = FIXTURE
    check("the raw recorded bytes do not contain the record",
          REPORTED_RECORD.encode() not in raw)
    check("the raw recorded bytes contain the split tail",
          b"PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET" in raw)

    with tempfile.TemporaryDirectory(prefix="hfoc-item-525-") as tmp:
        work = pathlib.Path(tmp)
        fixture = work / "raw-fixture.log"
        fixture.write_bytes(FIXTURE)
        (work / "shipped-python.py").write_text(python_source)
        normalised = subprocess.run(
            ["python3", str(work / "shipped-python.py"), "normalise",
             str(fixture)],
            capture_output=True)
        check("the shipped python normalise mode answers",
              normalised.returncode == 0,
              normalised.stderr.decode(errors="replace"))
        check("the shipped python recovers the record",
              REPORTED_RECORD.encode() in normalised.stdout,
              normalised.stdout[-200:])

        positive = run_block(work, "shipped", block, helper)
        check("the shipped block accepts the interleaved record",
              positive.returncode == 0,
              f"exit={positive.returncode} "
              f"stderr={positive.stderr.strip()!r}")

        negative = run_block(work, "raw", block, RAW_HELPER)
        check("the negative control still fails on the raw file",
              negative.returncode != 0,
              f"exit={negative.returncode}")
        check("the negative control fails the recorded way",
              REPORTED_FAILURE in negative.stderr,
              negative.stderr.strip())

    logical = []
    pending = ""
    for line in block.splitlines():
        pending += line
        if line.rstrip().endswith("|") or line.rstrip().endswith("\\"):
            continue
        logical.append(pending)
        pending = ""
    if pending:
        logical.append(pending)
    greps = [line for line in logical if "grep" in line]
    raw_greps = [line.strip() for line in greps
                 if "serial_view" not in line and ">&2" not in line]
    check("every gate assertion reads the normalised view", not raw_greps,
          str(raw_greps))

    print(f"\n{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
