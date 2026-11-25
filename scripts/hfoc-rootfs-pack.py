#!/usr/bin/env python3
"""Build a deterministic HFOC initramfs archive (ITEM-623).

The format is documented in kernel/include/initramfs.h.  The packer walks the
source directory in byte-sorted relative-path order, so a parent directory is
always emitted before its children, refuses symlinks and special files, and
writes no timestamps, owner ids or other host-dependent bytes.  Two runs over
the same tree produce identical output.
"""

import argparse
import os
import stat
import struct
import sys

MAGIC = b"HFOCIR01"
VERSION = 1
HEADER = struct.Struct("<8sIIIIII")
ENTRY = struct.Struct("<HHIIIII")
MAX_ENTRIES = 64
MAX_PATH = 1024
MAX_FILE = 64 * 1024
# ITEM-628 raised the VFS arena to 96 blocks (384 KiB) for the native rootfs;
# the packer kept the pre-ITEM-628 256 KiB bound, which the 15 utilities plus
# the 64 KiB test file now exceed.  Keep the build bound equal to the kernel's
# VFS_DATA_MAX (64 * 96) instead of refusing an image the kernel can mount.
MAX_DATA = 384 * 1024
TYPE_FILE = 1
TYPE_DIR = 2


def fnv1a(*chunks):
    value = 2166136261
    for chunk in chunks:
        for byte in chunk:
            value ^= byte
            value = (value * 16777619) & 0xFFFFFFFF
    return value


def align4(value):
    return (value + 3) & ~3


def collect(source):
    entries = []
    data_bytes = 0
    for root, directories, files in os.walk(source, followlinks=False):
        directories.sort()
        files.sort()
        for name in directories + files:
            full = os.path.join(root, name)
            relative = os.path.relpath(full, source).replace(os.sep, "/")
            if relative.startswith("../") or relative == "..":
                raise SystemExit("refusing a path outside the source tree: %s" % relative)
            info = os.lstat(full)
            path = ("/" + relative).encode("utf-8")
            if len(path) >= MAX_PATH:
                raise SystemExit("path too long: %s" % relative)
            if stat.S_ISLNK(info.st_mode):
                raise SystemExit("symlink refused: %s" % relative)
            if stat.S_ISDIR(info.st_mode):
                entries.append({"type": TYPE_DIR, "mode": info.st_mode & 0o777,
                                "path": path, "data": b""})
            elif stat.S_ISREG(info.st_mode):
                if info.st_size > MAX_FILE:
                    raise SystemExit("file too large: %s" % relative)
                with open(full, "rb") as handle:
                    payload = handle.read()
                if len(payload) != info.st_size:
                    raise SystemExit("file changed while reading: %s" % relative)
                data_bytes += len(payload)
                entries.append({"type": TYPE_FILE, "mode": info.st_mode & 0o777,
                                "path": path, "data": payload})
            else:
                raise SystemExit("special file refused: %s" % relative)
    entries.sort(key=lambda item: (item["path"], item["type"]))
    if len(entries) > MAX_ENTRIES:
        raise SystemExit("too many entries: %d" % len(entries))
    if data_bytes > MAX_DATA:
        raise SystemExit("file data exceeds the VFS budget")
    for item in entries:
        if item["type"] == TYPE_DIR and item["data"]:
            raise SystemExit("directory has data")
    return entries


def build(entries):
    body = bytearray()
    cursor = HEADER.size
    for item in entries:
        path = item["path"]
        data = item["data"]
        path_pad = align4(len(path)) - len(path)
        data_offset = cursor + ENTRY.size + len(path) + path_pad
        checksum = fnv1a(path, data)
        body += ENTRY.pack(item["type"], item["mode"], len(path), len(data),
                           data_offset, checksum, 0)
        body += path + b"\x00" * path_pad
        body += data
        body += b"\x00" * (align4(len(data)) - len(data))
        cursor = data_offset + align4(len(data))
    total = HEADER.size + len(body)
    header = HEADER.pack(MAGIC, VERSION, len(entries), total,
                         fnv1a(bytes(body)), 0, 0)
    return header + bytes(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="root directory to pack")
    parser.add_argument("output", help="archive path to write")
    args = parser.parse_args()
    if not os.path.isdir(args.source):
        raise SystemExit("source is not a directory: %s" % args.source)
    archive = build(collect(args.source))
    temporary = args.output + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(archive)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, args.output)
    print("rootfs archive: %s entries=%d bytes=%d" %
          (args.output, struct.unpack_from("<I", archive, 12)[0], len(archive)))


if __name__ == "__main__":
    sys.exit(main())
