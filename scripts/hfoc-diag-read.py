#!/usr/bin/env python3
"""ITEM-552: carry the HFOC diagnostic report off the boot stick.

The operator types one command on the laptop (`diag save`), shuts the machine
down, moves the stick to the Debian machine and runs this script.  It finds the
medium by what the medium carries - the reserved ``HFOC/INSTALL.LOG`` journal on
a FAT12 EFI partition, the same file the kernel wrote - and never by assuming
the stick is ``/dev/sda``: every whole block device is read-only probed, and a
machine where more than one device carries an HFOC journal is refused by name
instead of picking one.

The output is one file, ``HFOC-DIAG-<session>.txt``: the newest *complete*
diagnostic session - its BEGIN record, every record the kernel verified, and its
END record - with a short header naming the device, the journal placement and
the record count.  A session is complete only when its END record carries the
same identifier and the CRC over the records matches, so a save that was
interrupted is refused rather than presented as fresh.

Nothing here needs a package or the network: Python 3's standard library only,
the same reader the QEMU proofs use (``scripts/hfoc-install-log-read.py``), and
read-only opens of the devices.

Usage:
    sudo python3 scripts/hfoc-diag-read.py                  # find the stick
    sudo python3 scripts/hfoc-diag-read.py --disk /dev/sdb  # name it
    python3 scripts/hfoc-diag-read.py --disk stick.img      # an image
    ... --output DIR    where the .txt is written (default: the cwd)
    ... --json          the session as JSON instead of a .txt
    ... --list          only list the devices that carry an HFOC journal
"""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import sys
import zlib

READER = pathlib.Path(__file__).with_name("hfoc-install-log-read.py")
HOSTKEY = pathlib.Path(__file__).with_name("hfoc-ssh-hostkey.py")
SESSION_MARKER = re.compile(r"^\[DIAG\] id=([0-9A-Fa-f]{8}) state=(BEGIN|END)(?: |$)")
CONTINUATION = "[DIAG+] "
# The devices that are never a removable stick and never carry this journal.
SKIP_PREFIXES = ("loop", "ram", "zram", "dm-", "md", "sr", "fd")


def load_reader(path):
    spec = importlib.util.spec_from_file_location("hfoc_install_log_read", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load the journal reader at {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_hostkey(path):
    spec = importlib.util.spec_from_file_location("hfoc_ssh_hostkey", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load the host-key reader at {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def candidate_devices():
    """Every whole block device the kernel knows, by its own name from sysfs.

    A device is a candidate because sysfs says it has a device node, not because
    its name looks like a disk: the same rule finds `/dev/sdb`, `/dev/mmcblk0`
    and whatever the machine decided to call the stick.
    """
    devices = []
    for entry in sorted(pathlib.Path("/sys/block").iterdir()):
        name = entry.name
        if name.startswith(SKIP_PREFIXES):
            continue
        if not (entry / "device").exists():
            continue
        # ITEM-553: a stick is a USB mass-storage device.  The sysfs chain of an
        # internal disk (SATA, NVMe, eMMC) never passes through a USB port, so
        # requiring it keeps the probe on removable media - the whole point of
        # the search - and leaves every internal disk, `/dev/nvme0n1` included,
        # untouched by construction.
        if "/usb" not in os.path.realpath(entry / "device"):
            continue
        node = pathlib.Path("/dev") / name
        if node.exists():
            devices.append(node)
    return devices


def read_journal(reader, path):
    image = reader.Image(str(path))
    try:
        return reader.read_journal(image)
    finally:
        image.close()


def sessions_of(journal):
    """The diagnostic sessions in journal order, with the complete one named.

    A BEGIN opens a session and only a matching END (same identifier, matching
    record count and CRC) completes it.  The newest BEGIN decides what a reader
    may be shown: if it has no complete END, the save that wrote it was
    interrupted, and returning an older session would be exactly the "old report
    presented as fresh" the item forbids.
    """
    sessions = []
    current = None
    for record in journal["records"]:
        text = record["text"]
        marker = SESSION_MARKER.match(text)
        if marker is not None:
            identifier = int(marker.group(1), 16)
            state = marker.group(2)
            if state == "BEGIN":
                current = {
                    "id": identifier,
                    "begin_slot": record["slot"],
                    "begin_text": text,
                    "end_text": None,
                    "records": [],
                    "end": None,
                }
                sessions.append(current)
                continue
            if current is None or current["id"] != identifier:
                raise ValueError(
                    f"END at slot {record['slot']} does not belong to the "
                    f"session that is open")
            wanted_records, wanted_crc = parse_end(text)
            text_of_records = [entry["text"] for entry in current["records"]]
            computed = 0
            for line in text_of_records:
                computed = zlib.crc32(line.encode("ascii") + b"\n", computed)
            if wanted_records != len(text_of_records):
                raise ValueError(
                    f"session {identifier:08X} names {wanted_records} records "
                    f"but carries {len(text_of_records)}")
            if wanted_crc != computed:
                raise ValueError(
                    f"session {identifier:08X} CRC does not match: the END "
                    f"record says {wanted_crc:08X}, the records give "
                    f"{computed:08X}")
            current["end"] = record["slot"]
            current["end_text"] = text
            current["complete"] = True
            continue
        if current is not None and current["end"] is None:
            current["records"].append(record)
    return sessions


def parse_end(text):
    records = re.search(r" records=(\d+)", text)
    crc = re.search(r" crc=([0-9A-Fa-f]{8})", text)
    if records is None or crc is None:
        raise ValueError(f"END record is malformed: {text}")
    return int(records.group(1)), int(crc.group(1), 16)


def joined_lines(records):
    """The session's records as a reader wants them: a continuation belongs to
    the line before it, and every other record is a line of its own."""
    lines = []
    for record in records:
        text = record["text"]
        if text.startswith(CONTINUATION) and lines:
            lines[-1] += text[len(CONTINUATION):]
        else:
            lines.append(text)
    return lines


def newest_session(sessions):
    if not sessions:
        return None
    return sessions[-1]


def render(device, journal, session):
    lines = [
        "# HFOC diagnostic report",
        f"# device: {device}",
        f"# journal: {journal['placement']}, {journal['capacity_slots']} slots",
        f"# session: {session['id']:08X}",
        f"# begin slot: {session['begin_slot']}, end slot: {session['end']}",
        f"# records: {len(session['records'])}",
        "--------- session ---------",
    ]
    lines.append(session["begin_text"])
    lines.extend(joined_lines(session["records"]))
    lines.append(session["end_text"])
    return "\n".join(lines) + "\n"


def probe(path, reader):
    """What one device carries, or None when it is not an HFOC medium."""
    journal = read_journal(reader, path)
    if journal is None:
        return None
    sessions = sessions_of(journal)
    return {"path": str(path), "journal": journal, "sessions": sessions}


def describe(candidate):
    newest = newest_session(candidate["sessions"])
    if newest is None:
        return f"{candidate['path']}: journal, no diagnostic session"
    state = "complete" if newest.get("complete") else "INCOMPLETE"
    return (f"{candidate['path']}: journal, newest session "
            f"{newest['id']:08X} {state}")


def main(argv):
    parser = argparse.ArgumentParser(
        description="carry the HFOC diagnostic report off the boot stick")
    parser.add_argument("--disk", help="the medium (a device or a raw image)")
    parser.add_argument("--output", default=".",
                        help="directory for HFOC-DIAG-<session>.txt")
    parser.add_argument("--list", action="store_true",
                        help="only list the devices that carry an HFOC journal")
    parser.add_argument("--json", action="store_true",
                        help="the session as JSON instead of a .txt")
    args = parser.parse_args(argv)

    reader = load_reader(READER)
    candidates = []
    if args.disk:
        if not os.path.exists(args.disk):
            print(f"diag-read: {args.disk} does not exist", file=sys.stderr)
            return 2
        probe_paths = [pathlib.Path(args.disk)]
    else:
        probe_paths = candidate_devices()
        if not probe_paths:
            print("diag-read: no block device to probe", file=sys.stderr)
            return 2

    for path in probe_paths:
        try:
            candidate = probe(path, reader)
        except Exception as error:  # a probe that cannot read is not a medium
            if args.disk:
                print(f"diag-read: {path}: {error}", file=sys.stderr)
                return 2
            continue
        if candidate is not None:
            candidates.append(candidate)

    if args.list:
        if not candidates:
            print("diag-read: no HFOC medium found", file=sys.stderr)
            return 1
        for candidate in candidates:
            print(describe(candidate))
        return 0

    if not candidates:
        print("diag-read: no HFOC journal on any device - is the stick "
              "plugged in, and was `diag save` run on the machine?",
              file=sys.stderr)
        return 1
    if len(candidates) > 1 and not args.disk:
        print("diag-read: more than one device carries an HFOC journal; "
              "name the right one with --disk:", file=sys.stderr)
        for candidate in candidates:
            print("  " + describe(candidate), file=sys.stderr)
        return 1

    candidate = candidates[0]
    session = newest_session(candidate["sessions"])
    if session is None:
        print(f"diag-read: {candidate['path']} carries a journal but no "
              f"diagnostic session - run `diag save` on the machine",
              file=sys.stderr)
        return 1
    if not session.get("complete"):
        print(f"diag-read: the newest session ({session['id']:08X}) has no "
              f"verified END record: the save was interrupted.  Run `diag "
              f"save` again instead of trusting an older report.", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps({
            "device": str(candidate["path"]),
            "placement": candidate["journal"]["placement"],
            "session": session["id"],
            "begin_slot": session["begin_slot"],
            "end_slot": session["end"],
            "records": len(session["records"]),
            "lines": joined_lines(session["records"]),
        }, indent=2))
        return 0

    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / f"HFOC-DIAG-{session['id']:08X}.txt"
    session_text = render(candidate["path"], candidate["journal"], session)
    target.write_text(session_text, encoding="ascii")
    print(f"diag-read: {target}")
    print(f"diag-read: device {candidate['path']}, session "
          f"{session['id']:08X}, {len(session['records'])} records, verified")
    # ITEM-613: the same verified session carries the public SSH host key; the
    # key module validates it independently and writes the OpenSSH files.
    hostkey = load_hostkey(HOSTKEY)
    try:
        result = hostkey.export_session(
            joined_lines(session["records"]), output, session["id"])
    except hostkey.HostKeyError as error:
        print(f"diag-read: hostkey=REFUSED {error}", file=sys.stderr)
        return 1
    if result["state"] == "ABSENT":
        print("diag-read: hostkey=ABSENT reason=NO_HOSTKEY_RECORD "
              "(the diagnostic report is still valid)")
    elif result["state"] == "UNAVAILABLE":
        print(f"diag-read: hostkey=UNAVAILABLE reason={result['reason']}")
    else:
        print(f"diag-read: hostkey={result['fingerprint']} "
              f"pub={result['pub']} ({result['pub_state']})")
        if result.get("known_hosts"):
            print(f"diag-read: known_hosts={result['known_hosts']} "
                  f"({result['known_hosts_state']}) host={result['host']}")
        else:
            print("diag-read: known_hosts=UNAVAILABLE reason=NO_ADDRESS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
