#!/usr/bin/env python3
"""Host proof for hfoc-install-log-read.py (ITEM-521 requirement 4).

Builds the media itself, from the documented contract only - the MBR, the FAT12
BPB, the tables, the chain and the version-1 slot frame are all written here, so
the reader is checked against the contract rather than against a writer that
shares its code.  The real medium written by the C prototypes is checked
separately by run-reader-host.sh (`--cross-check`), which reads the images the
QEMU proof left in item-521-qemu-proof/evidence/.

Cases: FAT12 file, preallocated FAT12 file (header write_slot is the commit
point), both extent header layouts, --file, --offset (a volume inside a whole
disk, a bare volume dump and a bogus offset), the machine-readable half (--json
for a FAT12 file, a raw extent, a mounted file, a torn record and a medium with
no journal), and the refusals - torn record, empty committed slot, broken header
CRC, FAT loop, no journal, short file.

Usage: python3 test-reader-host.py [--reader PATH] [--keep DIR]
"""
import argparse
import json
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
ROOT_ENTRIES = 240
TOTAL_SECTORS = 6144
SPF = 9
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
DATA_LBA = RESERVED + NFAT * SPF + ROOT_SECTORS
MAGIC = b"HFOC-ILOG"

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


def record(text):
    body = text.encode("ascii")
    slot = struct.pack("<II", len(body), zlib.crc32(body) & 0xFFFFFFFF) + body
    return slot.ljust(SECTOR, b"\x00")


def header(slot_count, write_slot):
    slot = bytearray(SECTOR)
    slot[0:9] = MAGIC
    struct.pack_into("<I", slot, 9, 1)
    struct.pack_into("<I", slot, 13, SECTOR)
    struct.pack_into("<I", slot, 17, slot_count)
    struct.pack_into("<I", slot, 21, write_slot)
    struct.pack_into("<I", slot, 36, zlib.crc32(bytes(slot[:36])) & 0xFFFFFFFF)
    return bytes(slot)


def journal_bytes(slot_count, records, write_slot=None):
    slots = [header(slot_count, len(records) + 1 if write_slot is None else write_slot)]
    slots += [record(text) for text in records]
    slots += [bytes(SECTOR)] * (slot_count - len(slots))
    return b"".join(slots)


def fat12_medium(path, slot_count, records, preallocated=False, corrupt=None):
    """A whole disk: MBR type 0xEF at LBA 64, FAT12 volume, \\HFOC\\INSTALL.LOG."""
    image = bytearray(64 * SECTOR + TOTAL_SECTORS * SECTOR)
    mbr_off = 446
    struct.pack_into("<B", image, mbr_off + 4, 0xEF)
    struct.pack_into("<I", image, mbr_off + 8, 64)
    struct.pack_into("<I", image, mbr_off + 12, TOTAL_SECTORS)
    image[510:512] = b"\x55\xaa"
    base = 64 * SECTOR

    def put(lba, data):
        image[base + lba * SECTOR:base + lba * SECTOR + len(data)] = data

    boot = bytearray(SECTOR)
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = SPC
    struct.pack_into("<H", boot, 14, RESERVED)
    boot[16] = NFAT
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, TOTAL_SECTORS)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, SPF)
    boot[510:512] = b"\x55\xaa"
    put(0, bytes(boot))

    fat = bytearray(SPF * SECTOR)

    def fat_set(cluster, value):
        off = cluster + cluster // 2
        word = struct.unpack_from("<H", fat, off)[0]
        fat[off:off + 2] = struct.pack(
            "<H", (word & 0xF000) | value if cluster % 2 == 0
            else (word & 0x000F) | (value << 4))

    fat_set(0, 0xFF8)
    fat_set(1, 0xFFF)
    clusters = (slot_count * SECTOR + SPC * SECTOR - 1) // (SPC * SECTOR)
    file_first = 3
    if corrupt == "fat-loop":
        fat_set(file_first, file_first)
    else:
        for i in range(clusters):
            fat_set(file_first + i,
                    0xFFF if i == clusters - 1 else file_first + i + 1)
    fat_set(2, 0xFFF)  # the HFOC directory
    put(RESERVED, bytes(fat))
    put(RESERVED + SPF, bytes(fat))

    def dirent(name, ext, attr, cluster, size):
        entry = bytearray(32)
        entry[0:8] = name.ljust(8)[:8].encode("ascii")
        entry[8:11] = ext.ljust(3)[:3].encode("ascii")
        entry[11] = attr
        struct.pack_into("<H", entry, 26, cluster)
        struct.pack_into("<I", entry, 28, size)
        return bytes(entry)

    root = dirent("HFOC", "", 0x10, 2, 0)
    put(RESERVED + NFAT * SPF, root)

    size = (slot_count if preallocated else len(records) + 1) * SECTOR
    dir_cluster = 2
    dir_bytes = (dirent(".", "", 0x10, dir_cluster, 0)
                 + dirent("..", "", 0x10, 0, 0)
                 + dirent("INSTALL", "LOG", 0x20, file_first, size))
    data_lba = DATA_LBA + (dir_cluster - 2) * SPC
    put(data_lba, dir_bytes)

    journal = bytearray(journal_bytes(slot_count, records))
    if corrupt == "header-crc":
        journal[36] ^= 0xFF
    if corrupt == "torn-record" and len(records) >= 2:
        journal[2 * SECTOR + 12] ^= 0xFF
    if corrupt == "empty-slot" and len(records) >= 2:
        journal[2 * SECTOR:3 * SECTOR] = bytes(SECTOR)
    for i in range((len(journal) + SPC * SECTOR - 1) // (SPC * SECTOR)):
        put(DATA_LBA + (file_first - 2) * SPC + i * SPC,
            bytes(journal[i * SPC * SECTOR:(i + 1) * SPC * SECTOR]))
    with open(path, "wb") as f:
        f.write(bytes(image))
    return path


def extent_medium(path, blocks, slot_count, records, prototype_layout=True):
    image = bytearray(blocks * SECTOR)
    first = blocks - 34 - 2048
    slot = bytearray(SECTOR)
    slot[0:9] = MAGIC
    struct.pack_into("<I", slot, 9, 1)
    struct.pack_into("<I", slot, 13, SECTOR)
    struct.pack_into("<I", slot, 17, slot_count)
    struct.pack_into("<I", slot, 21, len(records) + 1)
    if prototype_layout:
        struct.pack_into("<Q", slot, 25, first)
        struct.pack_into("<I", slot, 41, zlib.crc32(bytes(slot[:41])) & 0xFFFFFFFF)
    else:
        struct.pack_into("<I", slot, 36, zlib.crc32(bytes(slot[:36])) & 0xFFFFFFFF)
    body = bytes(slot) + b"".join(record(text) for text in records)
    image[first * SECTOR:first * SECTOR + len(body)] = body
    with open(path, "wb") as f:
        f.write(bytes(image))
    return path


def run(reader, *args):
    proc = subprocess.run([sys.executable, reader, *args],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reader", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "scripts",
        "hfoc-install-log-read.py"))
    ap.add_argument("--keep", help="directory to keep the generated media in")
    ap.add_argument("--cross-check", metavar="DIR",
                    help="also read media the QEMU proof left in DIR")
    ap.add_argument("--require-image", action="store_true",
                    help="fail instead of skipping when --cross-check media is absent")
    args = ap.parse_args()
    work = args.keep or tempfile.mkdtemp(prefix="hfoc-reader-")
    os.makedirs(work, exist_ok=True)
    p = lambda name: os.path.join(work, name)
    records = ["[BOOT] machine cpus=1 memory=512M firmware=UEFI",
               "[INSTALL-MEDIUM] state=MATCH reason=OK scope=WHOLE",
               "[PRODUCTION-INSTALL] refused reason=NO_DISK writes=0"]

    print("case: FAT12 file (option a)")
    fat12_medium(p("fat12.img"), 64, records)
    code, out, err = run(args.reader, "--disk", p("fat12.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("placement named", "fat12-file" in out, out)
    check("capacity from the header", "4 slots of 64" in out, out)
    check("all records in order", all(r in out for r in records), out)
    check("no warnings", err == "", err)

    print("case: preallocated FAT12 file (option c, header write_slot commits)")
    fat12_medium(p("prealloc.img"), 64, records, preallocated=True)
    code, out, err = run(args.reader, p("prealloc.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("placement named", "fat12-preallocated" in out, out)
    check("write_slot cuts the prefix", "4 slots of 64" in out, out)
    check("no empty-slot warnings", "empty" not in err, err)

    print("case: raw extent, prototype header layout (option b)")
    extent_medium(p("extent.img"), 10310, 64, records)
    code, out, err = run(args.reader, "--disk", p("extent.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("extent named with its lba", "extent at lba 8228" in out, out)
    check("all records in order", all(r in out for r in records), out)

    print("case: raw extent, core header layout")
    extent_medium(p("extent-core.img"), 10310, 64, records, prototype_layout=False)
    code, out, err = run(args.reader, "--disk", p("extent-core.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("all records in order", all(r in out for r in records), out)

    print("case: mounted file (--file)")
    with open(p("INSTALL.LOG"), "wb") as f:
        f.write(journal_bytes(64, records[:2]))
    code, out, err = run(args.reader, "--file", p("INSTALL.LOG"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("records printed", records[0] in out and records[1] in out, out)

    print("case: --offset names the FAT12 volume inside a whole disk")
    code, out, err = run(args.reader, "--offset", str(64 * SECTOR),
                         p("fat12.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("placement named", "fat12-file" in out, out)
    check("all records in order", all(r in out for r in records), out)
    check("no warnings", err == "", err)

    print("case: --offset 0 reads a bare volume dump")
    with open(p("fat12.img"), "rb") as f:
        whole = f.read()
    with open(p("volume.img"), "wb") as f:
        f.write(whole[64 * SECTOR:64 * SECTOR + TOTAL_SECTORS * SECTOR])
    code, out, err = run(args.reader, "--offset", "0", p("volume.img"))
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("all records in order", all(r in out for r in records), out)
    check("no warnings", err == "", err)

    print("case: a bogus --offset is malformed, not silent")
    code, out, err = run(args.reader, "--offset", str(SECTOR), p("fat12.img"))
    check("exit 1", code == 1, f"exit={code} err={err!r}")
    check("reason named", "malformed" in err, err)
    check("not called absent", "no journal" not in err, err)

    print("case: torn committed record is warned, not printed")
    fat12_medium(p("torn.img"), 64, records, corrupt="torn-record")
    code, out, err = run(args.reader, p("torn.img"))
    check("exit 0 (the rest of the journal is intact)", code == 0, f"exit={code}")
    check("torn warned on stderr", "torn" in err, err)
    check("torn text not printed", "state=MATCH" not in out, out)
    check("later record still printed", records[2] in out, out)

    print("case: empty committed slot is named")
    fat12_medium(p("empty.img"), 64, records, corrupt="empty-slot")
    code, out, err = run(args.reader, p("empty.img"))
    check("exit 0", code == 0, f"exit={code}")
    check("empty warned", "empty" in err, err)

    print("case: broken header CRC is malformed, not 'no journal'")
    fat12_medium(p("badcrc.img"), 64, records, corrupt="header-crc")
    code, out, err = run(args.reader, p("badcrc.img"))
    check("exit 1", code == 1, f"exit={code} err={err!r}")
    check("reason named", "malformed" in err and "CRC" in err, err)

    print("case: FAT chain loop is malformed")
    fat12_medium(p("loop.img"), 64, records, corrupt="fat-loop")
    code, out, err = run(args.reader, p("loop.img"))
    check("exit 1", code == 1, f"exit={code} err={err!r}")
    check("loop named", "loops" in err, err)

    print("case: medium without a journal")
    with open(p("blank.img"), "wb") as f:
        f.write(bytes(4096))
    code, out, err = run(args.reader, "--disk", p("blank.img"))
    check("exit 2", code == 2, f"exit={code} err={err!r}")
    check("named as absent", "no journal" in err, err)

    print("case: a file shorter than one slot")
    with open(p("short.log"), "wb") as f:
        f.write(b"x" * 100)
    code, out, err = run(args.reader, "--file", p("short.log"))
    check("exit 1", code == 1, f"exit={code}")
    check("reason named", "shorter" in err, err)

    print("case: --json names the same journal, machine-readably")
    code, out, err = run(args.reader, "--disk", p("fat12.img"), "--json")
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    check("nothing on stderr", err == "", err)
    try:
        document = json.loads(out)
    except ValueError as error:
        document = None
        check("stdout is one JSON document", False, f"{error}: {out[:120]!r}")
    if document is not None:
        check("stdout is one JSON document", True)
        check("placement is named", document["placement"] == "fat12-file",
              document["placement"])
        check("the committed prefix is named", document["slots"] == 4,
              document["slots"])
        check("the header capacity is named", document["capacity_slots"] == 64,
              document["capacity_slots"])
        check("the same records in the same order",
              [r["text"] for r in document["records"]] == records,
              document["records"])
        check("no warnings", document["warnings"] == [], document["warnings"])

    print("case: --json names the raw extent with its lba")
    code, out, err = run(args.reader, "--disk", p("extent.img"), "--json")
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    try:
        document = json.loads(out)
    except ValueError as error:
        document = None
        check("stdout is one JSON document", False, f"{error}: {out[:120]!r}")
    if document is not None:
        check("placement is named", document["placement"] == "extent",
              document["placement"])
        check("the extent's lba is named", document["first_lba"] == 8228,
              document["first_lba"])
        check("the same records in the same order",
              [r["text"] for r in document["records"]] == records,
              document["records"])

    print("case: --json on a medium without a journal keeps stdout empty")
    code, out, err = run(args.reader, "--disk", p("blank.img"), "--json")
    check("exit 2", code == 2, f"exit={code} err={err!r}")
    check("stdout stays empty", out == "", out)
    check("named as absent", "no journal" in err, err)

    print("case: --json names the torn slot and withholds its text")
    code, out, err = run(args.reader, p("torn.img"), "--json")
    check("exit 0 (the rest of the journal is intact)", code == 0, f"exit={code}")
    try:
        document = json.loads(out)
    except ValueError as error:
        document = None
        check("stdout is one JSON document", False, f"{error}: {out[:120]!r}")
    if document is not None:
        check("the torn slot is warned in the document",
              any("torn" in warning for warning in document["warnings"]),
              document["warnings"])
        check("its text is not in stdout", "state=MATCH" not in out, out)
        check("the later record is still in the document",
              records[2] in [r["text"] for r in document["records"]],
              document["records"])

    print("case: --json with --offset names the same FAT12 journal")
    code, out, err = run(args.reader, "--offset", str(64 * SECTOR),
                         p("fat12.img"), "--json")
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    try:
        document = json.loads(out)
    except ValueError as error:
        document = None
        check("stdout is one JSON document", False, f"{error}: {out[:120]!r}")
    if document is not None:
        check("placement is named", document["placement"] == "fat12-file",
              document["placement"])
        check("the same records in the same order",
              [r["text"] for r in document["records"]] == records,
              document["records"])
        check("no warnings", document["warnings"] == [], document["warnings"])

    print("case: --file --json names the mounted-file placement")
    code, out, err = run(args.reader, "--file", p("INSTALL.LOG"), "--json")
    check("exit 0", code == 0, f"exit={code} err={err!r}")
    try:
        document = json.loads(out)
    except ValueError as error:
        document = None
        check("stdout is one JSON document", False, f"{error}: {out[:120]!r}")
    if document is not None:
        check("placement is named", document["placement"] == "file",
              document["placement"])
        check("the committed prefix is named", document["slots"] == 3,
              document["slots"])
        check("the same records in the same order",
              [r["text"] for r in document["records"]] == records[:2],
              document["records"])
        check("no warnings", document["warnings"] == [], document["warnings"])

    if args.cross_check:
        evidence = args.cross_check
        print("case: the real C-prototype media from the QEMU proof")
        for name, marker in (("medium-fat12-journal.img", "fat12-file"),
                             ("medium-extent-journal.img", "extent")):
            path = os.path.join(evidence, name)
            if not os.path.exists(path):
                if args.require_image:
                    check(f"{name} present", False, path)
                else:
                    print(f"SKIP {name}: not present in {evidence}")
                continue
            code, out, err = run(args.reader, "--disk", path)
            check(f"{name}: exit 0", code == 0, f"exit={code} err={err!r}")
            check(f"{name}: {marker}", marker in out, out)
            check(f"{name}: refusal recorded", "refused reason=NO_DISK" in out, out)
        before = os.path.join(evidence, "medium-fat12-before.img")
        if os.path.exists(before):
            code, out, err = run(args.reader, "--disk", before)
            check("before-image: exit 2", code == 2, f"exit={code}")
        elif args.require_image:
            check("medium-fat12-before.img present", False, before)
        else:
            print(f"SKIP medium-fat12-before.img: not present in {evidence}")

    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    print(f"\n{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
