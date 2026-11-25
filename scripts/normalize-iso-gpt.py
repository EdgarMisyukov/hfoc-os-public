#!/usr/bin/env python3
# ITEM-380 deterministic ISO GPT normalization.
#
# xorriso generates a random GPT disk GUID and random per-partition unique
# GUIDs for the EFI boot partition it embeds, so two builds of the same
# revision differ even when every file date is pinned.  Everything else in the
# image is already deterministic (verified by byte comparison), so this script
# rewrites exactly those identifier fields and recomputes the two GPT CRC32
# checksums.  Boot code, partition layout and file contents are untouched.
#
# The GUIDs are derived from the normalization seed (the source revision), so
# the same revision always produces the same image.
import hashlib
import struct
import sys
import zlib

GPT_HEADER_LBA = 1
GPT_SIGNATURE = b"EFI PART"


def guid_from(seed: str, kind: str, index: int) -> bytes:
    material = f"HFOC-ISO-GPT:{kind}:{index}:{seed}".encode()
    return hashlib.sha256(material).digest()[:16]


def le32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def le64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def write_le32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def normalize_header(image: bytearray, header_lba: int, seed: str) -> bool:
    offset = header_lba * 512
    if image[offset:offset + 8] != GPT_SIGNATURE:
        # limine bios-install overwrites the primary GPT header at LBA 1 with
        # its stage 2 code, so a missing primary header is expected.
        return False
    header_size = le32(image, offset + 12)
    entries_lba = le64(image, offset + 72)
    entry_count = le32(image, offset + 80)
    entry_size = le32(image, offset + 84)
    if header_size < 92 or header_size > 512 or entry_size < 128:
        raise SystemExit("normalize-iso-gpt: unsupported GPT header geometry")

    entries_offset = entries_lba * 512
    for index in range(entry_count):
        start = entries_offset + index * entry_size
        type_guid = bytes(image[start:start + 16])
        if type_guid == b"\x00" * 16:
            continue
        image[start + 16:start + 32] = guid_from(seed, "part", index)
    entries = bytes(image[entries_offset:entries_offset + entry_count * entry_size])
    write_le32(image, offset + 88, zlib.crc32(entries) & 0xFFFFFFFF)

    image[offset + 56:offset + 72] = guid_from(seed, "disk", 0)
    write_le32(image, offset + 16, 0)
    header = bytes(image[offset:offset + header_size])
    write_le32(image, offset + 16, zlib.crc32(header) & 0xFFFFFFFF)
    return True


def normalize_mbr(image: bytearray, seed: str) -> None:
    # The MBR disk signature at offset 440 is written with a random value by
    # the isohybrid/protective-MBR step; pin it to the same seed so two builds
    # of one revision agree byte for byte.
    image[440:444] = hashlib.sha256(
        f"HFOC-ISO-MBR-SIG:{seed}".encode()).digest()[:4]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: normalize-iso-gpt.py IMAGE SEED", file=sys.stderr)
        return 2
    path = sys.argv[1]
    seed = sys.argv[2]
    with open(path, "rb") as handle:
        image = bytearray(handle.read())
    if len(image) < 34 * 512:
        print("normalize-iso-gpt: image is too small", file=sys.stderr)
        return 1
    normalize_mbr(image, seed)
    normalize_header(image, GPT_HEADER_LBA, seed)
    backup_lba = le64(image, GPT_HEADER_LBA * 512 + 32)
    if backup_lba and backup_lba * 512 + 512 <= len(image):
        normalize_header(image, backup_lba, seed)
    with open(path, "wb") as handle:
        handle.write(image)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
