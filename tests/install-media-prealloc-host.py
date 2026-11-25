#!/usr/bin/env python3
"""Host proof for scripts/hfoc-install-media-prealloc.py (ITEM-521 increment 12).

The owner's answer (c) to the ITEM-520 fork says the build of an installer
medium reserves \\HFOC\\INSTALL.LOG in the ISO's FAT12 EFI-system partition,
and the kernel only reads the chain the build wrote.  This witness builds the
medium itself - MBR type 0xEF, the FAT12 BPB, both FAT copies, the root
directory, an EFI directory with a dummy BOOTX64.EFI and free space - and
drives the preallocator as a subprocess, so the reservation is checked against
the documented contract rather than against the writer's own code.

Cases: a first run reserves the file and writes the slot-0 header; the chain is
one contiguous run and both FAT copies agree; every reserved cluster but slot 0
is zero; a second run is a byte-identical no-op and says so; two first runs on
identical inputs are byte-identical; a medium that already carries a file of a
different capacity is refused and not resized; a full volume is refused with
NO_SPACE; an image without an ESP is refused; every refusal leaves the image
byte for byte as it was; bytes outside the FAT12 partition never change; and
`--offset` finds the volume in a bare dump.

Usage: python3 tests/install-media-prealloc-host.py [--preallocator PATH]
                                                    [--keep DIR]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SECTOR = 512
SPC = 2
RESERVED = 1
NFAT = 2
SPF = 9
ROOT_ENTRIES = 64
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
DATA_LBA = RESERVED + NFAT * SPF + ROOT_SECTORS
TOTAL_SECTORS = 700
ESP_LBA = 64
CLUSTERS = (TOTAL_SECTORS - DATA_LBA) // SPC
MAGIC = b"HFOC-ILOG"
DEFAULT_CLUSTERS = 256
EOC_MIN = 0xFF8

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


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put_le16(buf, off, value):
    struct.pack_into("<H", buf, off, value & 0xFFFF)


def put_le32(buf, off, value):
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def dirent(name8, ext3, attr, first_cluster, size):
    entry = bytearray(32)
    entry[0:8] = name8.encode("ascii").ljust(8)[:8]
    entry[8:11] = ext3.encode("ascii").ljust(3)[:3]
    entry[11] = attr
    put_le16(entry, 26, first_cluster)
    put_le32(entry, 28, size)
    return bytes(entry)


def entry_name(entry):
    return (entry[0:8].decode("latin1").rstrip(),
            entry[8:11].decode("latin1").rstrip())


def fat_set(fat, cluster, value):
    off = cluster + cluster // 2
    word = fat[off] | (fat[off + 1] << 8)
    if cluster & 1:
        word = (word & 0x000F) | ((value & 0x0FFF) << 4)
    else:
        word = (word & 0xF000) | (value & 0x0FFF)
    fat[off] = word & 0xFF
    fat[off + 1] = (word >> 8) & 0xFF


def fat_get(fat, cluster):
    off = cluster + cluster // 2
    word = fat[off] | (fat[off + 1] << 8)
    return (word >> 4) if cluster & 1 else (word & 0x0FFF)


def build_volume(total_sectors=TOTAL_SECTORS, fill=False, only_free=None):
    """The FAT12 volume: BPB, two FATs, root with EFI, EFI/BOOTX64.EFI, space."""
    volume = bytearray(total_sectors * SECTOR)

    boot = bytearray(SECTOR)
    put_le16(boot, 11, SECTOR)
    boot[13] = SPC
    put_le16(boot, 14, RESERVED)
    boot[16] = NFAT
    put_le16(boot, 17, ROOT_ENTRIES)
    put_le16(boot, 19, total_sectors)
    boot[21] = 0xF8
    put_le16(boot, 22, SPF)
    boot[510:512] = b"\x55\xaa"
    volume[0:SECTOR] = boot

    fat = bytearray(SPF * SECTOR)
    fat_set(fat, 0, 0xFF8)
    fat_set(fat, 1, 0xFFF)
    fat_set(fat, 2, 0xFFF)   # EFI directory
    fat_set(fat, 3, 0xFFF)   # BOOTX64.EFI
    clusters = (total_sectors - DATA_LBA) // SPC
    if only_free is not None:
        keep = set(only_free)
        for cluster in range(2, clusters + 2):
            if cluster not in keep:
                fat_set(fat, cluster, 0xFFF)
    elif fill:
        for cluster in range(2, clusters + 2):
            fat_set(fat, cluster, 0xFFF)
    for copy in range(NFAT):
        off = (RESERVED + copy * SPF) * SECTOR
        volume[off:off + len(fat)] = fat

    root = (RESERVED + NFAT * SPF) * SECTOR
    volume[root:root + 32] = dirent("EFI", "", 0x10, 2, 0)

    efi = DATA_LBA * SECTOR
    volume[efi:efi + 32] = dirent("BOOTX64", "EFI", 0x20, 3, 700)
    payload = (bytes(range(256)) * 3)[:700]
    data = (DATA_LBA + (3 - 2) * SPC) * SECTOR
    volume[data:data + len(payload)] = payload
    return bytes(volume)


def make_medium(path, *, mbr=True, mbr_signature=True, partition=True,
                partition_type=0xEF, esp_lba=ESP_LBA, fill=False,
                only_free=None):
    volume = build_volume(fill=fill, only_free=only_free)
    base = esp_lba * SECTOR
    image = bytearray(base + len(volume))
    if mbr:
        if mbr_signature:
            image[510:512] = b"\x55\xaa"
        if partition:
            image[446 + 4] = partition_type
            put_le32(image, 446 + 8, esp_lba)
            put_le32(image, 446 + 12, TOTAL_SECTORS)
    image[base:base + len(volume)] = volume
    with open(path, "wb") as handle:
        handle.write(image)
    return path


def read_file(path):
    with open(path, "rb") as handle:
        return handle.read()


def patch_bytes(path, offset, data):
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(data)


class Geometry:
    """The witness's own reading of the BPB, independent of the preallocator."""

    def __init__(self, data, base):
        bpb = data[base:base + SECTOR]
        self.base = base
        self.bps = le16(bpb, 11)
        self.spc = bpb[13]
        self.reserved = le16(bpb, 14)
        self.nfat = bpb[16]
        self.root_entries = le16(bpb, 17)
        total16 = le16(bpb, 19)
        self.spf = le16(bpb, 22) or le32(bpb, 36)
        self.total = total16 or le32(bpb, 32)
        self.root_lba = self.reserved + self.nfat * self.spf
        root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_lba = self.root_lba + root_sectors
        self.clusters = (self.total - self.data_lba) // self.spc
        self.cluster_bytes = self.spc * self.bps
        self.slots_per_cluster = self.cluster_bytes // SECTOR

    def cluster_offset(self, cluster):
        return self.base + (self.data_lba + (cluster - 2) * self.spc) * SECTOR

    def cluster_lba(self, cluster):
        return self.data_lba + (cluster - 2) * self.spc

    def fat(self, data, copy=0):
        off = self.base + (self.reserved + copy * self.spf) * SECTOR
        return data[off:off + self.spf * SECTOR]

    def entry(self, data, cluster, index):
        off = self.cluster_offset(cluster) + index * 32
        return data[off:off + 32]

    def root_find(self, data, name8, ext3):
        off = self.base + self.root_lba * SECTOR
        for index in range(self.root_entries):
            entry = data[off + index * 32:off + index * 32 + 32]
            if entry[0] == 0x00:
                return None
            if entry[0] == 0xE5:
                continue
            if entry_name(entry) == (name8, ext3):
                return entry
        return None

    def dir_find(self, data, dir_cluster, name8, ext3):
        for cluster in read_chain(data, self, dir_cluster):
            for index in range(self.cluster_bytes // 32):
                entry = self.entry(data, cluster, index)
                if entry[0] == 0x00:
                    return None
                if entry[0] == 0xE5:
                    continue
                if entry_name(entry) == (name8, ext3):
                    return entry
        return None


def read_chain(data, geom, first):
    fat = geom.fat(data)
    out = []
    cluster = first
    while 2 <= cluster < geom.clusters + 2:
        out.append(cluster)
        following = fat_get(fat, cluster)
        if following == 0 or following >= EOC_MIN:
            break
        cluster = following
        if len(out) > geom.clusters:
            raise AssertionError("chain does not terminate")
    return out


def run_prealloc(preallocator, image, *args):
    return subprocess.run(
        [sys.executable, preallocator, image, *args],
        capture_output=True, text=True, check=False)


def parse_summary(text):
    line = ""
    for candidate in text.strip().splitlines():
        if candidate.startswith("preallocated: "):
            line = candidate
    if not line:
        return None
    fields = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def slot_zero(data, geom, first_cluster):
    off = geom.cluster_offset(first_cluster)
    return data[off:off + SECTOR]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser.add_argument("--preallocator", default=os.path.join(
        root, "scripts", "hfoc-install-media-prealloc.py"))
    parser.add_argument("--keep", help="directory to keep the generated media in")
    args = parser.parse_args()

    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        work = args.keep
    else:
        work = tempfile.mkdtemp(prefix="hfoc-install-media-prealloc-")
    try:
        base = ESP_LBA * SECTOR
        part_start = base
        part_end = base + TOTAL_SECTORS * SECTOR
        capacity = DEFAULT_CLUSTERS
        capacity_bytes = capacity * SPC * SECTOR
        slot_count = capacity * (SPC * SECTOR // SECTOR)

        print("case: reserve HFOC/INSTALL.LOG in the ESP")
        fresh = make_medium(os.path.join(work, "fresh.img"))
        before = read_file(fresh)
        geom = Geometry(before, base)
        check("the witness volume is FAT12", 0 < geom.clusters < 4085, geom.clusters)
        result = run_prealloc(args.preallocator, fresh)
        check("first run exits 0", result.returncode == 0, result.stderr)
        fields = parse_summary(result.stdout)
        check("first run prints the summary line", fields is not None, result.stdout)
        check("summary names the file",
              fields is not None and fields.get("file") == "HFOC/INSTALL.LOG",
              result.stdout)
        check("summary names the requested cluster count",
              fields is not None and fields.get("clusters") == str(capacity),
              result.stdout)
        check("summary names the capacity in bytes",
              fields is not None and fields.get("size") == str(capacity_bytes),
              result.stdout)
        check("summary names the capacity in slots",
              fields is not None and fields.get("capacity_slots") == str(slot_count),
              result.stdout)

        after = read_file(fresh)
        check("bytes outside the FAT12 partition never change",
              before[:part_start] == after[:part_start] and
              before[part_end:] == after[part_end:])

        hfoc = geom.root_find(after, "HFOC", "")
        check("the root directory carries HFOC", hfoc is not None,
              "no HFOC entry")
        check("HFOC is a directory",
              hfoc is not None and (hfoc[11] & 0x10) != 0,
              hfoc[11] if hfoc else "")
        hfoc_cluster = le16(hfoc, 26) if hfoc else 0
        check("the HFOC chain checks out",
              hfoc is not None and read_chain(after, geom, hfoc_cluster) == [hfoc_cluster])

        log = geom.dir_find(after, hfoc_cluster, "INSTALL", "LOG")
        check("HFOC carries INSTALL.LOG", log is not None, "no INSTALL.LOG entry")
        check("INSTALL.LOG is a plain file",
              log is not None and (log[11] & 0x10) == 0 and (log[11] & 0x20) != 0,
              hex(log[11]) if log else "")
        first = le16(log, 26) if log else 0
        size = le32(log, 28) if log else 0
        check("the directory size is the whole capacity", size == capacity_bytes,
              f"{size} != {capacity_bytes}")
        check("the summary first_cluster is the entry's first cluster",
              fields is not None and fields.get("first_cluster") == str(first),
              result.stdout)
        check("the summary lba is the first cluster's sector",
              fields is not None and
              fields.get("lba") == str(base // SECTOR + geom.cluster_lba(first)),
              result.stdout)

        chain = read_chain(after, geom, first)
        check("the chain is exactly the reserved run",
              chain == list(range(first, first + capacity)),
              f"{chain[:4]}..{chain[-2:]} ({len(chain)})")
        fat0 = geom.fat(after, 0)
        fat1 = geom.fat(after, 1)
        check("each chain entry links to the next",
              all(fat_get(fat0, chain[i]) == chain[i + 1]
                  for i in range(len(chain) - 1)))
        check("the last chain entry is end-of-chain",
              bool(chain) and fat_get(fat0, chain[-1]) >= EOC_MIN,
              hex(fat_get(fat0, chain[-1])) if chain else "empty chain")
        check("both FAT copies agree over the chain",
              all(fat_get(fat0, cluster) == fat_get(fat1, cluster)
                  for cluster in chain))
        check("the clusters after the reservation stay free",
              all(fat_get(fat0, cluster) == 0
                  for cluster in range(first + capacity, geom.clusters + 2)))
        check("the EFI payload is untouched",
              after[geom.cluster_offset(3):geom.cluster_offset(3) + 700] ==
              (bytes(range(256)) * 3)[:700])

        header = slot_zero(after, geom, first)
        check("header magic", header[0:9] == MAGIC, header[0:9])
        check("header version", le32(header, 9) == 1, le32(header, 9))
        check("header slot size", le32(header, 13) == SECTOR, le32(header, 13))
        check("header capacity in slots", le32(header, 17) == slot_count,
              le32(header, 17))
        check("header write_slot is one", le32(header, 21) == 1, le32(header, 21))
        check("header CRC32 covers bytes 0..35",
              (zlib.crc32(header[0:36]) & 0xFFFFFFFF) == le32(header, 36),
              hex(le32(header, 36)))
        reserved = b"".join(
            after[geom.cluster_offset(cluster):
                  geom.cluster_offset(cluster) + geom.cluster_bytes]
            for cluster in chain)
        check("every reserved cluster but slot 0 is zero",
              reserved[SECTOR:] == bytes(len(reserved) - SECTOR))

        print("case: a second run is a no-op")
        snapshot = read_file(fresh)
        result = run_prealloc(args.preallocator, fresh)
        check("second run exits 0", result.returncode == 0, result.stderr)
        check("second run reports the reservation",
              "already reserved" in result.stdout, result.stdout)
        check("second run changes no byte", read_file(fresh) == snapshot)

        print("case: a reservation the kernel advanced stays reserved")
        advanced = os.path.join(work, "advanced.img")
        shutil.copyfile(fresh, advanced)
        header = bytearray(slot_zero(snapshot, geom, first))
        put_le32(header, 21, 5)
        put_le32(header, 36, zlib.crc32(bytes(header[0:36])) & 0xFFFFFFFF)
        patch_bytes(advanced, geom.cluster_offset(first), bytes(header))
        snapshot = read_file(advanced)
        result = run_prealloc(args.preallocator, advanced)
        check("an advanced commit point exits 0", result.returncode == 0,
              result.stderr)
        check("an advanced commit point reports the reservation",
              "already reserved" in result.stdout, result.stdout)
        check("an advanced commit point is not reset",
              read_file(advanced) == snapshot)

        print("case: a damaged reservation is refused")
        damaged = os.path.join(work, "damaged.img")
        shutil.copyfile(fresh, damaged)
        patch_bytes(damaged, geom.cluster_offset(first) + 36, b"\xff")
        snapshot = read_file(damaged)
        result = run_prealloc(args.preallocator, damaged)
        check("a damaged header exits 2", result.returncode == 2, result.stdout)
        check("a damaged header is named",
              "slot-0 header is not a valid version-1 journal header" in result.stderr,
              result.stderr)
        check("a damaged header changes nothing", read_file(damaged) == snapshot)

        print("case: an HFOC file instead of a directory is refused")
        hfoc_file = os.path.join(work, "hfoc-file.img")
        with open(hfoc_file, "wb") as handle:
            handle.write(before)
        root_free = base + geom.root_lba * SECTOR + 32
        patch_bytes(hfoc_file, root_free, dirent("HFOC", "", 0x20, 0, 0))
        snapshot = read_file(hfoc_file)
        result = run_prealloc(args.preallocator, hfoc_file)
        check("an HFOC file exits 2", result.returncode == 2, result.stdout)
        check("an HFOC file is named",
              "already carries a file named HFOC" in result.stderr, result.stderr)
        check("an HFOC file changes nothing", read_file(hfoc_file) == snapshot)

        print("case: a request beyond the kernel's chain bound is refused")
        too_big = make_medium(os.path.join(work, "too-big.img"))
        snapshot = read_file(too_big)
        result = run_prealloc(args.preallocator, too_big, "--clusters", "1024")
        check("more than 512 clusters exits 2", result.returncode == 2,
              result.stdout)
        check("more than 512 clusters changes nothing",
              read_file(too_big) == snapshot)

        print("case: two first runs are byte-identical")
        twin_a = make_medium(os.path.join(work, "twin-a.img"))
        twin_b = make_medium(os.path.join(work, "twin-b.img"))
        run_prealloc(args.preallocator, twin_a)
        run_prealloc(args.preallocator, twin_b)
        check("identical inputs produce identical images",
              read_file(twin_a) == read_file(twin_b))

        print("case: --quiet reserves silently")
        quiet = make_medium(os.path.join(work, "quiet.img"))
        result = run_prealloc(args.preallocator, quiet, "--quiet")
        check("quiet run exits 0", result.returncode == 0, result.stderr)
        check("quiet run prints nothing", result.stdout == "", result.stdout)

        print("case: an existing file of another capacity is never resized")
        small = make_medium(os.path.join(work, "small.img"))
        result = run_prealloc(args.preallocator, small, "--clusters", "8")
        check("the smaller reservation succeeds", result.returncode == 0,
              result.stderr)
        snapshot = read_file(small)
        result = run_prealloc(args.preallocator, small)
        check("a wider request exits 2", result.returncode == 2, result.stdout)
        check("the refusal names the resize",
              "refusing to resize" in result.stderr, result.stderr)
        check("the refusal changes nothing", read_file(small) == snapshot)

        print("case: a full volume refuses with NO_SPACE")
        full = make_medium(os.path.join(work, "full.img"), fill=True)
        snapshot = read_file(full)
        result = run_prealloc(args.preallocator, full)
        check("full volume exits 2", result.returncode == 2, result.stdout)
        check("full volume names NO_SPACE", "NO_SPACE" in result.stderr,
              result.stderr)
        check("full volume changes nothing", read_file(full) == snapshot)

        print("case: a refusal after the HFOC directory was written rolls back")
        one = make_medium(os.path.join(work, "one-free.img"),
                          only_free=[CLUSTERS + 1])
        snapshot = read_file(one)
        result = run_prealloc(args.preallocator, one)
        check("one free cluster exits 2", result.returncode == 2, result.stdout)
        check("one free cluster names NO_SPACE", "NO_SPACE" in result.stderr,
              result.stderr)
        check("the HFOC directory written before the refusal is rolled back",
              read_file(one) == snapshot)

        print("case: an image without an ESP is refused")
        wrong = make_medium(os.path.join(work, "wrong-type.img"),
                            partition_type=0x83)
        snapshot = read_file(wrong)
        result = run_prealloc(args.preallocator, wrong)
        check("wrong partition type exits 2", result.returncode == 2,
              result.stdout)
        check("wrong partition type names the ESP",
              "0xEF" in result.stderr, result.stderr)
        check("wrong partition type changes nothing", read_file(wrong) == snapshot)

        unsigned = make_medium(os.path.join(work, "no-mbr.img"),
                               mbr_signature=False)
        snapshot = read_file(unsigned)
        result = run_prealloc(args.preallocator, unsigned)
        check("an image without an MBR signature exits 2",
              result.returncode == 2, result.stdout)
        check("an image without an MBR signature changes nothing",
              read_file(unsigned) == snapshot)

        print("case: --offset names the volume directly")
        bare = make_medium(os.path.join(work, "bare.img"), mbr=False,
                           esp_lba=8)
        result = run_prealloc(args.preallocator, bare, "--offset", str(8 * SECTOR))
        check("--offset run exits 0", result.returncode == 0, result.stderr)
        bare_data = read_file(bare)
        bare_geom = Geometry(bare_data, 8 * SECTOR)
        bare_hfoc = bare_geom.root_find(bare_data, "HFOC", "")
        bare_log = (bare_geom.dir_find(bare_data, le16(bare_hfoc, 26),
                                       "INSTALL", "LOG")
                    if bare_hfoc else None)
        check("--offset reserves the file",
              bare_log is not None and le32(bare_log, 28) == capacity_bytes,
              "--offset found no file")
        check("--offset leaves the bytes before the volume alone",
              bare_data[:8 * SECTOR] == bytes(8 * SECTOR))
        snapshot = read_file(bare)
        result = run_prealloc(args.preallocator, bare, "--offset", "100")
        check("an unaligned --offset exits 2", result.returncode == 2,
              result.stdout)
        check("an unaligned --offset changes nothing",
              read_file(bare) == snapshot)

        print(f"\n{checks} checks, {failures} failures")
        if args.keep:
            print(f"kept: {work}")
        return 1 if failures else 0
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
