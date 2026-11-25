#!/usr/bin/env python3
"""Host proof for the HFOC boot log reader (ITEM-410).

The appliance kernel persists its boot records into a fixed 262144-byte file on
the EFI System Partition, `\\EFI\\HFOC\\BOOTLOG.BIN`, so that a hardware run can
be read back afterwards - out of a raw image or from a mounted ESP - without any
HFOC tooling.  This proof is the reader's own witness: it builds the log files
byte for byte from the format the kernel's writer publishes, runs
`scripts/hfoc-esp-read.py` as a subprocess and asserts on the stdout, the
stderr and the exit status an operator and the acceptance harness see.  It
never imports the reader.

    python3 tests/appliance-boot-log-reader-host.py [--require-image]

Every case prints one PASS line; a failure prints FAIL with what was expected
and what came back, and the script exits non-zero.  The case that reads the log
out of the appliance's own GPT+FAT32 image - the
`build/test-appliance-boot-log-host.img` the C host proof builds - is skipped
with a SKIP line until that image exists
and is a failure under `--require-image`.  The image path of the reader is
exercised either way by a GPT+FAT32 image this proof builds itself.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
READER = os.path.join(ROOT, "scripts", "hfoc-esp-read.py")
DEFAULT_IMAGE = os.path.join(ROOT, "build", "test-appliance-boot-log-host.img")

SLOT = 512
SLOTS = 512
FILE_BYTES = SLOT * SLOTS
RECORDS = SLOTS - 1
TEXT_MAX = 440
BOOTLOG_PATH = "\\EFI\\HFOC\\BOOTLOG.BIN"

ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
BASIC_DATA_TYPE = bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7")


# --------------------------------------------------------------------------
# The boot log format, rebuilt here from its definition.
# --------------------------------------------------------------------------

def slot_of(line):
    """One slot: the meaningful bytes, a newline, then NUL padding."""
    if len(line) > SLOT - 1:
        raise AssertionError(
            f"fixture line of {len(line)} bytes does not fit a slot")
    return line + b"\n" + bytes(SLOT - len(line) - 1)


def header_slot(gen=0, next_sequence=1, cursor=0, dropped=0, written=0,
                slots=SLOTS, records=RECORDS, slot_size=SLOT, version=1,
                magic=b"HFOC-BOOTLOG"):
    prefix = (magic + f" v={version}".encode("ascii") +
              f" slot={slot_size} slots={slots} "
              f"records={records}".encode("ascii") +
              f" gen={gen:010d} next={next_sequence:020d}".encode("ascii") +
              f" cursor={cursor:010d} dropped={dropped:010d}".encode("ascii") +
              f" written={written:020d} crc=".encode("ascii"))
    crc = zlib.crc32(prefix) & 0xFFFFFFFF
    return slot_of(prefix + f"{crc:08X}".encode("ascii"))


def record_slot(sequence, generation, ticks, text, crc=None):
    """A record slot whose CRC covers the field header and the text."""
    if isinstance(text, str):
        text = text.encode("ascii")
    prefix = f"@{sequence:020d} g{generation:010d} t{ticks:020d} " \
             f"l{len(text):03d} c".encode("ascii")
    value = zlib.crc32(prefix + text) & 0xFFFFFFFF if crc is None else crc
    return slot_of(prefix + f"{value:08X}".encode("ascii") + b" " + text)


def record_line(sequence, generation, ticks, text):
    """What the reader has to print for that record."""
    return f"#{sequence:020d} gen={generation} ticks={ticks} {text}"


def log_bytes(entries, count=SLOTS):
    """A file of `count` slots; `entries` holds the written ones' bytes."""
    data = bytearray(bytes(SLOT * count))
    for index, content in entries.items():
        if index >= count or len(content) != SLOT:
            raise AssertionError(f"fixture slot {index} does not fit a "
                                 f"{count}-slot file")
        data[index * SLOT:(index + 1) * SLOT] = content
    return bytes(data)


def sequential_log(count=5, generation=0, first=1):
    """`count` records in slots 1..count with a matching header."""
    entries = {0: header_slot(gen=generation, next_sequence=first + count,
                              cursor=count, written=count)}
    for offset in range(count):
        sequence = first + offset
        entries[1 + offset] = record_slot(sequence, generation, 100 * sequence,
                                          f"record {sequence}")
    return entries


def write_fixture(work, name, data):
    path = os.path.join(work, name)
    with open(path, "wb") as handle:
        handle.write(data)
    return path


# --------------------------------------------------------------------------
# A GPT disk with FAT32 volumes, built here so that the image path of the
# reader is proven even before the appliance's own image exists.
# --------------------------------------------------------------------------

def short_name_bytes(name):
    base, dot, extension = name.partition(".")
    if not dot:
        extension = ""
    return base.upper()[:8].ljust(8).encode("ascii") + \
        extension.upper()[:3].ljust(3).encode("ascii")


def short_display(name):
    """What an 8.3 entry can say: a longer name needs a VFAT long name."""
    base, dot, extension = name.partition(".")
    display = base.upper()[:8].rstrip()
    if dot:
        display += "." + extension.upper()[:3].rstrip()
    return display


def short_alias(name):
    """The 8.3 name a long name hides behind (what the medium carries)."""
    base, dot, extension = name.partition(".")
    alias = base.upper()[:6] + "~1"
    if dot:
        alias += "." + extension.upper()[:3]
    return alias


def long_name_entries(name, short):
    """The VFAT long-name entries that precede a short directory entry."""
    units = [ord(character) for character in name] + [0x0000]
    while len(units) % 13:
        units.append(0xFFFF)
    checksum = 0
    for byte in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
    count = len(units) // 13
    out = bytearray()
    for sequence in range(count, 0, -1):
        chunk = units[(sequence - 1) * 13:sequence * 13]
        entry = bytearray(32)
        entry[0] = sequence | (0x40 if sequence == count else 0x00)
        entry[11] = 0x0F
        entry[13] = checksum
        position = 0
        for start, length in ((1, 10), (14, 12), (28, 4)):
            for step in range(0, length, 2):
                struct.pack_into("<H", entry, start + step, chunk[position])
                position += 1
        out += entry
    return bytes(out)


def fat32_volume(tree):
    """A one-sector-per-cluster FAT32 volume carrying `tree`.

    `tree` maps a name to bytes (a file) or to another dict (a directory).  The
    names are 8.3, which is what `\\EFI\\HFOC\\BOOTLOG.BIN` is; the volume is
    exactly what the reader parses - a boot sector with a BPB, two FATs and the
    clusters the directory entries chain to.
    """
    reserved = 32
    fat_count = 2
    fat_sectors = 5
    entries_per_fat = fat_sectors * SLOT // 4
    data_start = reserved + fat_count * fat_sectors
    total_sectors = data_start + entries_per_fat
    volume = bytearray(bytes(total_sectors * SLOT))
    fat = [0] * entries_per_fat
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF
    first_free = [2]

    def cluster_offset(index):
        return (data_start + index - 2) * SLOT

    def allocate(count):
        first = first_free[0]
        for step in range(count):
            first_free[0] += 1
            fat[first + step] = \
                first + step + 1 if step + 1 < count else 0x0FFFFFFF
        return first

    def store(first, payload):
        index = first
        offset = 0
        while True:
            volume[cluster_offset(index):cluster_offset(index) + SLOT] = \
                payload[offset:offset + SLOT]
            offset += SLOT
            if fat[index] >= 0x0FFFFFF8:
                break
            index = fat[index]

    def directory(entries):
        out = bytearray()
        for name, attributes, first, size in entries:
            stored = name
            if short_display(name) != name.upper():
                stored = short_alias(name)
                out += long_name_entries(name, short_name_bytes(stored))
            entry = bytearray(32)
            entry[0:11] = short_name_bytes(stored)
            entry[11] = attributes
            entry[20:22] = struct.pack("<H", (first >> 16) & 0xFFFF)
            entry[26:28] = struct.pack("<H", first & 0xFFFF)
            entry[28:32] = struct.pack("<I", size)
            out += entry
        while len(out) % SLOT:
            out += b"\x00"
        return bytes(out)

    def place(node):
        entries = []
        for name in sorted(node):
            child = node[name]
            if isinstance(child, dict):
                entries.append((name, 0x10, place(child), 0))
            else:
                first = allocate(max(1, (len(child) + SLOT - 1) // SLOT))
                store(first, child)
                entries.append((name, 0x20, first, len(child)))
        first = allocate(1)
        store(first, directory(entries))
        return first

    root_cluster = place(tree)
    boot = bytearray(SLOT)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"HFOC    "
    boot[11:13] = struct.pack("<H", SLOT)
    boot[13] = 1
    boot[14:16] = struct.pack("<H", reserved)
    boot[16] = fat_count
    boot[32:36] = struct.pack("<I", total_sectors)
    boot[36:40] = struct.pack("<I", fat_sectors)
    boot[44:48] = struct.pack("<I", root_cluster)
    boot[510:512] = b"\x55\xaa"
    volume[0:SLOT] = boot
    fats = b"".join(struct.pack("<I", value) for value in fat)
    for copy in range(fat_count):
        start = (reserved + copy * fat_sectors) * SLOT
        volume[start:start + len(fats)] = fats
    return bytes(volume)


def enlarge_file_entry(volume, name, extra):
    """Claim `extra` bytes more than the cluster chain holds."""
    needle = short_name_bytes(name)
    offset = 0
    while True:
        offset = volume.find(needle, offset)
        if offset < 0:
            raise AssertionError(f"no {name} entry in the fixture volume")
        if volume[offset + 11] == 0x20:
            break
        offset += 1
    data = bytearray(volume)
    size = struct.unpack_from("<I", data, offset + 28)[0]
    struct.pack_into("<I", data, offset + 28, size + extra)
    return bytes(data)


def gpt_disk(volumes):
    """A GPT disk carrying `volumes`: [(type_guid, bytes)] in partition order.

    Returns (image, placement) where placement carries each partition's first
    LBA.  The primary entry array sits where a real tool puts it and a backup
    GPT closes the disk, so the image is a disk and not only a header.
    """
    entry_count = 128
    entry_size = 128
    array_sectors = entry_count * entry_size // SLOT
    first_usable = 2 + array_sectors
    placed = []
    lba = first_usable
    for type_guid, payload in volumes:
        sectors = (len(payload) + SLOT - 1) // SLOT
        placed.append((type_guid, lba, sectors, payload))
        lba += sectors
    last_usable = lba - 1
    total = lba + array_sectors + 1
    disk = bytearray(bytes(total * SLOT))

    array = bytearray(bytes(entry_count * entry_size))
    for index, (type_guid, first, sectors, _payload) in enumerate(placed):
        entry = bytearray(entry_size)
        entry[0:16] = type_guid
        entry[16:32] = struct.pack("<H", index + 1) * 8
        entry[32:40] = struct.pack("<Q", first)
        entry[40:48] = struct.pack("<Q", first + sectors - 1)
        name = f"HFOC-{index}".encode("utf-16-le")
        entry[56:56 + len(name)] = name
        array[index * entry_size:(index + 1) * entry_size] = entry
    disk[2 * SLOT:2 * SLOT + len(array)] = array

    header = bytearray(SLOT)
    header[0:8] = b"EFI PART"
    header[8:12] = struct.pack("<I", 0x00010000)
    header[12:16] = struct.pack("<I", 92)
    header[24:32] = struct.pack("<Q", 1)
    header[32:40] = struct.pack("<Q", total - 1)
    header[40:48] = struct.pack("<Q", first_usable)
    header[48:56] = struct.pack("<Q", last_usable)
    header[56:72] = bytes(range(16))
    header[72:80] = struct.pack("<Q", 2)
    header[80:84] = struct.pack("<I", entry_count)
    header[84:88] = struct.pack("<I", entry_size)
    header[88:92] = struct.pack("<I", zlib.crc32(bytes(array)) & 0xFFFFFFFF)
    header[16:20] = struct.pack(
        "<I", zlib.crc32(bytes(header[:92])) & 0xFFFFFFFF)
    disk[SLOT:2 * SLOT] = header

    backup = bytearray(header)
    backup[24:32] = struct.pack("<Q", total - 1)
    backup[32:40] = struct.pack("<Q", 1)
    backup[72:80] = struct.pack("<Q", total - array_sectors)
    backup[16:20] = b"\x00\x00\x00\x00"
    backup[16:20] = struct.pack(
        "<I", zlib.crc32(bytes(backup[:92])) & 0xFFFFFFFF)
    disk[(total - array_sectors) * SLOT:
         (total - array_sectors) * SLOT + len(array)] = array
    disk[(total - 1) * SLOT:total * SLOT] = backup

    mbr = bytearray(SLOT)
    mbr[446:450] = b"\x00\x02\x00\xEE"
    mbr[450:454] = b"\xFF\xFF\xFF\xFF"
    mbr[454:458] = struct.pack("<I", 1)
    mbr[458:462] = struct.pack("<I", min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    disk[0:SLOT] = mbr

    for _type_guid, first, _sectors, payload in placed:
        disk[first * SLOT:first * SLOT + len(payload)] = payload
    return bytes(disk), placed


# --------------------------------------------------------------------------
# The proof itself.
# --------------------------------------------------------------------------

class Proof:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, label, condition, detail=""):
        if condition:
            self.passed += 1
            print(f"PASS: {label}")
        else:
            self.failed += 1
            print(f"FAIL: {label}" + (f": {detail}" if detail else ""))
        return bool(condition)

    def equal(self, label, actual, expected):
        return self.check(label, actual == expected,
                          f"expected {expected!r}, got {actual!r}")

    def no_traceback(self, label, completed):
        text = completed.stderr.decode("utf-8", "replace")
        return self.check(f"{label}: no traceback escapes",
                          "Traceback" not in text, text)


def run_reader(*arguments):
    return subprocess.run([sys.executable, READER, *arguments],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def stdout_of(completed):
    return completed.stdout.decode("utf-8", "replace")


def stderr_of(completed):
    return completed.stderr.decode("utf-8", "replace")


def summary_of(completed):
    for line in stdout_of(completed).splitlines():
        if line.startswith("boot-log: path="):
            return line
    return None


def records_of(completed):
    return [line for line in stdout_of(completed).splitlines()
            if line.startswith("#")]


def field_of(summary, name):
    if summary is None:
        return None
    for item in summary.split():
        if item.startswith(name + "="):
            return item[len(name) + 1:]
    return None


def sequences_of(completed):
    return [int(line[1:21]) for line in records_of(completed)]


def case_1_empty_log(proof, work):
    """An all-zero log is a log without records, not a failure."""
    path = write_fixture(work, "empty-log.bin", bytes(FILE_BYTES))
    done = run_reader("--bootlog-file", path)
    summary = summary_of(done)
    proof.equal("case 1 (all-zero 262144-byte log): exit status",
                done.returncode, 0)
    proof.equal("case 1: header=MISSING", field_of(summary, "header"),
                "MISSING")
    proof.equal("case 1: the geometry is the file's own",
                (field_of(summary, "bytes"), field_of(summary, "slots"),
                 field_of(summary, "records")),
                (str(FILE_BYTES), str(SLOTS), str(RECORDS)))
    proof.equal("case 1: shown=0", field_of(summary, "shown"), "0")
    proof.check("case 1: 'boot-log: no records'",
                "boot-log: no records" in stdout_of(done))
    proof.equal("case 1: nothing is printed as a record", records_of(done), [])
    proof.no_traceback("case 1", done)


def case_2_sequential(proof, work):
    """Five records in slots 1..5 come back in order, with their numbers."""
    path = write_fixture(work, "sequential.bin", log_bytes(sequential_log()))
    done = run_reader("--bootlog-file", path)
    summary = summary_of(done)
    expected = [record_line(sequence, 0, 100 * sequence, f"record {sequence}")
                for sequence in range(1, 6)]
    proof.equal("case 2 (five sequential records): exit status",
                done.returncode, 0)
    proof.equal("case 2: the five record lines, oldest first",
                records_of(done), expected)
    proof.equal("case 2: header=OK", field_of(summary, "header"), "OK")
    proof.equal("case 2: wrapped=NO", field_of(summary, "wrapped"), "NO")
    proof.equal("case 2: oldest=1 newest=5 shown=5",
                (field_of(summary, "oldest"), field_of(summary, "newest"),
                 field_of(summary, "shown")), ("1", "5", "5"))
    proof.equal("case 2: the header's own counters are printed",
                (field_of(summary, "dropped"), field_of(summary, "written")),
                ("0", "5"))
    proof.equal("case 2: nothing is wrong, so stderr stays empty",
                stderr_of(done), "")
    proof.no_traceback("case 2", done)


def case_3_wrapped(proof, work):
    """Only the newest contiguous run is shown after a wrap; leftovers are
    not."""
    entries = {0: header_slot(gen=1, next_sequence=611, cursor=11,
                              written=611)}
    for offset, sequence in enumerate(range(600, 611)):
        entries[1 + offset] = record_slot(sequence, 1, 10 * sequence,
                                          f"new {sequence}")
    for offset, sequence in enumerate(range(90, 101)):
        entries[12 + offset] = record_slot(sequence, 0, 10 * sequence,
                                           f"old {sequence}")
    path = write_fixture(work, "wrapped.bin", log_bytes(entries))
    done = run_reader("--bootlog-file", path)
    summary = summary_of(done)
    expected = [record_line(sequence, 1, 10 * sequence, f"new {sequence}")
                for sequence in range(600, 611)]
    proof.equal("case 3 (wrapped log): exit status", done.returncode, 0)
    proof.equal("case 3: only the newest run, oldest first",
                records_of(done), expected)
    proof.equal("case 3: wrapped=YES", field_of(summary, "wrapped"), "YES")
    proof.equal("case 3: oldest=600 newest=610 shown=11",
                (field_of(summary, "oldest"), field_of(summary, "newest"),
                 field_of(summary, "shown")), ("600", "610", "11"))
    proof.check("case 3: the leftovers of the previous wrap are not printed",
                "old 90" not in stdout_of(done) and
                "old 100" not in stdout_of(done), stdout_of(done))
    proof.check("case 3: they are counted in a warning",
                "11 valid record(s) are outside the shown run"
                in stderr_of(done), stderr_of(done))
    proof.no_traceback("case 3", done)


def case_4_torn_tail(proof, work):
    """A power cut inside the last write is reported, not crashed on."""
    whole = log_bytes(sequential_log())
    cut = write_fixture(work, "cut-inside-slot.bin", whole[:5 * SLOT + 256])
    done = run_reader("--bootlog-file", cut)
    proof.equal("case 4 (file cut inside its fifth slot): exit status",
                done.returncode, 0)
    proof.check("case 4: the incomplete slot is warned about",
                "is not a whole number of 512-byte slots" in stderr_of(done),
                stderr_of(done))
    proof.equal("case 4: the four complete records are shown",
                sequences_of(done), [1, 2, 3, 4])
    proof.check("case 4: the cut record's text is never printed",
                "record 5" not in stdout_of(done), stdout_of(done))
    proof.no_traceback("case 4", done)

    partial = bytearray(whole)
    partial[5 * SLOT + 40:6 * SLOT] = bytes(SLOT - 40)
    torn = write_fixture(work, "half-written-slot.bin", bytes(partial))
    done = run_reader("--bootlog-file", torn)
    proof.equal("case 4b (a slot written only in part): exit status",
                done.returncode, 0)
    proof.check("case 4b: the torn slot is warned about, naming slot 5",
                "1 torn or unparseable non-empty slot(s), the first at slot 5"
                in stderr_of(done), stderr_of(done))
    proof.equal("case 4b: the records before it are still shown",
                sequences_of(done), [1, 2, 3, 4])
    proof.check("case 4b: the torn record's text is never printed",
                "record 5" not in stdout_of(done), stdout_of(done))
    proof.no_traceback("case 4b", done)


def case_5_header_claims_more(proof, work):
    """A header that overstates the file is mistrusted, and what is there is
    read."""
    entries = {0: header_slot(gen=0, next_sequence=1000000, cursor=2,
                              dropped=0, written=999999)}
    entries[1] = record_slot(1, 0, 100, "first")
    entries[2] = record_slot(2, 0, 200, "second")
    path = write_fixture(work, "overclaiming-header.bin",
                         log_bytes(entries, count=8))
    done = run_reader("--bootlog-file", path)
    summary = summary_of(done)
    stderr = stderr_of(done)
    proof.equal("case 5 (a header claiming more than the file holds): "
                "exit status", done.returncode, 0)
    proof.equal("case 5: header=MISMATCH", field_of(summary, "header"),
                "MISMATCH")
    proof.equal("case 5: the file's real geometry is printed",
                (field_of(summary, "bytes"), field_of(summary, "slots"),
                 field_of(summary, "records")),
                (str(8 * SLOT), "8", "7"))
    proof.equal("case 5: the two records that really are there",
                records_of(done),
                [record_line(1, 0, 100, "first"),
                 record_line(2, 0, 200, "second")])
    proof.equal("case 5: the counters of a mistrusted header are unknown",
                (field_of(summary, "dropped"), field_of(summary, "written")),
                ("?", "?"))
    proof.check("case 5: the disagreement with the file is warned about",
                "header claims slot=512 slots=512 records=511" in stderr,
                stderr)
    proof.check("case 5: its written/next are not taken on trust",
                "written=999999" in stderr and "next=1000000" in stderr,
                stderr)
    proof.no_traceback("case 5", done)


def case_6_garbage(proof, work):
    """Noise and a wrong magic are refused cleanly, with nothing on stdout."""
    noise = write_fixture(work, "noise.bin",
                          bytes(((index * 7) % 251) + 1
                                for index in range(FILE_BYTES)))
    done = run_reader("--bootlog-file", noise)
    proof.equal("case 6 (262144 bytes of noise): exit status",
                done.returncode, 2)
    proof.check("case 6: a clean refusal on stderr",
                stderr_of(done).startswith("hfoc-esp-read: ") and
                "is not an HFOC boot log" in stderr_of(done), stderr_of(done))
    proof.equal("case 6: nothing at all on stdout", done.stdout, b"")
    proof.no_traceback("case 6", done)

    wrong = write_fixture(work, "wrong-magic.bin",
                          header_slot(magic=b"XFOC-BOOTLOG"))
    done = run_reader("--bootlog-file", wrong)
    proof.equal("case 6b (a log line with another magic): exit status",
                done.returncode, 2)
    proof.check("case 6b: the refusal names the log it wanted",
                "is not an HFOC boot log" in stderr_of(done), stderr_of(done))
    proof.equal("case 6b: nothing that looks like a record on stdout",
                records_of(done), [])
    proof.no_traceback("case 6b", done)


def case_7_short_files(proof, work):
    """An empty file, a 100-byte file and a missing path are all refusals."""
    empty = write_fixture(work, "zero-bytes.bin", b"")
    done = run_reader("--bootlog-file", empty)
    proof.equal("case 7 (a 0-byte file): exit status", done.returncode, 2)
    proof.check("case 7: it says the file is shorter than a slot",
                "shorter than one 512-byte slot" in stderr_of(done),
                stderr_of(done))
    proof.equal("case 7: nothing on stdout", done.stdout, b"")
    proof.no_traceback("case 7", done)

    stub = write_fixture(work, "hundred-bytes.bin", bytes(100))
    done = run_reader("--bootlog-file", stub)
    proof.equal("case 7b (a 100-byte file): exit status", done.returncode, 2)
    proof.check("case 7b: it names the size it found",
                "is 100 bytes, shorter than one 512-byte slot"
                in stderr_of(done), stderr_of(done))
    proof.equal("case 7b: nothing on stdout", done.stdout, b"")
    proof.no_traceback("case 7b", done)

    done = run_reader("--bootlog-file", os.path.join(work, "not-there.bin"))
    proof.equal("case 7c (a path that is not there): exit status",
                done.returncode, 2)
    proof.check("case 7c: a clean message", "cannot read" in stderr_of(done),
                stderr_of(done))
    proof.no_traceback("case 7c", done)


def case_8_torn_slots(proof, work):
    """A non-printable byte and a bad CRC are torn: warned about, never
    printed."""
    text = b"crc mismatch here"
    prefix = f"@{2:020d} g{0:010d} t{200:020d} " \
             f"l{len(text):03d} c".encode("ascii")
    wrong_crc = (zlib.crc32(prefix + text) & 0xFFFFFFFF) ^ 0xFFFFFFFF
    entries = {0: header_slot(gen=0, next_sequence=1, cursor=2, written=2),
               1: record_slot(1, 0, 100, b"high byte \xff here"),
               2: record_slot(2, 0, 200, text, crc=wrong_crc)}
    path = write_fixture(work, "torn-slots.bin", log_bytes(entries))
    done = run_reader("--bootlog-file", path)
    summary = summary_of(done)
    stderr = stderr_of(done)
    proof.equal("case 8 (a non-printable byte and a bad CRC): exit status",
                done.returncode, 0)
    proof.equal("case 8: header=OK, shown=0",
                (field_of(summary, "header"), field_of(summary, "shown")),
                ("OK", "0"))
    proof.check("case 8: 'boot-log: no records'",
                "boot-log: no records" in stdout_of(done))
    proof.equal("case 8: one warning counts both and names the first slot",
                stderr.strip(),
                "boot-log: warning: 2 torn or unparseable non-empty slot(s), "
                "the first at slot 1")
    proof.check("case 8: the non-printable text is never printed",
                b"high byte" not in done.stdout and b"\xff" not in done.stdout,
                repr(done.stdout))
    proof.check("case 8: the CRC-mismatched text is never printed",
                b"crc mismatch here" not in done.stdout, repr(done.stdout))
    proof.no_traceback("case 8", done)


def image_case_message(proof, image, require_image, label):
    """True when the case may run; a missing image is a skip, or a failure."""
    if os.path.exists(image):
        return True
    if require_image:
        proof.check(f"{label} (--require-image): {image} exists", False,
                    "not built yet: run the C host proof that creates it")
    else:
        print(f"SKIP: {image} not built yet")
    return False


def case_9_image(proof, image, require_image):
    """The log out of the appliance's own image, read with the positional
    form."""
    if not image_case_message(proof, image, require_image,
                              "case 9 (--bootlog inside the built image)"):
        return
    done = run_reader(image, "--bootlog")
    summary = summary_of(done)
    lines = records_of(done)
    proof.equal("case 9 (--bootlog inside the built image): exit status",
                done.returncode, 0)
    proof.check("case 9: a summary line names the log inside the ESP",
                summary is not None and f"path={BOOTLOG_PATH}" in summary,
                str(summary))
    proof.check(f"case 9: at least three records (found {len(lines)})",
                len(lines) >= 3)
    sequences = sequences_of(done)
    proof.check("case 9: the sequence numbers increase consecutively",
                all(later == earlier + 1
                    for earlier, later in zip(sequences, sequences[1:])),
                str(sequences))
    if lines:
        print(f"case 9: first record: {lines[0]}")
        print(f"case 9: last record:  {lines[-1]}")
    proof.no_traceback("case 9", done)


def case_10_both_ways(proof, image, require_image, work):
    """The same bytes read out of the image and out of a mounted-ESP copy."""
    if not image_case_message(proof, image, require_image,
                              "case 10 (the ESP's log read both ways)"):
        return
    extracted = run_reader("--disk", image, "--path", BOOTLOG_PATH)
    proof.equal("case 10: --path extracts the log from the ESP",
                extracted.returncode, 0)
    if extracted.returncode != 0:
        return
    copy = write_fixture(work, "extracted-bootlog.bin", extracted.stdout)
    from_image = run_reader(image, "--bootlog")
    from_file = run_reader("--bootlog-file", copy)
    proof.equal("case 10: --bootlog-file reads the copy",
                from_file.returncode, 0)
    proof.equal("case 10: the record lines are identical",
                records_of(from_file), records_of(from_image))
    names = ("bytes", "slots", "records", "header", "gen", "wrapped", "oldest",
             "newest", "shown", "dropped", "written")
    proof.equal("case 10: every summary field but the path agrees",
                [field_of(summary_of(from_file), name) for name in names],
                [field_of(summary_of(from_image), name) for name in names])
    proof.no_traceback("case 10", from_file)


def case_11_synthetic_image(proof, work):
    """The image path of the reader, on a GPT+FAT32 disk built here."""
    log = log_bytes(sequential_log(count=6))
    elsewhere = log_bytes({0: header_slot(next_sequence=2, written=1),
                           1: record_slot(1, 0, 5, "not the ESP log")})
    medium_conf = b"TIMEOUT=3\npath: boot():/boot/hfoc-kernel.elf\n"
    no_log_here = fat32_volume({"EFI": {"HFOC": {"README.TXT": b"no log\n"}}})
    carries_it = fat32_volume({
        "EFI": {"HFOC": {"BOOTLOG.BIN": log}},
        "boot": {"limine": {"limine.conf": medium_conf}},
    })
    other_volume = fat32_volume({"EFI": {"HFOC": {"BOOTLOG.BIN": elsewhere}}})
    image = write_fixture(
        work, "synthetic-gpt.img",
        gpt_disk([(BASIC_DATA_TYPE, other_volume),
                  (ESP_TYPE, no_log_here),
                  (ESP_TYPE, carries_it)])[0])

    done = run_reader(image, "--bootlog")
    expected = [record_line(sequence, 0, 100 * sequence, f"record {sequence}")
                for sequence in range(1, 7)]
    proof.equal("case 11 (a GPT+FAT32 image built here): exit status",
                done.returncode, 0)
    proof.equal("case 11: the log comes from the ESP that carries it",
                records_of(done), expected)
    proof.check("case 11: the non-ESP partition's own log is not used",
                "not the ESP log" not in stdout_of(done), stdout_of(done))
    proof.no_traceback("case 11", done)

    done = run_reader(image, "--bootlog", "--partition-index", "2")
    proof.equal("case 11: --partition-index 2 selects that ESP",
                (done.returncode, records_of(done)), (0, expected))

    done = run_reader("--disk", image, "--partition-index", "2",
                      "--path", "boot\\limine\\limine.conf")
    proof.equal("case 11: the same volume is a FAT32 volume with a VFAT long "
                "name, which --path still reads byte for byte",
                (done.returncode, done.stdout), (0, medium_conf))

    done = run_reader(image, "--bootlog", "--partition-index", "1")
    proof.equal("case 11: --partition-index 1 has no log in it",
                done.returncode, 2)
    proof.check("case 11: and says so with the path it looked for",
                f"no HFOC boot log at {BOOTLOG_PATH}" in stderr_of(done),
                stderr_of(done))
    proof.no_traceback("case 11", done)

    done = run_reader(image, "--bootlog", "--partition-index", "0")
    proof.equal("case 11: a named partition is used whatever its type",
                (done.returncode, records_of(done)),
                (0, [record_line(1, 0, 5, "not the ESP log")]))

    absent = write_fixture(
        work, "synthetic-gpt-no-log.img",
        gpt_disk([(BASIC_DATA_TYPE, other_volume),
                  (ESP_TYPE, no_log_here),
                  (ESP_TYPE, no_log_here)])[0])
    done = run_reader(absent, "--bootlog")
    proof.equal("case 11: no ESP carries the file, so it is a refusal",
                done.returncode, 2)
    proof.check("case 11: the refusal names the file and the two ESPs checked",
                f"no HFOC boot log at {BOOTLOG_PATH} in 2 EFI System "
                f"Partition(s) checked" in stderr_of(done), stderr_of(done))
    proof.no_traceback("case 11", done)

    short = fat32_volume({"EFI": {"HFOC": {"BOOTLOG.BIN": log}}})
    image = write_fixture(
        work, "synthetic-gpt-short.img",
        gpt_disk([(ESP_TYPE, enlarge_file_entry(short, "BOOTLOG.BIN",
                                                4 * SLOT))])[0])
    done = run_reader(image, "--bootlog")
    proof.equal("case 11: a chain shorter than the entry claims: exit status",
                done.returncode, 0)
    proof.check("case 11: the short chain is warned about",
                "the image ends inside the cluster chain" in stderr_of(done),
                stderr_of(done))
    proof.equal("case 11: and what is there is parsed",
                sequences_of(done), [1, 2, 3, 4, 5, 6])
    proof.no_traceback("case 11", done)


def case_12_usage(proof, work):
    """The command line itself: one image, one job, or a clean refusal."""
    path = write_fixture(work, "usage-log.bin", bytes(FILE_BYTES))
    done = subprocess.run([sys.executable, READER],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    proof.equal("case 12 (no arguments at all): exit status",
                done.returncode, 2)
    proof.check("case 12: a message on stderr, not a traceback",
                stderr_of(done).startswith("hfoc-esp-read: "),
                stderr_of(done))
    proof.no_traceback("case 12", done)

    done = run_reader(path, "--disk", path + ".other", "--bootlog")
    proof.equal("case 12: an image named twice, differently, is a usage error",
                done.returncode, 2)
    proof.no_traceback("case 12", done)

    done = run_reader("--bootlog")
    proof.equal("case 12: --bootlog without an image is a usage error",
                done.returncode, 2)
    proof.no_traceback("case 12", done)

    done = run_reader(path, "--disk", path, "--bootlog-file", path)
    proof.equal("case 12: --bootlog-file and an image are a usage error",
                done.returncode, 2)
    proof.no_traceback("case 12", done)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-image", action="store_true",
                        help="fail instead of skipping when "
                             f"{DEFAULT_IMAGE} does not exist")
    parser.add_argument("--image", default=DEFAULT_IMAGE,
                        help="the GPT+FAT32 image the C host proof builds")
    arguments = parser.parse_args()

    if not os.path.exists(READER):
        print(f"FAIL: the reader is missing: {READER}")
        return 1

    proof = Proof()
    work = tempfile.mkdtemp(prefix="hfoc-bootlog-reader-host-")
    try:
        case_1_empty_log(proof, work)
        case_2_sequential(proof, work)
        case_3_wrapped(proof, work)
        case_4_torn_tail(proof, work)
        case_5_header_claims_more(proof, work)
        case_6_garbage(proof, work)
        case_7_short_files(proof, work)
        case_8_torn_slots(proof, work)
        case_9_image(proof, arguments.image, arguments.require_image)
        case_10_both_ways(proof, arguments.image, arguments.require_image, work)
        case_11_synthetic_image(proof, work)
        case_12_usage(proof, work)
    finally:
        if proof.failed:
            print(f"fixtures kept in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    print(f"appliance-boot-log-reader-host: {proof.passed} checks passed, "
          f"{proof.failed} failed")
    return 1 if proof.failed else 0


if __name__ == "__main__":
    sys.exit(main())
