#!/usr/bin/env python3
"""Report serial marker lines that are assembled without a sequence lock.

A printed line built from more than one serial_write*() call can be split by an
interrupt-context print (IRQ traces such as "[IOAPIC] irq0 ..."), which corrupts
the marker for the QEMU proof parsers and shows up as "timed out waiting for
<marker>" even though the guest emitted it. Such a line must sit inside one
serial_sequence_begin()/serial_sequence_end() pair.

Fragments that never terminate a line (for example a MAC address helper) are
ignored: they end at a function boundary and are covered by their caller's
sequence lock.
"""

import re
import sys

WRITE_RE = re.compile(r"^\s*serial_write(_u64|_hex)?\s*\(")
BEGIN_RE = re.compile(r"\bserial_sequence_begin\s*\(")
END_RE = re.compile(r"\bserial_sequence_end\s*\(")


def analyse(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    violations = []
    locked = False
    group = []
    group_locked = True
    statement = None
    depth = 0

    def finish_line():
        nonlocal group, group_locked
        if len(group) >= 2 and not group_locked:
            violations.append((group[0], group[-1], len(group)))
        group = []
        group_locked = True

    def drop_group():
        nonlocal group, group_locked
        group = []
        group_locked = True

    for number, line in enumerate(lines, start=1):
        if line == "}":
            drop_group()
            statement = None
            depth = 0
            continue
        if statement is None:
            if BEGIN_RE.search(line):
                locked = True
                continue
            if END_RE.search(line):
                locked = False
                continue
            if WRITE_RE.match(line) is None:
                continue
            statement = number
            depth = line.count("(") - line.count(")")
            if not group:
                group_locked = locked
            group.append(number)
            if depth <= 0:
                statement = None
                if "\\n" in line:
                    finish_line()
            continue
        depth += line.count("(") - line.count(")")
        if depth <= 0:
            statement = None
            if "\\n" in line:
                finish_line()
    drop_group()
    return violations


def main(argv):
    status = 0
    for path in argv[1:]:
        for start, end, count in analyse(path):
            print(f"{path}:{start}-{end}: line assembled from {count} "
                  f"serial_write calls without serial_sequence_begin/end",
                  file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
