#!/usr/bin/env python3
"""Prepare the raw image of an HFOC diagnostic stick.

ITEM-582: the installed machine may write its diagnostic journal only to the
medium it booted from, so a diagnostic export needs a second, separately
prepared stick.  The kernel binds that stick only when the identity is exact:

  * the MBR signature is 0x55AA and the MBR disk id (bytes 440..443) is the
    diagnostic constant 0x48464344 (the four bytes "HFCD", little-endian);
  * the first partition entry (bytes 446..461) is type 0x01 (FAT12) and names
    an extent that lies inside the device;
  * the FAT12 volume carries a preallocated \\HFOC\\INSTALL.LOG whose slot-0
    header frames the capacity the kernel may write.

A random FAT volume has neither the disk id nor the reservation, so the stick
is opted in by what it carries, never by being the first device attached.  The
kernel allocates nothing (see install_fat12_journal.c): everything that
allocates - the HFOC directory, the file's cluster chain and the slot-0 header -
is done here, once, with the same proven builder the installer medium uses
(scripts/hfoc-install-media-prealloc.py).  That builder is imported, not
copied: its Image, PartitionWriter, Fat12Volume, reserve() and header helpers
are the ones that write the image, so the reservation is byte-for-byte the one
the QEMU proofs already trust.

Image layout (defaults):

    byte 0            MBR, 512 bytes, 0x55AA at 510..511
    bytes 432..439    zero
    bytes 440..443    disk id 0x48464344 -> 44 43 46 48
    bytes 444..445    zero
    byte  446         partition entry 1: 0x80, CHS start 00 02 00, type 0x01,
                      CHS end, first LBA 2048, length size_mib*2048 - 2048
    bytes 462..511    zero; bytes 512..1048575 zero
    byte 1048576      FAT12 BPB ("HFOC    "), 512-byte sectors, 1 reserved
                      sector, 2 FATs, 16 root entries, media 0xF8, total
                      sectors = partition sectors, sectors-per-FAT computed for
                      the resulting cluster count
    then the zeroed FATs and root directory, and the reserved journal file.

FAT12 cluster size: the specification asks for one 512-byte sector per cluster
with an 8 MiB image (a 7 MiB partition).  One sector per cluster over 14336
sectors yields 14250 data clusters, and no FAT12 volume may carry 4085 or more
(FAT12_CLUSTER_LIMIT here, the reader and the kernel's volume_geometry() all
refuse at that point).  The BPB therefore uses the smallest power-of-two
cluster size that keeps the real cluster count in 2..4084 - 4 sectors (2048
bytes) for the default 8 MiB image, giving 3578 clusters and an 11-sector FAT.
A partition small enough for 512-byte clusters (at most 2 MiB, i.e.
--size-mib up to 3) keeps one sector per cluster exactly as specified.
Everything else in the requested layout is unchanged: the partition still runs
from byte 1048576 to the end of the image and the volume covers it completely.

Journal size: clusters = journal_kib * 1024 // cluster_bytes, at most the
builder's MAX_CLUSTERS (512), the kernel's FAT12_JOURNAL_MAX_CLUSTERS chain
bound.  A refusal (exit 2) never touches the output: the image is built beside
the target and renamed into place only after the reservation is verified and
the file is flushed, so an existing image survives both a refusal and a
failure.  --force replaces it.

Reading the stick back: the volume starts at byte 1048576, so
`scripts/hfoc-install-log-read.py --offset 1048576 IMAGE` reads it today (its
MBR scan looks for the installer medium's type-0xEF entry; this stick's entry
is the diagnostic type 0x01, as the kernel requires).

Usage:
  python3 scripts/hfoc-diag-stick-prepare.py --output <path> [--size-mib N]
                                             [--journal-kib J] [--force]

  --output PATH    raw image to write (required; existing file needs --force)
  --size-mib N     whole image size in MiB (default 8); the FAT12 partition
                   starts at LBA 2048, so it is N-1 MiB
  --journal-kib J  reserved journal size in KiB (default 256)
  --force          replace an existing regular file

Exit status: 0 on success, 1 on an I/O or builder-loading error, 2 on a
refusal.  Only regular files are written; a block device or a directory named
by --output is refused, so this tool never touches a device.
"""

import argparse
import hashlib
import importlib.util
import os
import pathlib
import struct
import sys

SLOT = 512
MIB = 1024 * 1024
SECTORS_PER_MIB = MIB // SLOT

# kernel/include/install_medium.h: INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID.
MBR_DISK_ID = 0x48464344  # "HFCD" little-endian
MBR_PARTITION_OFFSET = 446
MBR_PARTITION_SIZE = 16
MBR_SIGNATURE_OFFSET = 510
PARTITION_START_LBA = 2048
PARTITION_TYPE_FAT12 = 0x01

# Geometry fixed by the diagnostic stick contract.
BYTES_PER_SECTOR = SLOT
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 16
MEDIA_DESCRIPTOR = 0xF8
FAT12_CLUSTER_LIMIT = 4085  # clusters < 4085, exactly as the reader checks
MAX_SECTORS_PER_CLUSTER = 128  # 64 KiB clusters: the FAT12 ceiling
SECTORS_PER_TRACK = 32
HEADS = 64
# Padded here, never inline: a BPB slice assignment of the wrong length would
# resize the 512-byte sector instead of failing.
OEM_NAME = b"HFOC".ljust(8)
VOLUME_LABEL = b"HFOC-DIAG".ljust(11)
FS_TYPE = b"FAT12".ljust(8)

DEFAULT_SIZE_MIB = 8
DEFAULT_JOURNAL_KIB = 256

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_REFUSED = 2

BUILDER_NAME = "hfoc-install-media-prealloc.py"


class Refused(Exception):
    """The requested image cannot exist as specified."""


class BuilderFailure(Exception):
    """The prealloc builder named by the repository cannot be loaded."""


def put_le16(buf, off, value):
    struct.pack_into("<H", buf, off, value & 0xFFFF)


def put_le32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def load_builder(path):
    """The proven reservation builder, loaded as a module by path."""
    if not path.is_file():
        raise BuilderFailure(f"the prealloc builder is missing at {path}")
    spec = importlib.util.spec_from_file_location(
        "hfoc_install_media_prealloc", path)
    if spec is None or spec.loader is None:
        raise BuilderFailure(f"cannot load the prealloc builder at {path}")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except (OSError, ImportError) as error:
        raise BuilderFailure(f"cannot load the prealloc builder at {path}: {error}")
    return module


def chs(lba, heads=HEADS, sectors=SECTORS_PER_TRACK):
    """Three CHS bytes for `lba`, saturated the way a BIOS table does."""
    per_cylinder = heads * sectors
    if lba >= 1024 * per_cylinder:
        return b"\xfe\xff\xff"
    cylinder = lba // per_cylinder
    head = (lba // sectors) % heads
    sector = (lba % sectors) + 1
    return bytes([head, (sector & 0x3F) | ((cylinder >> 2) & 0xC0),
                  cylinder & 0xFF])


def build_mbr(partition_sectors):
    """The diagnostic MBR: signature, disk id and the FAT12 partition entry."""
    mbr = bytearray(SLOT)
    put_le32(mbr, 440, MBR_DISK_ID)
    entry = bytearray(MBR_PARTITION_SIZE)
    entry[0] = 0x80  # boot flag
    entry[1:4] = b"\x00\x02\x00"  # CHS start, as the contract fixes it
    entry[4] = PARTITION_TYPE_FAT12
    entry[5:8] = chs(PARTITION_START_LBA + partition_sectors - 1)
    put_le32(entry, 8, PARTITION_START_LBA)
    put_le32(entry, 12, partition_sectors)
    mbr[MBR_PARTITION_OFFSET:MBR_PARTITION_OFFSET + MBR_PARTITION_SIZE] = entry
    mbr[MBR_SIGNATURE_OFFSET:MBR_SIGNATURE_OFFSET + 2] = b"\x55\xaa"
    return bytes(mbr)


def build_bpb(geometry):
    """The FAT12 BPB of the diagnostic volume, with its own 0x55AA."""
    bpb = bytearray(SLOT)
    bpb[0:3] = b"\xeb\x3c\x90"  # short jump over the BPB
    bpb[3:11] = OEM_NAME  # 8 bytes, padded
    put_le16(bpb, 11, geometry["bytes_per_sector"])
    bpb[13] = geometry["sectors_per_cluster"]
    put_le16(bpb, 14, RESERVED_SECTORS)
    bpb[16] = FAT_COUNT
    put_le16(bpb, 17, ROOT_ENTRIES)
    total = geometry["total_sectors"]
    if total <= 0xFFFF:
        put_le16(bpb, 19, total)
    else:
        put_le32(bpb, 32, total)
    bpb[21] = MEDIA_DESCRIPTOR
    put_le16(bpb, 22, geometry["sectors_per_fat"])
    put_le16(bpb, 24, SECTORS_PER_TRACK)
    put_le16(bpb, 26, HEADS)
    put_le32(bpb, 28, PARTITION_START_LBA)  # hidden sectors
    bpb[36] = 0x80  # drive number
    bpb[38] = 0x29  # extended boot signature
    put_le32(bpb, 39, MBR_DISK_ID)  # volume id, the stick's own identity
    bpb[43:54] = VOLUME_LABEL  # 11 bytes, padded
    bpb[54:62] = FS_TYPE  # 8 bytes, padded
    bpb[MBR_SIGNATURE_OFFSET:MBR_SIGNATURE_OFFSET + 2] = b"\x55\xaa"
    return bytes(bpb)


def compute_geometry(total_sectors):
    """The FAT12 geometry whose FAT is big enough for its real cluster count.

    The smallest power-of-two cluster size that keeps the cluster count inside
    0 < clusters < 4085 wins, and the sectors-per-FAT is iterated until it
    holds cluster 0 .. cluster_count+1 (floor(spf * 512 * 2 / 3) entries),
    which is exactly what the builder, the reader and the kernel check.
    """
    root_sectors = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
    if total_sectors < 2:
        raise Refused("the partition declares no sectors")

    sectors_per_cluster = 1
    while sectors_per_cluster <= MAX_SECTORS_PER_CLUSTER:
        sectors_per_fat = 1
        while True:
            overhead = (RESERVED_SECTORS + FAT_COUNT * sectors_per_fat +
                        root_sectors)
            if overhead >= total_sectors:
                raise Refused(
                    f"a partition of {total_sectors} sectors is too small for "
                    f"a FAT12 volume")
            clusters = (total_sectors - overhead) // sectors_per_cluster
            entries = clusters + 2
            need_bytes = (entries * 3 + 1) // 2
            need_sectors = (need_bytes + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
            if need_sectors <= sectors_per_fat:
                break
            sectors_per_fat = need_sectors
            if sectors_per_fat > 0xFFFF:
                raise Refused("the FAT would not fit the BPB's 16-bit field")
        if clusters < 2:
            raise Refused(
                f"a partition of {total_sectors} sectors holds only "
                f"{clusters} clusters, too few for a FAT12 volume")
        if clusters < FAT12_CLUSTER_LIMIT:
            fat_bytes = sectors_per_fat * BYTES_PER_SECTOR
            if (fat_bytes * 2) // 3 < clusters + 2:
                raise Refused("the FAT is too short for the cluster count")
            return {
                "bytes_per_sector": BYTES_PER_SECTOR,
                "sectors_per_cluster": sectors_per_cluster,
                "reserved_sectors": RESERVED_SECTORS,
                "fat_count": FAT_COUNT,
                "root_entries": ROOT_ENTRIES,
                "root_sectors": root_sectors,
                "sectors_per_fat": sectors_per_fat,
                "total_sectors": total_sectors,
                "clusters": clusters,
                "cluster_bytes": sectors_per_cluster * BYTES_PER_SECTOR,
                "fat_offset": RESERVED_SECTORS * BYTES_PER_SECTOR,
                "root_offset": (RESERVED_SECTORS +
                                FAT_COUNT * sectors_per_fat) * BYTES_PER_SECTOR,
                "data_offset": (RESERVED_SECTORS +
                                FAT_COUNT * sectors_per_fat +
                                root_sectors) * BYTES_PER_SECTOR,
            }
        sectors_per_cluster *= 2

    raise Refused(
        f"a partition of {total_sectors} sectors cannot be FAT12 even with "
        f"{MAX_SECTORS_PER_CLUSTER}-sector clusters; use --size-mib 256 or less")


def write_image(path, label, image_bytes, mbr, base, bpb, geometry):
    """Create the image and lay down the MBR, BPB, zeroed FATs and root.

    `path` is the temporary file being built and `label` is the path the caller
    named, so a failure reads as a failure of the output the owner asked for.
    """
    fat_bytes = (geometry["fat_count"] * geometry["sectors_per_fat"] *
                 BYTES_PER_SECTOR)
    root_bytes = geometry["root_sectors"] * BYTES_PER_SECTOR
    try:
        with open(path, "wb") as image:
            image.write(mbr)
            image.truncate(image_bytes)  # zero-filled partition area
            image.seek(base)
            image.write(bpb)
            image.seek(base + geometry["fat_offset"])
            image.write(bytes(fat_bytes))
            image.seek(base + geometry["root_offset"])
            image.write(bytes(root_bytes))
            image.flush()
            os.fsync(image.fileno())
    except OSError as error:
        raise OSError(
            f"cannot write {label}: {error.strerror or error}") from error


def journal_clusters(builder, volume, journal_kib):
    """The requested reservation in whole clusters, or a refusal."""
    if journal_kib < 1:
        raise Refused("--journal-kib must be at least 1")
    clusters = journal_kib * 1024 // volume.cluster_bytes
    if clusters < 2:
        raise Refused(
            f"--journal-kib {journal_kib} is smaller than two clusters of "
            f"{volume.cluster_bytes} bytes; ask for at least "
            f"{(2 * volume.cluster_bytes + 1023) // 1024} KiB")
    if clusters > builder.MAX_CLUSTERS:
        raise Refused(
            f"--journal-kib {journal_kib} needs {clusters} clusters, but the "
            f"kernel's chain bound is {builder.MAX_CLUSTERS} "
            f"(FAT12_JOURNAL_MAX_CLUSTERS); ask for at most "
            f"{builder.MAX_CLUSTERS * volume.cluster_bytes // 1024} KiB")
    # The HFOC directory needs one further cluster, so the volume must hold
    # the journal plus that cluster.
    if clusters + 1 > volume.clusters:
        raise Refused(
            f"the volume has {volume.clusters} data clusters; the journal "
            f"needs {clusters} plus one for the HFOC directory; grow "
            f"--size-mib or shrink --journal-kib")
    return clusters


def reserve_journal(builder, path, base, size_bytes, journal_kib):
    """Run the proven builder over the fresh volume and return (line, info)."""
    image = builder.Image(path)
    writer = None
    try:
        writer = builder.PartitionWriter(image, base, base + size_bytes)
        volume = builder.Fat12Volume(image, base, writer)
        clusters = journal_clusters(builder, volume, journal_kib)
        line = builder.reserve(volume, clusters)
        writer.verify()
        image.flush()
        os.fsync(image.f.fileno())
        return line, {
            "clusters": clusters,
            "cluster_bytes": volume.cluster_bytes,
            "capacity_slots": clusters * volume.slots_per_cluster,
        }
    except builder.Refused as error:
        if writer is not None and writer.undo:
            writer.rollback()
            image.flush()
        message = str(error)
        if message.startswith("NO_SPACE"):
            message += " (grow --size-mib or shrink --journal-kib)"
        raise Refused(message) from error
    except BaseException:
        if writer is not None and writer.undo:
            writer.rollback()
            image.flush()
        raise
    finally:
        image.close()


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as image:
        for chunk in iter(lambda: image.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(args):
    output = os.path.abspath(args.output)
    if os.path.lexists(output):
        if not os.path.isfile(output):
            raise Refused(
                f"{args.output} exists and is not a regular file; refusing to "
                f"write a device or a directory")
        if not args.force:
            raise Refused(
                f"{args.output} already exists; refusing to overwrite it "
                f"without --force")
    if args.size_mib < 2:
        raise Refused("--size-mib must be at least 2 (the partition starts at "
                      "LBA 2048)")

    image_bytes = args.size_mib * MIB
    partition_sectors = args.size_mib * SECTORS_PER_MIB - PARTITION_START_LBA
    if partition_sectors < 1:
        raise Refused("--size-mib leaves no room for the FAT12 partition")
    base = PARTITION_START_LBA * SLOT
    geometry = compute_geometry(partition_sectors)

    builder = load_builder(pathlib.Path(__file__).with_name(BUILDER_NAME))
    mbr = build_mbr(partition_sectors)
    bpb = build_bpb(geometry)

    temporary = f"{output}.tmp-{os.getpid()}"
    replaced = False
    try:
        write_image(temporary, args.output, image_bytes, mbr, base, bpb,
                    geometry)
        line, info = reserve_journal(
            builder, temporary, base, partition_sectors * SLOT, args.journal_kib)
        os.replace(temporary, output)
        replaced = True
    finally:
        if not replaced:
            try:
                os.unlink(temporary)
            except OSError:
                pass

    requested = args.journal_kib * 1024
    if requested != info["clusters"] * info["cluster_bytes"]:
        print(f"diag-stick: note: --journal-kib {args.journal_kib} rounds down "
              f"to {info['clusters'] * info['cluster_bytes'] // 1024} KiB "
              f"({info['clusters']} clusters of {info['cluster_bytes']} bytes)",
              file=sys.stderr)

    digest = sha256_of(output)
    entry = mbr[MBR_PARTITION_OFFSET:MBR_PARTITION_OFFSET + MBR_PARTITION_SIZE]
    print(f"diag-stick: output={args.output}")
    print(f"diag-stick: size={os.path.getsize(output)}")
    print(f"diag-stick: sha256={digest}")
    print(f"diag-stick: mbr_disk_id={MBR_DISK_ID:08X}")
    print(f"diag-stick: partition=1 type=0x{PARTITION_TYPE_FAT12:02X} "
          f"start_lba={PARTITION_START_LBA} length={partition_sectors} "
          f"entry={entry.hex()}")
    print(f"diag-stick: volume=fat12 bytes_per_sector="
          f"{geometry['bytes_per_sector']} sectors_per_cluster="
          f"{geometry['sectors_per_cluster']} sectors_per_fat="
          f"{geometry['sectors_per_fat']} clusters={geometry['clusters']} "
          f"journal_clusters={info['clusters']} "
          f"capacity_slots={info['capacity_slots']}")
    print(line)
    return EXIT_OK


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="prepare a raw HFOC diagnostic stick image")
    parser.add_argument("--output", required=True,
                        help="raw image to write")
    parser.add_argument("--size-mib", type=int, default=DEFAULT_SIZE_MIB,
                        help=f"whole image size in MiB (default "
                             f"{DEFAULT_SIZE_MIB}); the FAT12 partition starts "
                             f"at LBA 2048, so it is size_mib-1 MiB")
    parser.add_argument("--journal-kib", type=int, default=DEFAULT_JOURNAL_KIB,
                        help=f"reserved journal size in KiB (default "
                             f"{DEFAULT_JOURNAL_KIB})")
    parser.add_argument("--force", action="store_true",
                        help="replace an existing regular file")
    args = parser.parse_args(argv)

    try:
        return run(args)
    except Refused as error:
        print(f"diag-stick: {error}", file=sys.stderr)
        return EXIT_REFUSED
    except BuilderFailure as error:
        print(f"diag-stick: {error}", file=sys.stderr)
        return EXIT_ERROR
    except OSError as error:
        print(f"diag-stick: {error}", file=sys.stderr)
        return EXIT_ERROR
    except Exception as error:  # builder.Failed and unexpected write errors
        print(f"diag-stick: {error}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
