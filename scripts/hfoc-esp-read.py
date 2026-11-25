#!/usr/bin/env python3
"""Read one file out of the EFI System Partition of a raw disk image.

The install proofs have to read back what the installer actually put on the
disk - the installed boot configuration, the generated menu - from the raw
image, independently of the kernel that wrote it.  The ESP is a FAT32 volume
the kernel's own ESP writer produced (long names, VFAT chains, one sector per
cluster), so this tool parses the GPT, the volume's BIOS parameter block and
the file's cluster chain itself and writes the file's bytes to stdout.

Nothing here comes from the kernel: the partition is selected by its GPT type
GUID, the geometry from the BPB, and the bytes from the raw sectors.  A proof
that compares this output with the bytes the medium sealed is a byte-for-byte
comparison of the two, not a restatement of a record.

Usage:
  hfoc-esp-read.py --disk IMAGE --path 'boot\\limine\\limine.conf'
      [--partition-index N]
  hfoc-esp-read.py IMAGE --bootlog [--partition-index N]
  hfoc-esp-read.py --bootlog-file /mnt/EFI/HFOC/BOOTLOG.BIN

`--partition-index` selects a partition by its index in the GPT entry array;
without it the first EFI System Partition is used.  The path is relative to the
volume root and may use `/` or `\\` as the separator.

`--bootlog` prints the appliance boot log the kernel keeps at
`\\EFI\\HFOC\\BOOTLOG.BIN` (ITEM-410): one header line, then one line per record
of the newest contiguous run, oldest first, each with its sequence number.  The
image path is the positional argument and `--disk` is its other spelling.  The
image is searched for the first EFI System Partition that actually carries the
file; `--partition-index N` names one partition instead.

`--bootlog-file` reads the very same log from a plain file, which is what a
human who mounted the ESP does:

  mount /dev/sda1 /mnt &&
      python3 scripts/hfoc-esp-read.py --bootlog-file /mnt/EFI/HFOC/BOOTLOG.BIN

A mounted ESP also shows the log with no HFOC tooling at all - every slot is a
NUL-padded ASCII line - but only this tool verifies the CRCs and picks the
newest run out of a wrapped log.  What it could not read is reported on stderr
as `boot-log: warning: ...`, so the log stays pipeable; warnings are not
failures.

The file is 512 slots of 512 bytes: slot 0 is the header, slots 1..511 are the
records, and every slot is one ASCII line closed by a newline and padded with
zeros.  A slot whose first byte is zero was never written.  A record whose CRC
does not match, whose length disagrees with the line or whose text is not
printable ASCII is a torn write and is never printed.  The exit status is 0
when an HFOC boot log was found and read, and 2 when the file is missing,
shorter than one slot or not an HFOC boot log.
"""

import argparse
import fcntl
import mmap
import os
import re
import stat
import struct
import sys
import zlib

# BLKGETSIZE64: the size of a block device in bytes, which `fstat` does not
# report - it answers zero for a device, and a zero-length mapping is EINVAL.
BLKGETSIZE64 = 0x80081272

SECTOR = 512
ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
FAT_ENTRY_SIZE = 4
CHAIN_LIMIT = 1 << 20

BOOTLOG_PATH = "\\EFI\\HFOC\\BOOTLOG.BIN"
BOOTLOG_SLOT = 512
BOOTLOG_SLOT_COUNT = 512
BOOTLOG_RECORD_CAPACITY = BOOTLOG_SLOT_COUNT - 1
BOOTLOG_TEXT_MAX = 440
# The log is 256 KiB; a file far larger than that is not one, and is read only
# this far so that a mistyped path cannot become a whole-disk read.
BOOTLOG_SCAN_LIMIT = 64 << 20
GPT_ENTRY_LIMIT = 1 << 20

# The header line, field by field, exactly as the kernel's writer renders it:
# the CRC covers everything before the CRC digits and the line ends in a
# newline.  The widths are the format's own, so a line that cannot be a version
# 1 header does not match at all.
HEADER_LINE = re.compile(
    rb"HFOC-BOOTLOG v=(?P<version>[0-9]{1,2})"
    rb" slot=(?P<slot>[0-9]{1,3})"
    rb" slots=(?P<slots>[0-9]{1,3})"
    rb" records=(?P<records>[0-9]{1,3})"
    rb" gen=(?P<gen>[0-9]{1,10})"
    rb" next=(?P<next>[0-9]{1,20})"
    rb" cursor=(?P<cursor>[0-9]{1,10})"
    rb" dropped=(?P<dropped>[0-9]{1,10})"
    rb" written=(?P<written>[0-9]{1,20})"
    rb" crc=(?P<crc>[0-9A-Fa-f]{8})\n")

RECORD_LINE = re.compile(
    rb"@(?P<seq>[0-9]{1,20})"
    rb" g(?P<gen>[0-9]{1,10})"
    rb" t(?P<ticks>[0-9]{1,20})"
    rb" l(?P<length>[0-9]{1,3})"
    rb" c(?P<crc>[0-9A-Fa-f]{8}) ")


def fail(message):
    print(f"hfoc-esp-read: {message}", file=sys.stderr)
    raise SystemExit(2)


def warn(message):
    """A refusal to trust one reading: the log is still printed."""
    print(f"boot-log: warning: {message}", file=sys.stderr)


class Volume:
    def __init__(self, disk, first_lba):
        self.disk = disk
        base = first_lba * SECTOR
        bpb = disk[base:base + SECTOR]
        if len(bpb) < SECTOR or bpb[510:512] != b"\x55\xaa":
            fail("the selected partition does not start with a boot sector")
        self.bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
        self.sectors_per_cluster = bpb[13]
        self.reserved = struct.unpack_from("<H", bpb, 14)[0]
        self.fat_count = bpb[16]
        self.sectors_per_fat = struct.unpack_from("<I", bpb, 36)[0]
        self.root_cluster = struct.unpack_from("<I", bpb, 44)[0]
        if self.bytes_per_sector != SECTOR or self.sectors_per_cluster == 0:
            fail("the ESP is not the one-sector-cluster FAT32 volume expected")
        if self.fat_count == 0 or self.sectors_per_fat == 0:
            fail("the ESP carries no file allocation table")
        self.cluster_bytes = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_start = base + self.reserved * self.bytes_per_sector
        self.data_start = base + (self.reserved +
                                  self.fat_count * self.sectors_per_fat) * \
            self.bytes_per_sector

    def next_cluster(self, index):
        offset = self.fat_start + index * FAT_ENTRY_SIZE
        return struct.unpack_from("<I", self.disk, offset)[0] & 0x0FFFFFFF

    def chain(self, start):
        out = []
        index = start
        while 2 <= index < 0x0FFFFFF8 and len(out) < CHAIN_LIMIT:
            out.append(index)
            index = self.next_cluster(index)
        return out

    def cluster_data(self, index):
        offset = self.data_start + (index - 2) * self.cluster_bytes
        return self.disk[offset:offset + self.cluster_bytes]

    def directory(self, cluster):
        data = b"".join(self.cluster_data(c) for c in self.chain(cluster))
        entries = []
        pending = []
        for offset in range(0, len(data) - 31, 32):
            entry = data[offset:offset + 32]
            if entry[0] == 0x00:
                break
            if entry[0] == 0xE5:
                pending = []
                continue
            if entry[11] == 0x0F:
                pending.append(entry)
                continue
            name = self.long_name(pending) or self.short_name(entry)
            pending = []
            entries.append((name, entry))
        return entries

    @staticmethod
    def short_name(entry):
        name = entry[0:8].decode("ascii", "replace").rstrip()
        extension = entry[8:11].decode("ascii", "replace").rstrip()
        return name + "." + extension if extension else name

    @staticmethod
    def long_name(entries):
        """Reassemble a VFAT long name.

        The entries are stored in reverse name order - the physically first
        entry carries the *last* characters and the highest sequence number with
        the 0x40 bit set - so they are read back in ascending sequence order.
        A 0x0000 UTF-16 unit ends the name.
        """
        if not entries:
            return None
        text = ""
        for entry in sorted(entries, key=lambda item: item[0] & 0x3F):
            for start, length in ((1, 10), (14, 12), (28, 4)):
                for index in range(start, start + length, 2):
                    unit = struct.unpack_from("<H", entry, index)[0]
                    if unit == 0x0000:
                        return text
                    text += chr(unit)
        return text

    def read(self, path):
        elements = [element for element in path.replace("\\", "/").split("/")
                    if element]
        if not elements:
            fail("empty path")
        current = self.root_cluster
        for element in elements[:-1]:
            entry = self.find(self.directory(current), element)
            if entry is None:
                fail(f"no {element!r} in the volume")
            if not entry[11] & 0x10:
                fail(f"{element!r} is not a directory")
            current = entry_cluster(entry)
        entry = self.find(self.directory(current), elements[-1])
        if entry is None:
            fail(f"no {elements[-1]!r} in the volume")
        if entry[11] & 0x10:
            fail(f"{elements[-1]!r} is a directory")
        length = struct.unpack_from("<I", entry, 28)[0]
        data = b"".join(self.cluster_data(c) for c in
                        self.chain(entry_cluster(entry)))
        return data[:length]

    @staticmethod
    def find(entries, wanted):
        for name, entry in entries:
            if name.upper() == wanted.upper():
                return entry
        return None

    def read_optional(self, path, limit):
        """Read a file that may not be there: (data, missing_bytes) or None.

        `read()` is the extract path behind `--path`, so a file the volume does
        not carry is a refusal there; the boot log is *searched* for, so for
        this method an absent file is an ordinary answer.  At most `limit`
        bytes are gathered and `missing_bytes` is how much of what the
        directory entry claims the image could not supply, which is how an
        image that ends inside the cluster chain is reported instead of
        crashing.
        """
        elements = [element for element in path.replace("\\", "/").split("/")
                    if element]
        if not elements:
            return None
        current = self.root_cluster
        for element in elements[:-1]:
            entry = self.find(self.directory(current), element)
            if entry is None or not entry[11] & 0x10:
                return None
            current = entry_cluster(entry)
        entry = self.find(self.directory(current), elements[-1])
        if entry is None or entry[11] & 0x10:
            return None
        claimed = struct.unpack_from("<I", entry, 28)[0]
        wanted = min(claimed, limit)
        chunks = []
        gathered = 0
        for index in self.chain(entry_cluster(entry)):
            chunk = self.cluster_data(index)
            if not chunk:
                break
            chunks.append(chunk)
            gathered += len(chunk)
            if gathered >= wanted:
                break
        data = b"".join(chunks)[:wanted]
        return (data, max(0, wanted - len(data)))


def entry_cluster(entry):
    """The first cluster of a FAT directory entry: high word first, then low."""
    return (struct.unpack_from("<H", entry, 20)[0] << 16) | \
        struct.unpack_from("<H", entry, 26)[0]


def partition_extents(disk, index):
    header = disk[SECTOR:2 * SECTOR]
    if header[0:8] != b"EFI PART":
        fail("no GPT header on the disk")
    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    if entry_count == 0 or entry_size < 128:
        fail("the GPT entry array is unusable")
    array = entries_lba * SECTOR
    extents = []
    for slot in range(entry_count):
        entry = disk[array + slot * entry_size:
                     array + (slot + 1) * entry_size]
        if entry[0:16] == b"\x00" * 16:
            continue
        extents.append((slot, entry[0:16],
                        struct.unpack_from("<Q", entry, 32)[0]))
    if index is not None:
        for slot, _type_guid, first in extents:
            if slot == index:
                return first
        fail(f"the GPT has no partition at index {index}")
    for _slot, type_guid, first in extents:
        if type_guid == ESP_TYPE:
            return first
    fail("the GPT carries no EFI System Partition")


def esp_partitions(disk):
    """The EFI System Partitions in GPT index order: [(index, first_lba)].

    `partition_extents()` above is the `--path` selection and answers with one
    partition or a refusal; the boot log is searched for, so the whole ordered
    list is what this needs.  It is parsed separately so that nothing here can
    change what `--path` reads or how it refuses.
    """
    header = disk[SECTOR:2 * SECTOR]
    if len(header) < 92 or header[0:8] != b"EFI PART":
        fail("no GPT header on the disk")
    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    if entry_count == 0 or entry_count > GPT_ENTRY_LIMIT or entry_size < 128:
        fail("the GPT entry array is unusable")
    array = entries_lba * SECTOR
    found = []
    for slot in range(entry_count):
        entry = disk[array + slot * entry_size:array + (slot + 1) * entry_size]
        if len(entry) < entry_size:
            break
        if entry[0:16] == b"\x00" * 16:
            continue
        if entry[0:16] == ESP_TYPE:
            found.append((slot, struct.unpack_from("<Q", entry, 32)[0]))
    return found


def bootlog_crc32(data):
    """CRC-32, IEEE 802.3: zlib's, which is the kernel's own boot log CRC."""
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_bootlog_header(slot):
    """(state, fields, reason) for slot 0 of a boot log.

    The state is MISSING when the slot was never written, CORRUPT when it does
    not parse as a version 1 header (magic, widths, CRC or version), and OK
    otherwise; the caller turns OK into MISMATCH when the geometry the header
    declares is not the geometry of the file it read.
    """
    if slot == bytes(BOOTLOG_SLOT):
        return ("MISSING", None, "the header slot is empty")
    match = HEADER_LINE.match(slot)
    if match is None:
        return ("CORRUPT", None,
                "the header slot is not an 'HFOC-BOOTLOG v=1' line")
    if bootlog_crc32(slot[:match.start("crc")]) != int(match.group("crc"), 16):
        return ("CORRUPT", None, "the header CRC does not match the header")
    if int(match.group("version")) != 1:
        return ("CORRUPT", None,
                "the header declares version %d, not 1"
                % int(match.group("version")))
    fields = {name: int(match.group(name))
              for name in ("slot", "slots", "records", "gen", "next",
                           "cursor", "dropped", "written")}
    return ("OK", fields, None)


def parse_bootlog_record(slot):
    """(entry, state) for one record slot: state is OK, EMPTY or TORN.

    A record's text is only handed back when its CRC matches, its length is
    within the format, every text byte is printable ASCII and a single newline
    closes the text.  Anything else is a torn write and is never printed.
    """
    if slot[0] == 0x00:
        return (None, "EMPTY")
    match = RECORD_LINE.match(slot)
    if match is None:
        return (None, "TORN")
    length = int(match.group("length"))
    start = match.end()
    if length > BOOTLOG_TEXT_MAX or start + length >= BOOTLOG_SLOT:
        return (None, "TORN")
    text = slot[start:start + length]
    if slot[start + length] != 0x0A:
        return (None, "TORN")
    if any(byte < 0x20 or byte > 0x7E for byte in text):
        return (None, "TORN")
    if bootlog_crc32(slot[:match.start("crc")] + text) != \
            int(match.group("crc"), 16):
        return (None, "TORN")
    return ({"seq": int(match.group("seq")), "gen": int(match.group("gen")),
             "ticks": int(match.group("ticks")), "text": text}, "OK")


def newest_run(valid):
    """The slots of the newest contiguous sequence run, oldest first.

    The newest record is the one with the highest sequence number.  Only a
    rewritten image can carry one sequence twice, and there the copy whose slot
    index continues the run is the one that was written last; from it the walk
    goes backwards slot by slot - slot 1 continues at slot 511 - while the
    previous slot holds a valid record with the sequence exactly one lower.
    Records outside that run are the leftovers of an earlier wrap: they are
    counted, never printed.
    """
    if not valid:
        return []
    newest = max(entry["seq"] for entry in valid.values())
    candidates = sorted(index for index, entry in valid.items()
                        if entry["seq"] == newest)
    chosen = None
    for index in candidates:
        previous = BOOTLOG_RECORD_CAPACITY if index == 1 else index - 1
        entry = valid.get(previous)
        if entry is not None and entry["seq"] == newest - 1:
            chosen = index
            break
    if chosen is None:
        chosen = candidates[0]
    run = [chosen]
    while len(run) < len(valid):
        current = run[-1]
        previous = BOOTLOG_RECORD_CAPACITY if current == 1 else current - 1
        entry = valid.get(previous)
        if entry is None or entry["seq"] != valid[current]["seq"] - 1:
            break
        run.append(previous)
    run.reverse()
    return run


def print_boot_log(path, data):
    """Print the summary and the records of one boot log; refuse a non-log."""
    total = len(data)
    if total < BOOTLOG_SLOT:
        fail(f"{path} is {total} bytes, shorter than one "
             f"{BOOTLOG_SLOT}-byte slot")
    if total % BOOTLOG_SLOT:
        warn(f"bytes={total} is not a whole number of {BOOTLOG_SLOT}-byte "
             f"slots; reading {total // BOOTLOG_SLOT} slots")
    slots = total // BOOTLOG_SLOT
    records = max(0, slots - 1)

    state, header, reason = parse_bootlog_header(data[:BOOTLOG_SLOT])
    if state == "CORRUPT":
        fail(f"{path} is not an HFOC boot log: {reason}")
    if state == "OK" and (header["slot"] != BOOTLOG_SLOT or
                          header["slots"] != slots or
                          header["records"] != records):
        warn(f"header claims slot={header['slot']} slots={header['slots']} "
             f"records={header['records']} but the file holds {slots} slots "
             f"and {records} records")
        state = "MISMATCH"

    valid = {}
    torn = 0
    first_torn = None
    for index in range(1, slots):
        entry, slot_state = parse_bootlog_record(
            data[index * BOOTLOG_SLOT:(index + 1) * BOOTLOG_SLOT])
        if slot_state == "OK":
            valid[index] = entry
        elif slot_state == "TORN":
            torn += 1
            if first_torn is None:
                first_torn = index
    if torn:
        warn(f"{torn} torn or unparseable non-empty slot(s), the first at "
             f"slot {first_torn}")

    run = newest_run(valid)
    outside = len(valid) - len(run)
    if outside:
        warn(f"{outside} valid record(s) are outside the shown run, the "
             f"leftovers of an earlier wrap")

    if header is not None:
        if header["cursor"] >= BOOTLOG_RECORD_CAPACITY:
            warn(f"header cursor={header['cursor']} is outside 0.."
                 f"{BOOTLOG_RECORD_CAPACITY - 1}")
        present = sum(1 for index in range(1, slots)
                      if data[index * BOOTLOG_SLOT] != 0x00)
        if header["written"] > present:
            warn(f"header written={header['written']} is larger than the "
                 f"{present} record slot(s) that carry data")
        if valid:
            newest = max(entry["seq"] for entry in valid.values())
            if header["next"] > newest + 1:
                warn(f"header next={header['next']} is beyond the newest "
                     f"record present, sequence {newest}")
        elif header["next"] > 1:
            warn(f"header next={header['next']} but no record is present")

    generations = {valid[index]["gen"] for index in run}
    if state == "OK":
        generation = header["gen"]
        dropped = header["dropped"]
        written = header["written"]
    else:
        generation = valid[run[-1]]["gen"] if run else "?"
        dropped = "?"
        written = "?"
    wrapped = "YES" if len(generations) > 1 or \
        (header is not None and header["gen"] > 0) else "NO"
    oldest = valid[run[0]]["seq"] if run else "?"
    newest = valid[run[-1]]["seq"] if run else "?"
    print(f"boot-log: path={path} bytes={total} slot={BOOTLOG_SLOT} "
          f"slots={slots} records={records} header={state} gen={generation} "
          f"wrapped={wrapped} oldest={oldest} newest={newest} "
          f"shown={len(run)} dropped={dropped} written={written}")
    for index in run:
        entry = valid[index]
        print(f"#{entry['seq']:020d} gen={entry['gen']} ticks={entry['ticks']} "
              f"{entry['text'].decode('ascii')}")
    if not run:
        print("boot-log: no records")
    return 0


def boot_log_from_file(path):
    try:
        with open(path, "rb") as handle:
            data = handle.read(BOOTLOG_SCAN_LIMIT + 1)
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    if len(data) > BOOTLOG_SCAN_LIMIT:
        warn(f"{path} is larger than {BOOTLOG_SCAN_LIMIT} bytes; reading the "
             f"first {BOOTLOG_SCAN_LIMIT}")
        data = data[:BOOTLOG_SCAN_LIMIT]
    return print_boot_log(path, data)


def locate_boot_log(disk, partition_index):
    """(data, missing_bytes, partitions_checked) for the log inside an image."""
    if partition_index is None:
        candidates = esp_partitions(disk)
    else:
        try:
            candidates = [(partition_index,
                           partition_extents(disk, partition_index))]
        except SystemExit:
            raise
        except Exception as error:
            fail(f"cannot read partition {partition_index} of the image: "
                 f"{error}")
    checked = 0
    for slot, first_lba in candidates:
        checked += 1
        try:
            found = Volume(disk, first_lba).read_optional(
                BOOTLOG_PATH, BOOTLOG_SCAN_LIMIT)
        except SystemExit:
            # A partition that is not a FAT volume this tool can read is an
            # answer to a search, not a failure; to an explicit
            # `--partition-index` it is reported below.
            if partition_index is not None:
                fail(f"no HFOC boot log at {BOOTLOG_PATH} in partition "
                     f"{slot}: it is not a FAT volume this tool can read")
            warn(f"partition {slot} is an EFI System Partition this tool "
                 f"cannot read as FAT32")
            found = None
        except Exception as error:
            if partition_index is not None:
                fail(f"cannot read partition {slot} of the image: {error}")
            found = None
        if found is not None:
            return (found[0], found[1], checked)
    if partition_index is not None:
        fail(f"no HFOC boot log at {BOOTLOG_PATH} in partition "
             f"{partition_index} (1 EFI System Partition checked)")
    fail(f"no HFOC boot log at {BOOTLOG_PATH} in {checked} EFI System "
         f"Partition(s) checked")


def map_image(handle, image):
    """Map an image, whether it is a file or a whole block device.

    A block device has no size in `fstat` - the kernel reports zero - so
    `mmap(..., 0)` asks for an empty mapping and the kernel answers EINVAL.
    That is what the runbook command hit: `hfoc-esp-read.py /dev/sda --bootlog`
    failed with `cannot map /dev/sda: [Errno 22] Invalid argument` on the
    machine whose log the operator needs, and the log could then only be read
    after mounting the ESP.  The size of a block device comes from
    BLKGETSIZE64, and an explicit length maps the device exactly as the file is
    mapped.
    """
    try:
        return mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
    except (OSError, ValueError) as error:
        if not stat.S_ISBLK(os.fstat(handle.fileno()).st_mode):
            fail(f"cannot map {image}: {error}")
    try:
        length = struct.unpack(
            "q", fcntl.ioctl(handle.fileno(), BLKGETSIZE64, b"\0" * 8))[0]
        return mmap.mmap(handle.fileno(), length, access=mmap.ACCESS_READ)
    except (OSError, ValueError) as error:
        fail(f"cannot map {image}: {error}")


def boot_log_from_image(image, partition_index):
    try:
        handle = open(image, "rb")
    except OSError as error:
        fail(f"cannot read {image}: {error}")
    with handle:
        disk = map_image(handle, image)
        with disk:
            data, missing, _checked = locate_boot_log(disk, partition_index)
    if missing:
        warn(f"the image ends inside the cluster chain of {BOOTLOG_PATH}: "
             f"{missing} of the {len(data) + missing} bytes are missing; "
             f"parsing the {len(data)} that are there")
    return print_boot_log(BOOTLOG_PATH, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", default=None,
                        help="the raw disk image or device to read")
    parser.add_argument("--disk", default=None,
                        help="the raw disk image to read (same as IMAGE)")
    parser.add_argument("--path", default=None,
                        help="extract this file, relative to the volume root")
    parser.add_argument("--partition-index", type=int, default=None)
    parser.add_argument("--bootlog", action="store_true",
                        help="print the appliance boot log "
                             "(\\EFI\\HFOC\\BOOTLOG.BIN) out of the ESP")
    parser.add_argument("--bootlog-file", default=None, metavar="PATH",
                        help="print the appliance boot log from a plain file")
    arguments = parser.parse_args()

    image = arguments.image
    if arguments.disk is not None:
        if image is None:
            image = arguments.disk
        elif image != arguments.disk:
            fail(f"--disk names {arguments.disk!r} and the image argument "
                 f"names {image!r}")

    if arguments.bootlog_file is not None:
        if arguments.bootlog:
            fail("--bootlog-file reads a file of its own; --bootlog reads the "
                 "log inside an image")
        if image is not None:
            fail("--bootlog-file reads a plain file and takes no image")
        if arguments.partition_index is not None:
            fail("--partition-index selects a partition of an image; "
                 "--bootlog-file takes no image")
        if arguments.path is not None:
            fail("--bootlog-file prints the boot log; --path extracts a file")
        return boot_log_from_file(arguments.bootlog_file)

    if arguments.bootlog:
        if arguments.path is not None:
            fail("--bootlog prints the boot log; --path extracts a file")
        if image is None:
            fail("no image given: pass it as the image argument or as --disk")
        return boot_log_from_image(image, arguments.partition_index)

    if arguments.path is None:
        fail("no --path given: pass --path, --bootlog or --bootlog-file")
    if image is None:
        fail("no image given: pass it as the image argument or as --disk")

    # The acceptance images are 114 GiB sparse files, so the image is mapped
    # rather than read: only the pages this tool touches are faulted in, and
    # every slice below is the same byte comparison it would be on a copy.  A
    # block device is mapped the same way, from its BLKGETSIZE64 length.
    try:
        with open(image, "rb") as handle:
            with map_image(handle, image) as disk:
                first_lba = partition_extents(disk, arguments.partition_index)
                data = Volume(disk, first_lba).read(arguments.path)
    except SystemExit:
        raise
    except Exception as error:
        # An image this tool cannot parse used to end in an uncaught traceback
        # and status 1; the status the install proofs see is kept, the
        # traceback is not.
        print(f"hfoc-esp-read: cannot read {image}: {error}", file=sys.stderr)
        raise SystemExit(1) from None
    sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
