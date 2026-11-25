#!/usr/bin/env python3
"""Read the install journal off the installation medium.

ITEM-521 requirement 4: the owner brings the stick back and one command prints
what the install run recorded, without mounting partitions or hunting for the
file.  The journal is written by the kernel during the run (the wiring lands in
the ITEM-521 increments after the ITEM-520 placement fork is answered); this
tool is the reading half and is independent of which placement is chosen:

  (a) FAT12 file  \\HFOC\\INSTALL.LOG  created by the installer (item-520-fat12-c)
  (b) raw extent  block_count - 34 - 2048, self-describing header
  (c) preallocated \\HFOC\\INSTALL.LOG the installer only writes inside: the
      directory size stays at capacity and the header's write_slot advances

The slot contract is `kernel/include/install_journal_core.h` (version 1):

  header: 0..8 magic "HFOC-ILOG", 9..12 version, 13..16 slot_size,
          17..20 slot_count, 21..24 write_slot, 25..32 sequence,
          33..35 reserved, 36..39 CRC32 over bytes 0..35;
  record: 0..3 length, 4..7 CRC32 over the text, 8.. text, NUL padding.

A record whose length or CRC does not check out is torn or foreign and is not
printed as text.  Nothing here comes from the kernel or from the writer: the
partition is found by its MBR type, the geometry from the BPB, the bytes from
the raw sectors, and the header is checked against its own CRC.

Usage:
  hfoc-install-log-read.py --disk /dev/sdb
  hfoc-install-log-read.py IMAGE [--offset BYTES]
  hfoc-install-log-read.py --file /mnt/EFI/HFOC/INSTALL.LOG
  hfoc-install-log-read.py --disk /dev/sdb --json

`--disk` is the other spelling of the positional image path, so the runbook's
`hfoc-install-log-read.py --disk /dev/sdb` works on the acceptance host as well.
`--file` reads an already-mounted journal file and needs no FAT parsing.
`--offset BYTES` names the FAT12 volume's first byte directly, so a partition
dump or an image with no MBR reads the same way; a volume that does not parse
at that offset is malformed (exit 1), not "no journal".

Exit status: 0 when a journal was found and read, 1 when one was found but is
malformed (the reason is printed on stderr), 2 when the medium carries none.
Read-only: the image or device is opened for reading only, and a block device
is sized with BLKGETSIZE64 (`fstat` answers zero for a device).
"""

import argparse
import fcntl
import json
import os
import stat
import struct
import sys
import zlib

BLKGETSIZE64 = 0x80081272
SLOT = 512
TEXT_MAX = 440
MAGIC = b"HFOC-ILOG"
MAGIC_BYTES = 9
VERSION = 1
EOC = 0xFF8
ATTR_DIRECTORY = 0x10

EXIT_OK = 0
EXIT_MALFORMED = 1
EXIT_ABSENT = 2


class Malformed(Exception):
    """The medium carries a journal, but it does not check out."""


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def le64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def crc32(buf, off, length):
    return zlib.crc32(buf[off:off + length]) & 0xFFFFFFFF


def decode_record(slot, index, warnings):
    """One record slot of the version-1 frame.  A slot whose length or CRC does
    not check out is torn or foreign and is never printed as text; an all-zero
    slot inside the committed prefix was committed but never written."""
    if not any(slot):
        warnings.append(f"slot {index} is empty (committed but never written)")
        return None
    length = le32(slot, 0)
    if length == 0 or length > TEXT_MAX or length + 8 > SLOT:
        warnings.append(f"slot {index} has no valid record frame (length {length})")
        return None
    text = slot[8:8 + length]
    if (zlib.crc32(text) & 0xFFFFFFFF) != le32(slot, 4):
        warnings.append(f"slot {index} is torn (record CRC mismatch)")
        return None
    if any(byte == 0 or byte >= 0x7F or byte < 0x20 for byte in text):
        warnings.append(f"slot {index} carries non-printable text")
        return None
    return text.decode("ascii")


class Image:
    """A raw image or block device, read-only."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        st = os.fstat(self.f.fileno())
        if stat.S_ISBLK(st.st_mode):
            buf = bytearray(8)
            fcntl.ioctl(self.f.fileno(), BLKGETSIZE64, buf, True)
            self.size = struct.unpack("<Q", buf)[0]
        else:
            self.size = st.st_size

    def close(self):
        self.f.close()

    def read(self, offset, length):
        self.f.seek(offset)
        data = self.f.read(length)
        if len(data) != length:
            raise Malformed(f"short read at byte {offset}: {len(data)} of {length}")
        return data


class Fat12Volume:
    """The FAT12 volume the installer medium carries: MBR type 0xEF, 512-byte
    sectors, a fixed root directory and a 12-bit allocation table."""

    def __init__(self, image, base):
        self.image = image
        self.base = base
        bpb = self.read_lba(0)
        if bpb[510:512] != b"\x55\xaa":
            raise Malformed("volume has no 0x55AA signature")
        self.bps = le16(bpb, 11)
        self.spc = bpb[13]
        self.reserved = le16(bpb, 14)
        self.nfat = bpb[16]
        self.root_entries = le16(bpb, 17)
        total16 = le16(bpb, 19)
        self.spf = le16(bpb, 22) or le32(bpb, 36)
        self.total = total16 or le32(bpb, 32)
        if self.bps != SLOT:
            raise Malformed(f"unsupported sector size {self.bps}")
        if self.spc == 0 or self.nfat == 0 or self.spf == 0:
            raise Malformed("degenerate BPB")
        self.fat_lba = self.reserved
        self.root_lba = self.reserved + self.nfat * self.spf
        root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_lba = self.root_lba + root_sectors
        self.clusters = (self.total - self.data_lba) // self.spc
        if not 0 < self.clusters < 4085:
            raise Malformed(f"not a FAT12 volume ({self.clusters} clusters)")

    def read_lba(self, lba):
        return self.image.read(self.base + lba * SLOT, SLOT)

    def read_bytes(self, lba, offset, length):
        out = b""
        while length:
            sec = self.read_lba(lba + offset // SLOT)
            take = min(length, SLOT - offset % SLOT)
            out += sec[offset % SLOT:offset % SLOT + take]
            offset += take
            length -= take
        return out

    def fat_entry(self, cluster):
        off = cluster + cluster // 2
        word = struct.unpack("<H", self.read_bytes(self.fat_lba, off, 2))[0]
        return (word >> 4) if cluster % 2 else (word & 0x0FFF)

    def cluster_lba(self, cluster):
        return self.data_lba + (cluster - 2) * self.spc

    def chain(self, first):
        out = []
        cluster = first
        while 2 <= cluster < self.clusters + 2:
            out.append(cluster)
            nxt = self.fat_entry(cluster)
            if nxt == 0 or nxt >= EOC:
                break
            if nxt in out:
                raise Malformed("FAT chain loops")
            cluster = nxt
        if not out:
            raise Malformed(f"FAT chain starts outside the data area ({first})")
        return out

    @staticmethod
    def name_of(entry):
        return (entry[0:8].decode("latin1").rstrip(),
                entry[8:11].decode("latin1").rstrip())

    def find_in(self, dir_cluster, name8, ext3):
        for cluster in self.chain(dir_cluster):
            base = self.cluster_lba(cluster) * SLOT
            for i in range(self.spc * SLOT // 32):
                entry = self.read_bytes(base // SLOT, (base % SLOT) + i * 32, 32)
                if entry[0] == 0x00:
                    return None
                if entry[0] == 0xE5:
                    continue
                if self.name_of(entry) == (name8, ext3):
                    return entry
        return None

    def find_root(self, name8, ext3):
        root = self.root_lba * SLOT
        for i in range(self.root_entries):
            entry = self.read_bytes(root // SLOT, (root % SLOT) + i * 32, 32)
            if entry[0] == 0x00:
                break
            if entry[0] == 0xE5:
                continue
            if self.name_of(entry) == (name8, ext3):
                return entry
        return None


def read_fat12_journal(volume):
    """\\HFOC\\INSTALL.LOG, or None when the volume simply does not carry it."""
    hfoc = volume.find_root("HFOC", "")
    if hfoc is None or not (hfoc[11] & ATTR_DIRECTORY):
        return None
    entry = volume.find_in(le16(hfoc, 26), "INSTALL", "LOG")
    if entry is None or (entry[11] & ATTR_DIRECTORY):
        return None
    first = le16(entry, 26)
    size = le32(entry, 28)
    if size == 0 or size % SLOT:
        raise Malformed(f"journal size {size} is not a whole number of slots")
    chain = volume.chain(first)
    capacity_bytes = len(chain) * volume.spc * SLOT
    if size > capacity_bytes:
        raise Malformed("journal size is larger than its FAT chain")
    data = b"".join(volume.read_bytes(volume.cluster_lba(c), 0,
                                      volume.spc * SLOT) for c in chain)
    return finish_journal(
        placement="fat12-file" if size < capacity_bytes else "fat12-preallocated",
        header=data[:SLOT],
        slots=data[:size],
        slots_per_cluster=volume.spc,
        chain_clusters=len(chain),
        contiguous=chain == list(range(first, first + len(chain))),
    )


def read_extent_journal(image):
    """The raw-extent placement (option b): a self-describing header at
    `blocks - 34 - 2048`, so the reader finds it from the device alone."""
    blocks = image.size // SLOT
    first = blocks - 34 - 2048
    if first < 1:
        return None
    header = image.read(first * SLOT, SLOT)
    if header[0:MAGIC_BYTES] != MAGIC:
        return None
    if le32(header, 9) != VERSION or le32(header, 13) != SLOT:
        raise Malformed("extent header version or slot size mismatch")
    # Two extent header layouts exist and the reader accepts both: the prototype
    # module (item-520-fat12-c/install_extent_journal.c) keeps first_lba at 25
    # and its CRC at 41, while the placement-independent core writes slot 0 in
    # the core layout (CRC at 36 over bytes 0..35, no first_lba of its own).
    # The final PR reconciles the two; until then the reader names what it read.
    if le64(header, 25) == first and crc32(header, 0, 41) == le32(header, 41):
        header_crc = (41, 41)
    elif crc32(header, 0, 36) == le32(header, 36):
        header_crc = (36, 36)
    else:
        raise Malformed("extent header CRC mismatch")
    slot_count = le32(header, 17)
    write_slot = le32(header, 21)
    if not 2 <= slot_count or write_slot > slot_count or write_slot < 1:
        raise Malformed(f"extent header slot_count={slot_count} write_slot={write_slot}")
    slots = b"".join(image.read((first + i) * SLOT, SLOT) for i in range(write_slot))
    return finish_journal(
        placement="extent",
        header=header,
        slots=slots,
        header_crc=header_crc,
        first_lba=first,
        capacity_slots=slot_count,
        chain_clusters=0,
        contiguous=True,
    )


def finish_journal(placement, header, slots, header_crc=(36, 36),
                   slots_per_cluster=0, chain_clusters=0, contiguous=True,
                   first_lba=None, capacity_slots=None):
    """Validate slot 0 and cut the committed prefix.  The commit point differs
    by placement: the FAT12 writer moves the directory size (size = visible
    slots), the preallocated and extent writers advance the header's
    write_slot while the file/region keeps its full capacity."""
    if header[0:MAGIC_BYTES] != MAGIC:
        raise Malformed(f"bad journal magic {header[0:MAGIC_BYTES]!r}")
    if le32(header, 9) != VERSION:
        raise Malformed(f"unsupported journal version {le32(header, 9)}")
    if le32(header, 13) != SLOT:
        raise Malformed(f"unsupported slot size {le32(header, 13)}")
    covered, crc_offset = header_crc
    if crc32(header, 0, covered) != le32(header, crc_offset):
        raise Malformed("journal header CRC mismatch")
    header_count = le32(header, 17)
    write_slot = le32(header, 21)
    by_size = len(slots) // SLOT
    if by_size < 1:
        raise Malformed("journal is shorter than one slot")
    if capacity_slots is None:
        capacity_slots = header_count if header_count else by_size
    # (a) the directory size is the commit point; (c) the file is preallocated
    # and the header's write_slot is.  A header that never advanced cannot hide
    # committed records, and one that advanced inside a full-size file must.
    if 1 <= write_slot < by_size:
        visible = write_slot
    else:
        visible = by_size
    warnings = []
    records = []
    for index in range(1, visible):
        slot = slots[index * SLOT:(index + 1) * SLOT]
        if len(slot) < SLOT:
            warnings.append(f"slot {index} is truncated")
            continue
        text = decode_record(slot, index, warnings)
        if text is not None:
            records.append({"slot": index, "text": text})
    return {
        "placement": placement,
        "first_lba": first_lba,
        "slots": visible,
        "capacity_slots": capacity_slots,
        "slots_per_cluster": slots_per_cluster,
        "chain_clusters": chain_clusters,
        "contiguous": contiguous,
        "warnings": warnings,
        "records": records,
    }


def find_efi_partition(image, offset):
    if offset is not None:
        return offset
    mbr = image.read(0, SLOT)
    if mbr[510:512] != b"\x55\xaa":
        raise Malformed("image has no MBR")
    # The install medium's volume is the 0xEF partition; the separately prepared
    # diagnostic stick (ITEM-582) carries its reserved journal in a FAT12
    # partition (type 0x01), so both are tried, the EFI one first.
    for wanted in ((0xEF,), (0x01, 0x04, 0x06, 0x0B, 0x0C)):
        for i in range(4):
            entry = mbr[446 + 16 * i:446 + 16 * (i + 1)]
            if entry[4] in wanted and le32(entry, 12) != 0:
                return le32(entry, 8) * SLOT
    raise Malformed("no FAT or EFI partition in the MBR")


def read_journal(image, offset=None):
    """FAT12 file first (the owner's requirement names the file), then the raw
    extent.  A malformed FAT12 journal is reported, not silently skipped."""
    try:
        base = find_efi_partition(image, offset)
    except Malformed:
        base = None
    if base is not None:
        volume = Fat12Volume(image, base)
        journal = read_fat12_journal(volume)
        if journal is not None:
            return journal
    return read_extent_journal(image)


def read_mounted_file(path):
    """An already-mounted journal file: no FAT parsing, the same slot frame."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < SLOT:
        raise Malformed("file is shorter than one slot")
    return finish_journal(placement="file", header=data[:SLOT], slots=data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", help="raw disk image")
    ap.add_argument("--disk", dest="disk", help="raw disk image (other spelling)")
    ap.add_argument("--file", dest="file", help="an already-mounted journal file")
    ap.add_argument("--offset", type=lambda x: int(x, 0),
                    help="byte offset of the FAT12 volume")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()
    named = [name for name in (args.image, args.disk, args.file) if name]
    if len(named) != 1:
        ap.error("name exactly one of IMAGE, --disk or --file")

    try:
        if args.file:
            journal = read_mounted_file(args.file)
        else:
            image = Image(args.disk or args.image)
            try:
                journal = read_journal(image, args.offset)
            finally:
                image.close()
    except Malformed as error:
        print(f"install-log: malformed: {error}", file=sys.stderr)
        return EXIT_MALFORMED
    except OSError as error:
        print(f"install-log: {error}", file=sys.stderr)
        return EXIT_MALFORMED

    if journal is None:
        print("install-log: no journal on this medium", file=sys.stderr)
        return EXIT_ABSENT
    for warning in journal["warnings"]:
        print(f"install-log: warning: {warning}", file=sys.stderr)
    if args.json:
        print(json.dumps(journal, indent=2))
        return EXIT_OK
    where = journal["placement"]
    if journal["first_lba"] is not None:
        where += f" at lba {journal['first_lba']}"
    print(f"journal: {where}, {journal['slots']} slots of "
          f"{journal['capacity_slots']}, {journal['chain_clusters']} clusters, "
          f"contiguous={journal['contiguous']}")
    for record in journal["records"]:
        print(f"{record['slot']:4d}  {record['text']}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
