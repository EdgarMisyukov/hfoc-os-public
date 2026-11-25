#!/usr/bin/env python3
"""ITEM-526/ITEM-528: render a serial log with its periodic IRQ lines removed.

The console prints one character at a time, and the kernel writes periodic
[IOAPIC] and [LAPIC-TIMER] IRQ lines from interrupt context, so such a line can
land in the middle of a record the console is printing.  A shell assertion that
greps the raw serial file can then report a record missing while the proof's
python half - which already strips the periodic lines - has seen it.  Run
38264136090 (2026-10-12) lost the credential-durability gate exactly that way.

ITEM-528 narrowed the rule to those periodic lines.  ITEM-526's broader form
also removed one-shot console records that merely begin with an interrupt
prefix, and proofs require some of them ("[IRQ ] IF enabled; entering HLT event
loop.", "[PIC ] Master mask: ...", "[PIC ] disabled; APIC runtime active.").

This is the one normaliser every shell assertion of the QEMU proofs renders the
serial through: it is the same rule the embedded python helpers carry
(`DIAGNOSTIC_LINE` in test-qemu-production-service-https.sh and
test-qemu-production-forward.sh, `IRQ_DIAGNOSTIC` in
test-qemu-production-dhcp-dns.sh), so the two halves of a proof cannot disagree
about what the console said.

The rendered stream is written to stdout; the shell helpers (`serial_view` in
the proofs) redirect it to a file and grep that file, so a reader that stops
early cannot SIGPIPE the renderer (ITEM-437).

Usage: hfoc-serial-view.py <serial-log>
"""
import pathlib
import re
import sys

# The periodic IRQ lines are complete lines written from interrupt context:
# removing them restores the character stream the console emitted.  This
# literal is the shared rule; the embedded copies in the three proofs above
# must keep the same text.
#
# ITEM-528 narrowed the rule.  ITEM-526's form removed every line that began
# with an interrupt prefix, which also deleted console records the proofs
# require - "[IRQ ] IF enabled; entering HLT event loop." (test-qemu-irqs.sh),
# "[PIC ] Master mask: ..." (test-qemu-ioapic-irq0.sh, test-qemu-ioapic-irq1.sh)
# and "[PIC ] disabled; APIC runtime active." (test-qemu-ioapic-irq3-7.sh,
# test-qemu-ioapic-irq8-15.sh) are output, not diagnostics.  Only the periodic
# lines can split a record, and the witness derives them from
# kernel/src/arch/x86_64/interrupts.c, so a new periodic line fails the witness
# until the rule covers it.
DIAGNOSTIC_LINE = re.compile(
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")


def normalise(text):
    """The diagnostics-stripped console stream of one serial log."""
    return DIAGNOSTIC_LINE.sub("", text)


def serial_text(path):
    """Read a serial file and return its normalised text, empty when absent."""
    try:
        return normalise(
            path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: hfoc-serial-view.py <serial-log>\n")
        return 2
    sys.stdout.write(serial_text(pathlib.Path(argv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
