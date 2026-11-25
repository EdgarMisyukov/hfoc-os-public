#!/usr/bin/env python3
"""Wrap a flat native code blob in the static ELF64 shape ITEM-625 maps.

The test fixture linker can emit headers in a separate segment; this wrapper
publishes the same bytes as one page-aligned RX PT_LOAD at the requested
entry, which is exactly the shape the native loader accepts.  It is a test
and packaging helper, not a general linker.
"""
import struct
import sys

HEADER = 64
PHDR = 56
CODE_OFFSET = 0x1000


def main(argv):
    if len(argv) != 4:
        print("usage: hfoc-user-elf-wrap.py BLOB OUT ENTRY_HEX", file=sys.stderr)
        return 2
    blob = open(argv[1], "rb").read()
    if not blob or len(blob) > 4096:
        print("blob must be 1..4096 bytes", file=sys.stderr)
        return 2
    entry = int(argv[3], 16)
    if entry & 0xFFF:
        print("entry must be page aligned", file=sys.stderr)
        return 2
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    header = ident + struct.pack(
        "<HHIQQQIHHHHHH",
        2,      # ET_EXEC
        62,     # EM_X86_64
        1,      # EV_CURRENT
        entry,
        HEADER, # e_phoff
        0,      # e_shoff
        0,      # e_flags
        HEADER,
        PHDR,
        1,      # e_phnum
        64,     # e_shentsize
        0,      # e_shnum
        0,      # e_shstrndx
    )
    program = struct.pack(
        "<IIQQQQQQ",
        1,          # PT_LOAD
        5,          # PF_R | PF_X
        CODE_OFFSET,
        entry,
        entry,
        len(blob),
        len(blob),
        0x1000,
    )
    image = bytearray(CODE_OFFSET + len(blob))
    image[0:HEADER] = header
    image[HEADER:HEADER + PHDR] = program
    image[CODE_OFFSET:] = blob
    with open(argv[2], "wb") as output:
        output.write(image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
