#!/usr/bin/env python3
"""Create or verify an ITEM-598 personalised install-media seed record.

The 32-byte payload comes only from the provisioning host's ready kernel
CSPRNG through os.getrandom().  The tool never prints, hashes only to stdout as
the whole seed, or accepts the seed on the command line.  The printed digest is
over the public record header, not over the complete record.
"""

import argparse
import hashlib
import os
import pathlib
import struct
import sys

MAGIC = b"HFOCSED1"
VERSION = 1
DOMAIN_INSTALL = 1
TARGET_LENGTH = 32
SEED_LENGTH = 32
RECORD_LENGTH = 120
DIGEST_OFFSET = 88
PUBLIC_HEADER_LENGTH = 56
MIN_DISTINCT_BYTES = 16
REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parent.parent


def target_bytes(target):
    try:
        encoded = target.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("target serial must be ASCII") from error
    if not 1 <= len(encoded) <= TARGET_LENGTH:
        raise ValueError("target serial must contain 1..32 bytes")
    if any(
        not (
            ord("A") <= value <= ord("Z")
            or ord("a") <= value <= ord("z")
            or ord("0") <= value <= ord("9")
            or value in b"-_.:"
        )
        for value in encoded
    ):
        raise ValueError("target serial contains an unsupported character")
    return encoded


def seed_is_diverse(seed):
    return (
        len(seed) == SEED_LENGTH
        and len(set(seed)) >= MIN_DISTINCT_BYTES
        and seed[: SEED_LENGTH // 2] != seed[SEED_LENGTH // 2 :]
    )


def build_record(seed, target, build_id):
    if not seed_is_diverse(seed):
        raise ValueError("host CSPRNG returned repetitive material")
    if build_id < 1:
        raise ValueError("build id must be >= 1")
    target_field = target_bytes(target)
    record = bytearray(RECORD_LENGTH)
    record[0:8] = MAGIC
    struct.pack_into("<I", record, 8, VERSION)
    struct.pack_into("<I", record, 12, DOMAIN_INSTALL)
    struct.pack_into("<I", record, 16, build_id)
    record[24 : 24 + len(target_field)] = target_field
    record[56:88] = seed
    record[88:120] = hashlib.sha256(record[:88]).digest()
    return bytes(record)


def verify_record(record, target, build_id):
    if len(record) != RECORD_LENGTH:
        raise ValueError("seed record has the wrong length")
    if record[:8] != MAGIC:
        raise ValueError("seed record magic is invalid")
    version, domain, found_build = struct.unpack_from("<III", record, 8)
    if version != VERSION:
        raise ValueError("seed record version is unsupported")
    if domain != DOMAIN_INSTALL:
        raise ValueError("seed record domain is not INSTALL")
    if found_build != build_id:
        raise ValueError(
            f"seed record build id {found_build} does not match {build_id}")
    target_field = target_bytes(target)
    if record[24 : 24 + len(target_field)] != target_field:
        raise ValueError("seed record target serial does not match")
    if any(record[24 + len(target_field) : 56]):
        raise ValueError("seed record target padding is nonzero")
    if not seed_is_diverse(record[56:88]):
        raise ValueError("seed record material is repetitive")
    if record[88:120] != hashlib.sha256(record[:88]).digest():
        raise ValueError("seed record integrity check failed")


def refuse_unsafe_output(path):
    resolved = path.resolve()
    try:
        resolved.relative_to(REPOSITORY_ROOT)
    except ValueError:
        return
    raise ValueError(
        "refusing to write a personalised seed inside the repository")


def write_new_record(path, record):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        offset = 0
        while offset < len(record):
            written = os.write(descriptor, record[offset:])
            if written <= 0:
                raise OSError("short write")
            offset += written
        os.fsync(descriptor)
    except BaseException:
        os.close(descriptor)
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        raise
    else:
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--output")
    operation.add_argument("--verify")
    parser.add_argument("--target", required=True)
    parser.add_argument("--build-id", required=True, type=int)
    arguments = parser.parse_args()

    try:
        if arguments.verify:
            path = pathlib.Path(arguments.verify)
            record = path.read_bytes()
            verify_record(record, arguments.target, arguments.build_id)
        else:
            path = pathlib.Path(arguments.output)
            refuse_unsafe_output(path)
            path.parent.mkdir(parents=True, exist_ok=True)
            seed = os.getrandom(SEED_LENGTH)
            record = build_record(seed, arguments.target, arguments.build_id)
            write_new_record(path, record)
            del seed
    except (OSError, ValueError) as error:
        print(f"HFOC host seed: REFUSED: {error}", file=sys.stderr)
        return 2

    public_header_digest = hashlib.sha256(
        record[:PUBLIC_HEADER_LENGTH]).hexdigest()
    print(
        "HFOC host seed: OK "
        f"target={arguments.target} build={arguments.build_id} "
        f"record_bytes={len(record)} header_sha256={public_header_digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
