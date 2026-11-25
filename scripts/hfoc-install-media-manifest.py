#!/usr/bin/env python3
"""Seal the ITEM-394 installer media manifest (HFOCMED1).

ITEM-371 makes the installer refuse every write until the media it read is
policy-accepted and its payload signature verifies.  The QEMU fixture used to
be the only thing that ever produced such an envelope, with a deterministic
test verifier; the product media needs a real one.

This tool seals the payload set the installer reads and writes:

    installer payload blob = boot payload || loader configuration || kernel

`--payload-output` writes that exact concatenation, which the medium ships as
its own module so the kernel can hash the whole blob as the signed media, while
`--boot-payload`/`--limine-config`/`--kernel` are shipped separately as the
source payloads the ESP install consumes.  The sealed manifest binds both: the
media digest and length over the blob, and the source digest and length over
the concatenation of the three files, so a medium whose payload modules do not
reproduce the signed blob is refused before the first write.

The signature is the same deterministic (RFC 6979) ECDSA P-256 signature over
the 32-byte media SHA-256 digest that `scripts/hfoc-boot-chain-manifest.py`
already produces for the signed boot chain, and the envelope is the same
HFOCSIGN `signed_metadata` format version 2 the kernel parses.

Usage:
  hfoc-install-media-manifest.py --boot-payload FILE --limine-config FILE \
      --kernel FILE --key PRIVATE.pem [--release-id N] [--signer-id N] \
      --payload-output FILE --envelope-output FILE
"""

import argparse
import hashlib
import importlib.util
import pathlib
import struct
import sys

MANIFEST_MAGIC = b"HFOCMED1"
MANIFEST_FORMAT_VERSION = 1
ALGORITHM_ECDSA_P256 = 2
MANIFEST_HEADER_SIZE = 96
SIGNATURE_SIZE = 64
RELEASE_ID_MIN = 1


def load_boot_chain_module():
    """Reuse the boot-chain signer: same key parsing, same envelope."""

    path = pathlib.Path(__file__).resolve().parent / "hfoc-boot-chain-manifest.py"
    spec = importlib.util.spec_from_file_location(
        "hfoc_boot_chain_manifest", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"Cannot load the boot-chain signer: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_bytes(path):
    data = pathlib.Path(path).read_bytes()
    if not data:
        raise SystemExit(f"Refusing an empty input: {path}")
    return data


def build_manifest(release_id, media_digest, media_length, source_digest,
                   source_length, signature):
    if release_id < RELEASE_ID_MIN:
        raise SystemExit("release id must be >= 1")
    if len(media_digest) != 32 or len(source_digest) != 32:
        raise SystemExit("digests must be 32 bytes")
    if len(signature) != SIGNATURE_SIZE:
        raise SystemExit("signature must be the raw 64-byte r || s form")
    manifest = bytearray(MANIFEST_HEADER_SIZE + SIGNATURE_SIZE)
    manifest[0:len(MANIFEST_MAGIC)] = MANIFEST_MAGIC
    manifest[len(MANIFEST_MAGIC)] = MANIFEST_FORMAT_VERSION
    manifest[len(MANIFEST_MAGIC) + 1] = ALGORITHM_ECDSA_P256
    struct.pack_into("<I", manifest, 12, release_id)
    struct.pack_into("<Q", manifest, 16, media_length)
    manifest[24:56] = media_digest
    struct.pack_into("<Q", manifest, 56, source_length)
    manifest[64:96] = source_digest
    manifest[MANIFEST_HEADER_SIZE:] = signature
    return bytes(manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-payload", required=True)
    parser.add_argument("--limine-config", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--release-id", type=int, default=1)
    parser.add_argument("--signer-id", type=int, default=1)
    parser.add_argument("--payload-output", required=True)
    parser.add_argument("--envelope-output", required=True)
    arguments = parser.parse_args()

    boot_chain = load_boot_chain_module()

    boot_payload = read_bytes(arguments.boot_payload)
    limine_config = read_bytes(arguments.limine_config)
    kernel = read_bytes(arguments.kernel)
    blob = boot_payload + limine_config + kernel

    media_digest = hashlib.sha256(blob).digest()
    source_digest = hashlib.sha256(blob).digest()
    if len(blob) != len(boot_payload) + len(limine_config) + len(kernel):
        raise SystemExit("payload concatenation overflow")
    if len(blob) == 0:
        raise SystemExit("refusing an empty installer payload set")

    private_key = boot_chain.load_private_key(arguments.key)
    signature = boot_chain.ecdsa_sign(private_key, media_digest)
    if len(signature) != SIGNATURE_SIZE:
        raise SystemExit("unexpected signature length")

    manifest = build_manifest(
        arguments.release_id, media_digest, len(blob), source_digest,
        len(blob), signature)
    envelope = boot_chain.build_envelope(manifest, arguments.signer_id)

    pathlib.Path(arguments.payload_output).write_bytes(blob)
    pathlib.Path(arguments.envelope_output).write_bytes(envelope)

    print(
        "installer media manifest: "
        f"release_id={arguments.release_id} "
        f"signer_id={arguments.signer_id} "
        f"media_length={len(blob)} "
        f"media_sha256={media_digest.hex()} "
        f"envelope_length={len(envelope)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
