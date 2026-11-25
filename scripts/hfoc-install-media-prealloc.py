#!/usr/bin/env python3
"""Reserve the install journal inside the installer medium's FAT12 ESP.

ITEM-521 increment 12, the owner's answer (c) to the ITEM-520 fork: the build
of an installer medium reserves the file \\HFOC\\INSTALL.LOG inside the ISO's
FAT12 EFI-system partition, and the kernel only reads the FAT12 chain this
script wrote and writes journal slots inside those already-reserved clusters.
The kernel implements no FAT12 allocation, so everything that allocates - the
HFOC directory, the file's cluster chain and the slot-0 journal header - is
done here, once, at build time.

The medium's ESP is FAT12: MBR partition type 0xEF, 512-byte sectors, and the
volume is classified exactly the way scripts/hfoc-install-log-read.py does
(0 < data clusters < 4085) so the reader and the writer agree on the geometry.
The file is reserved as one contiguous run of free clusters when the volume
carries one (falling back to the first free clusters when the volume is
fragmented), the chain is marked in every FAT copy, every reserved cluster is
zeroed, the directory entry carries the whole capacity as its size, and slot 0
of the first cluster carries the version-1 journal header whose CRC the reader
checks.

Slot 0 layout (kernel/include/install_journal_core.h, version 1):

  0..8   magic "HFOC-ILOG"
  9..12  little-endian u32 version 1
  13..16 little-endian u32 slot size 512
  17..20 little-endian u32 capacity_slots = clusters * cluster_bytes // 512
  21..24 little-endian u32 write_slot = 1
  25..32 sequence (zero until the kernel commits records)
  33..35 reserved
  36..39 little-endian CRC32 (zlib.crc32) over bytes 0..35

Idempotence: a \\HFOC\\INSTALL.LOG that already has exactly the requested
capacity, agrees in every FAT copy and carries a valid header is reported as
`preallocated: already reserved` and the image is not touched.  A header whose
write_slot the kernel advanced past 1 is still valid - the reservation is
intact and carries records - and is never reset; only a header that does not
frame the requested capacity with a matching CRC is refused.  A file with any
other shape is a refusal (exit 2): this script never silently resizes or
overwrites a reservation.

Nothing outside the FAT12 volume changes: every write is bounded by the
partition and read back, and a refusal rolls the image back byte for byte.

Usage:
  python3 scripts/hfoc-install-media-prealloc.py IMAGE [--clusters N]
                                                   [--offset BYTES] [--quiet]

`--clusters` is the reservation size in clusters (default 256, at most 512 so
the kernel's FAT12_JOURNAL_MAX_CLUSTERS chain bound is never exceeded).
`--offset BYTES` names the FAT12 volume's first byte directly (a multiple of
the 512-byte sector) and skips MBR detection.  `--quiet` prints nothing on
success; refusals always go to stderr.

Exit status: 0 on success (created or already reserved), 1 on an I/O or open
error, 2 on a refusal.
"""

import argparse
import os
import struct
import sys
import zlib

SLOT = 512
MAGIC = b"HFOC-ILOG"
MAGIC_BYTES = 9
VERSION = 1
EOC = 0xFFF
EOC_MIN = 0xFF8
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
DIR_ENTRY_BYTES = 32
FAT12_CLUSTER_LIMIT = 4085
DEFAULT_CLUSTERS = 256
# kernel/include/install_fat12_journal.h: FAT12_JOURNAL_MAX_CLUSTERS.  A longer
# chain is refused by the kernel, so the build never writes one.
MAX_CLUSTERS = 512

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_REFUSED = 2


class Refused(Exception):
    """The image cannot carry the reservation the caller asked for."""


class Failed(Exception):
    """The image could not be opened, read or written."""


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put_le16(buf, off, value):
    struct.pack_into("<H", buf, off, value & 0xFFFF)


def put_le32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def crc32(buf, off, length):
    return zlib.crc32(buf[off:off + length]) & 0xFFFFFFFF


class Image:
    """The raw image, opened for reading and writing."""

    def __init__(self, path):
        self.path = path
        try:
            self.f = open(path, "r+b")
        except OSError as error:
            raise Failed(f"cannot open {path}: {error.strerror or error}") from error
        self.size = os.fstat(self.f.fileno()).st_size

    def close(self):
        self.f.close()

    def read(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.size:
            raise Failed(f"read of {length} bytes at byte {offset} escapes the image")
        self.f.seek(offset)
        data = self.f.read(length)
        if len(data) != length:
            raise Failed(f"short read at byte {offset}: {len(data)} of {length}")
        return data

    def write(self, offset, data):
        self.f.seek(offset)
        written = self.f.write(data)
        if written != len(data):
            raise Failed(f"short write at byte {offset}: {written} of {len(data)}")

    def flush(self):
        self.f.flush()


class PartitionWriter:
    """Write only inside the FAT12 partition, and be able to take it back.

    Every byte range is checked against the partition before it is written and
    read back afterwards, so the post-write verification names exactly the
    ranges this run changed; a refusal replays the saved bytes in reverse.
    """

    BLOCK = 32

    def __init__(self, image, start, end):
        self.image = image
        self.start = start
        self.end = end
        self.undo = []
        self.changed = []
        self.final = {}

    def write(self, offset, data):
        if offset < self.start or offset + len(data) > self.end:
            raise Refused(
                f"write of bytes {offset}..{offset + len(data)} escapes the "
                f"FAT12 partition at bytes {self.start}..{self.end}")
        before = self.image.read(offset, len(data))
        self.image.write(offset, data)
        self.undo.append((offset, before))
        self.changed.append((offset, len(data)))
        for index in range(0, len(data), self.BLOCK):
            self.final[offset + index] = bytes(data[index:index + self.BLOCK])

    def verify(self):
        """Every changed byte is inside the partition, and the final content of
        every block this run wrote is on the image.  A later write deliberately
        supersedes an earlier one to the same bytes (a zeroed cluster then its
        slot-0 header), so only the final content is compared."""
        for offset, length in self.changed:
            if offset < self.start or offset + length > self.end:
                raise Refused(
                    f"changed bytes {offset}..{offset + length} lie outside "
                    f"the FAT12 partition at bytes {self.start}..{self.end}")
        points = sorted(self.final)
        index = 0
        while index < len(points):
            start = points[index]
            run = bytearray(self.final[start])
            index += 1
            while index < len(points) and points[index] == start + len(run):
                run += self.final[points[index]]
                index += 1
            if self.image.read(start, len(run)) != bytes(run):
                raise Refused(f"bytes {start}..{start + len(run)} did not read back")

    def rollback(self):
        for offset, before in reversed(self.undo):
            try:
                self.image.write(offset, before)
            except (Failed, OSError):
                pass
        self.undo.clear()
        self.changed.clear()
        self.final.clear()


class Fat12Volume:
    """The FAT12 volume the installer medium's ESP carries."""

    def __init__(self, image, base, writer):
        self.image = image
        self.base = base
        self.writer = writer
        bpb = image.read(base, SLOT)
        if bpb[510:512] != b"\x55\xaa":
            raise Refused("the FAT12 volume has no 0x55AA BPB signature")
        self.bytes_per_sector = le16(bpb, 11)
        self.sectors_per_cluster = bpb[13]
        self.reserved_sectors = le16(bpb, 14)
        self.fat_count = bpb[16]
        self.root_entries = le16(bpb, 17)
        total16 = le16(bpb, 19)
        self.sectors_per_fat = le16(bpb, 22) or le32(bpb, 36)
        self.total_sectors = total16 or le32(bpb, 32)

        if self.bytes_per_sector != SLOT:
            raise Refused(f"unsupported sector size {self.bytes_per_sector}")
        if self.sectors_per_cluster < 1 or (
                self.sectors_per_cluster & (self.sectors_per_cluster - 1)):
            raise Refused(
                f"sectors per cluster {self.sectors_per_cluster} is not a power of two")
        if self.reserved_sectors < 1:
            raise Refused("the reserved sector count is zero")
        if self.fat_count < 1:
            raise Refused("the volume has no FAT")
        if self.sectors_per_fat < 1:
            raise Refused("the FAT is empty")
        if self.root_entries < 1:
            raise Refused("the root directory is empty")
        if self.total_sectors < 1:
            raise Refused("the volume declares no sectors")

        root_sectors = (self.root_entries * DIR_ENTRY_BYTES +
                        self.bytes_per_sector - 1) // self.bytes_per_sector
        self.fat_lba = self.reserved_sectors
        self.root_lba = self.reserved_sectors + self.fat_count * self.sectors_per_fat
        self.data_lba = self.root_lba + root_sectors
        self.clusters = (self.total_sectors - self.data_lba) // self.sectors_per_cluster
        if not 0 < self.clusters < FAT12_CLUSTER_LIMIT:
            raise Refused(f"not a FAT12 volume ({self.clusters} data clusters)")
        fat_bytes = self.sectors_per_fat * self.bytes_per_sector
        if (fat_bytes * 2) // 3 < self.clusters + 2:
            raise Refused("the FAT is too short for the volume's cluster count")

        self.cluster_bytes = self.sectors_per_cluster * self.bytes_per_sector
        self.slots_per_cluster = self.cluster_bytes // SLOT
        self.fat_offset = self.fat_lba * SLOT
        self.fat_bytes = fat_bytes
        self.root_offset = self.root_lba * SLOT
        self.data_offset = self.data_lba * SLOT
        self.volume_bytes = self.total_sectors * SLOT
        if base + self.volume_bytes > writer.end:
            raise Refused(
                f"the volume needs {self.volume_bytes} bytes but only "
                f"{writer.end - base} fit the partition")
        self.fats = [
            bytearray(image.read(base + self.fat_offset + copy * fat_bytes, fat_bytes))
            for copy in range(self.fat_count)
        ]
        self.dirty = set()

    def absolute(self, offset):
        return self.base + offset

    def read(self, offset, length):
        return self.image.read(self.base + offset, length)

    def fat_entry(self, copy, cluster):
        fat = self.fats[copy]
        offset = cluster + cluster // 2
        if offset + 1 >= len(fat):
            raise Refused(f"FAT entry {cluster} lies outside the table")
        word = fat[offset] | (fat[offset + 1] << 8)
        return (word >> 4) if cluster & 1 else (word & 0x0FFF)

    def fat_set(self, copy, cluster, value):
        fat = self.fats[copy]
        offset = cluster + cluster // 2
        if offset + 1 >= len(fat):
            raise Refused(f"FAT entry {cluster} lies outside the table")
        word = fat[offset] | (fat[offset + 1] << 8)
        if cluster & 1:
            word = (word & 0x000F) | ((value & 0x0FFF) << 4)
        else:
            word = (word & 0xF000) | (value & 0x0FFF)
        fat[offset] = word & 0xFF
        fat[offset + 1] = (word >> 8) & 0xFF
        self.dirty.add((copy, offset // SLOT))

    def fat_flush(self):
        for copy, sector in sorted(self.dirty):
            start = sector * SLOT
            self.writer.write(
                self.absolute(self.fat_offset + copy * self.fat_bytes + start),
                bytes(self.fats[copy][start:start + SLOT]))
        self.dirty.clear()

    def cluster_free(self, cluster):
        return all(self.fat_entry(copy, cluster) == 0
                   for copy in range(self.fat_count))

    def cluster_offset(self, cluster):
        return self.data_offset + (cluster - 2) * self.cluster_bytes

    def cluster_lba(self, cluster):
        return self.data_lba + (cluster - 2) * self.sectors_per_cluster

    def chain(self, first):
        if first < 2 or first >= self.clusters + 2:
            raise Refused(
                f"the FAT chain starts outside the data area (cluster {first})")
        out = []
        seen = set()
        cluster = first
        while True:
            if cluster in seen:
                raise Refused("the FAT chain loops")
            if len(out) > self.clusters:
                raise Refused("the FAT chain runs past the volume")
            out.append(cluster)
            seen.add(cluster)
            following = self.fat_entry(0, cluster)
            if following == 0 or following >= EOC_MIN:
                break
            if following < 2 or following >= self.clusters + 2:
                raise Refused(
                    f"the FAT chain leaves the data area (cluster {following})")
            cluster = following
        return out

    def allocate(self, count):
        """`count` free clusters: the first contiguous run when the volume has
        one, otherwise the first free clusters.  None when the volume is full."""
        if count < 1:
            raise Refused(f"cannot reserve {count} clusters")
        run_start = 0
        run_length = 0
        for cluster in range(2, self.clusters + 2):
            if self.cluster_free(cluster):
                if run_length == 0:
                    run_start = cluster
                run_length += 1
                if run_length == count:
                    return list(range(run_start, run_start + count))
            else:
                run_length = 0
        free = [cluster for cluster in range(2, self.clusters + 2)
                if self.cluster_free(cluster)]
        if len(free) >= count:
            return free[:count]
        return None

    def free_count(self):
        return sum(1 for cluster in range(2, self.clusters + 2)
                   if self.cluster_free(cluster))

    def mark_chain(self, chain):
        for index, cluster in enumerate(chain):
            value = EOC if index == len(chain) - 1 else chain[index + 1]
            for copy in range(self.fat_count):
                self.fat_set(copy, cluster, value)

    def zero_chain(self, chain):
        zero = bytes(self.cluster_bytes)
        for cluster in chain:
            self.writer.write(self.absolute(self.cluster_offset(cluster)), zero)

    @staticmethod
    def entry_name(entry):
        return (entry[0:8].decode("latin1").rstrip(),
                entry[8:11].decode("latin1").rstrip())

    def scan_root(self, wanted):
        """(entry, offset, free_offset) for `wanted` in the fixed root
        directory; entry is None when absent, free_offset when no slot is free."""
        free = None
        for index in range(self.root_entries):
            offset = self.root_offset + index * DIR_ENTRY_BYTES
            entry = self.read(offset, DIR_ENTRY_BYTES)
            if entry[0] == 0x00:
                if free is None:
                    free = offset
                return None, None, free
            if entry[0] == 0xE5:
                if free is None:
                    free = offset
                continue
            if wanted is not None and self.entry_name(entry) == wanted:
                return entry, offset, free
        return None, None, free

    def scan_directory(self, dir_cluster, wanted):
        """The same, over the clusters of a subdirectory."""
        free = None
        for cluster in self.chain(dir_cluster):
            base = self.cluster_offset(cluster)
            for index in range(self.cluster_bytes // DIR_ENTRY_BYTES):
                offset = base + index * DIR_ENTRY_BYTES
                entry = self.read(offset, DIR_ENTRY_BYTES)
                if entry[0] == 0x00:
                    if free is None:
                        free = offset
                    return None, None, free
                if entry[0] == 0xE5:
                    if free is None:
                        free = offset
                    continue
                if wanted is not None and self.entry_name(entry) == wanted:
                    return entry, offset, free
        return None, None, free


def directory_entry(name8, ext3, attr, first_cluster, size):
    entry = bytearray(DIR_ENTRY_BYTES)
    entry[0:8] = name8.encode("ascii").ljust(8)[:8]
    entry[8:11] = ext3.encode("ascii").ljust(3)[:3]
    entry[11] = attr
    put_le16(entry, 26, first_cluster)
    put_le32(entry, 28, size)
    return bytes(entry)


def header_valid(slot, capacity_slots):
    if len(slot) < SLOT or slot[0:MAGIC_BYTES] != MAGIC:
        return False
    if (le32(slot, 9) != VERSION or le32(slot, 13) != SLOT or
            le32(slot, 17) != capacity_slots):
        return False
    write_slot = le32(slot, 21)
    if write_slot < 1 or write_slot > capacity_slots:
        return False
    return crc32(slot, 0, 36) == le32(slot, 36)


def format_header(capacity_slots):
    slot = bytearray(SLOT)
    slot[0:MAGIC_BYTES] = MAGIC
    put_le32(slot, 9, VERSION)
    put_le32(slot, 13, SLOT)
    put_le32(slot, 17, capacity_slots)
    put_le32(slot, 21, 1)
    put_le32(slot, 36, crc32(slot, 0, 36))
    return bytes(slot)


def summary(volume, first_cluster, clusters, size, capacity_slots, already=False):
    """One machine-readable line: the file, its chain, its capacity and where
    the chain's first cluster sits (absolute LBA from the image start)."""
    where = "preallocated: already reserved " if already else "preallocated: "
    lba = volume.base // SLOT + volume.cluster_lba(first_cluster)
    return (f"{where}file=HFOC/INSTALL.LOG first_cluster={first_cluster} "
            f"clusters={clusters} size={size} capacity_slots={capacity_slots} "
            f"lba={lba}")


def reserve(volume, clusters):
    """Reserve \\HFOC\\INSTALL.LOG, or report that the image already carries it."""
    expected_size = clusters * volume.cluster_bytes
    capacity_slots = clusters * volume.slots_per_cluster
    writer = volume.writer

    hfoc, _, free_root = volume.scan_root(("HFOC", ""))
    if hfoc is not None:
        if not (hfoc[11] & ATTR_DIRECTORY):
            raise Refused("the root directory already carries a file named HFOC")
        dir_cluster = le16(hfoc, 26)
        volume.chain(dir_cluster)  # validate the directory's own chain
    else:
        if free_root is None:
            raise Refused("the root directory has no free entry for HFOC")
        run = volume.allocate(1)
        if run is None:
            raise Refused("NO_SPACE: no free cluster for the HFOC directory")
        dir_cluster = run[0]
        volume.zero_chain([dir_cluster])
        writer.write(volume.absolute(volume.cluster_offset(dir_cluster)),
                     directory_entry(".", "", ATTR_DIRECTORY, dir_cluster, 0))
        writer.write(volume.absolute(volume.cluster_offset(dir_cluster) +
                                     DIR_ENTRY_BYTES),
                     directory_entry("..", "", ATTR_DIRECTORY, 0, 0))
        volume.mark_chain([dir_cluster])
        volume.fat_flush()
        writer.write(volume.absolute(free_root),
                     directory_entry("HFOC", "", ATTR_DIRECTORY, dir_cluster, 0))

    entry, _, free_slot = volume.scan_directory(dir_cluster, ("INSTALL", "LOG"))
    if entry is not None:
        if entry[11] & ATTR_DIRECTORY:
            raise Refused("HFOC/INSTALL.LOG exists as a directory")
        first_cluster = le16(entry, 26)
        size = le32(entry, 28)
        chain = volume.chain(first_cluster)
        if len(chain) == clusters and size == expected_size:
            for index, cluster in enumerate(chain):
                expected = chain[index + 1] if index + 1 < len(chain) else None
                for copy in range(volume.fat_count):
                    value = volume.fat_entry(copy, cluster)
                    valid = value >= EOC_MIN if expected is None else value == expected
                    if not valid:
                        raise Refused(
                            "HFOC/INSTALL.LOG already has the requested "
                            "capacity but its FAT copies do not agree; "
                            "refusing to touch it")
            slot = volume.read(volume.cluster_offset(chain[0]), SLOT)
            if header_valid(slot, capacity_slots):
                return summary(volume, chain[0], len(chain), size,
                               capacity_slots, already=True)
            raise Refused(
                "HFOC/INSTALL.LOG already has the requested capacity but its "
                "slot-0 header is not a valid version-1 journal header; "
                "refusing to touch it")
        raise Refused(
            f"HFOC/INSTALL.LOG already exists with {len(chain)} clusters and "
            f"size {size}; requested {clusters} clusters / {expected_size} "
            f"bytes; refusing to resize")

    if free_slot is None:
        raise Refused("the HFOC directory has no free entry for INSTALL.LOG")
    chain = volume.allocate(clusters)
    if chain is None:
        raise Refused(
            f"NO_SPACE: need {clusters} free clusters, "
            f"{volume.free_count()} available")
    volume.zero_chain(chain)
    writer.write(volume.absolute(volume.cluster_offset(chain[0])),
                 format_header(capacity_slots))
    volume.mark_chain(chain)
    volume.fat_flush()
    writer.write(volume.absolute(free_slot),
                 directory_entry("INSTALL", "LOG", ATTR_ARCHIVE, chain[0],
                                 expected_size))
    return summary(volume, chain[0], len(chain), expected_size, capacity_slots)


def locate_partition(image, offset):
    """(volume base, partition start, partition end) in image bytes."""
    if offset is not None:
        if offset < 0 or offset % SLOT:
            raise Refused(
                f"volume offset {offset} is not a multiple of {SLOT} bytes")
        if offset >= image.size:
            raise Refused(f"volume offset {offset} is outside the image")
        return offset, offset, image.size

    mbr = image.read(0, SLOT)
    if mbr[510:512] != b"\x55\xaa":
        raise Refused("the image has no MBR signature at bytes 510..511")
    found = []
    for index in range(4):
        entry = mbr[446 + 16 * index:446 + 16 * (index + 1)]
        if entry[4] == 0xEF:
            found.append(entry)
    if len(found) != 1:
        raise Refused(
            f"the MBR carries {len(found)} type-0xEF partitions, "
            f"expected exactly one")
    entry = found[0]
    lba = le32(entry, 8)
    sectors = le32(entry, 12)
    base = lba * SLOT
    if lba < 1 or base >= image.size:
        raise Refused(f"the 0xEF partition starts at LBA {lba}, outside the image")
    if sectors < 1:
        raise Refused("the 0xEF partition declares no sectors")
    end = base + sectors * SLOT
    if end > image.size:
        raise Refused(
            f"the 0xEF partition ends at byte {end}, past the image's "
            f"{image.size} bytes")
    return base, base, end


def run(args):
    image = Image(args.image)
    writer = None
    try:
        base, part_start, part_end = locate_partition(image, args.offset)
        writer = PartitionWriter(image, part_start, part_end)
        volume = Fat12Volume(image, base, writer)
        writer.end = min(writer.end, base + volume.volume_bytes)
        line = reserve(volume, args.clusters)
        writer.verify()
        image.flush()
        return line
    except BaseException:
        if writer is not None and writer.undo:
            writer.rollback()
            image.flush()
        raise
    finally:
        image.close()


def main():
    parser = argparse.ArgumentParser(
        description="reserve HFOC/INSTALL.LOG in an installer medium's FAT12 ESP")
    parser.add_argument("image", help="raw installer ISO or disk image")
    parser.add_argument("--clusters", type=int, default=DEFAULT_CLUSTERS,
                        help=f"clusters to reserve (default {DEFAULT_CLUSTERS}, "
                             f"at most {MAX_CLUSTERS})")
    parser.add_argument("--offset", type=lambda value: int(value, 0),
                        help="byte offset of the FAT12 volume (skips MBR detection)")
    parser.add_argument("--quiet", action="store_true",
                        help="print nothing on success")
    args = parser.parse_args()
    if args.clusters < 1 or args.clusters > MAX_CLUSTERS:
        print(f"preallocated: --clusters must be 1..{MAX_CLUSTERS}",
              file=sys.stderr)
        return EXIT_REFUSED

    try:
        line = run(args)
    except Refused as error:
        print(f"preallocated: {error}", file=sys.stderr)
        return EXIT_REFUSED
    except (Failed, OSError) as error:
        print(f"preallocated: {error}", file=sys.stderr)
        return EXIT_ERROR

    if not args.quiet:
        print(line)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
