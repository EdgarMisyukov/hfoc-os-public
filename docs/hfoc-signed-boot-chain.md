# HFOC Signed Boot Chain and Boot Attestation

**Date:** 2026-09-12
**Target:** ITEM-372 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

ITEM-372 closes the gap ITEM-371 left open: the repository had no asymmetric
signature verification. `signed_metadata` is a keyed-policy SHA-256 envelope,
`struct trust_anchor` carries no key material, and the ITEM-371 media verifier
was a caller-bound cryptographic boundary with nothing bound to it. This item
adds the production verifier, applies it to the files the bootloader actually
loaded, measures them into a readable register set, and publishes one honest
verdict that the production boot coordinator consumes before it probes
anything and that the management evidence snapshot renders.

The item verifies the loaded chain and publishes measurements. It does not
claim a firmware root of trust: physical UEFI Secure Boot and
firmware-enforced signature checking are not implemented and stay
`NOT_TESTED`.

## Implemented Boundary

### Asymmetric verification boundary

`kernel/include/image_signature.h`, `kernel/src/image_signature.c` and
`kernel/src/image_signature_ecdsa.c` add the missing cryptographic boundary:

- Real ECDSA P-256 verification over a 32-byte SHA-256 digest with raw 64-byte
  `r || s` signatures and a 65-byte uncompressed public key.
- A bounded trust-anchor store (`IMAGE_SIGNATURE_ANCHOR_CAPACITY` 4) whose
  entries carry a signer id, the ECDSA P-256 algorithm, the accepted key
  purposes and the public key. The declared trust set is the release anchor
  (signer id 2) plus the development anchor (signer id 1); a release-only
  store trusts the release anchor only.
- The release anchor is the declared production signing identity; only its
  public half is in the repository and its private half is offline, held by
  the release ceremony owned by ITEM-380. The development anchor pairs with
  `boot/keys/hfoc-development-signing-key.pem`, a declared non-secret build
  identity, and is compiled in only when `HFOC_RELEASE_BUILD` is not defined.
- `image_signature_policy_init()` turns the store anchors into the
  `verification_policy` consumed by `signed_metadata_verify()`, so a manifest
  envelope and the asymmetric signature are checked against the same trust
  set.
- Fail closed: an unknown signer, a denied purpose, an unsupported algorithm
  or key, an unavailable verifier and a malformed or mismatching signature all
  refuse (`UNKNOWN_SIGNER`, `PURPOSE_DENIED`, `UNSUPPORTED_ALGORITHM`,
  `UNSUPPORTED_KEY`, `MALFORMED_SIGNATURE`, `MISMATCH`, `UNAVAILABLE`,
  `BAD_ANCHOR`, `DUPLICATE_ANCHOR`, `CAPACITY`). A caller never receives
  `IMAGE_SIGNATURE_OK` from a partial check.
- `image_signature_ecdsa_p256_verify()` has a weak default that returns
  `IMAGE_SIGNATURE_UNAVAILABLE`: a kernel that does not link the ECDSA backend
  fails closed instead of failing to link or silently accepting a signature.

### Signed boot-chain manifest

`kernel/include/boot_chain.h` and `kernel/src/boot_chain.c` define the fixed
little-endian signed boot-chain record:

- Magic `HFOCBOOT`, format version 1, a 32-byte header and a bounded set of up
  to 4 components: `BOOT_PAYLOAD` (1), `LOADER_CONFIG` (2) and `KERNEL` (3).
- Every component entry is kind LE32, reserved LE32, length LE64 and a
  SHA-256[32] digest, so the body is 224 bytes and the record is sealed with a
  64-byte ECDSA P-256 signature over `SHA-256(body)`.
- The manifest is sealed in the existing `HFOCSIGN` envelope
  (`signed_metadata` format version 2) with purpose `IMAGE` through
  `signed_metadata_seal()`, and verified through `signed_metadata_verify()`
  against the policy built from the anchor store.
- `boot_chain_chain_digest()` and `boot_chain_manifest_seal()` are the
  producer boundary; `boot_chain_verify()` fills a cleared
  `boot_chain_verify_report` and `boot_chain_report_is_trusted()` is true only
  when every stage boolean is set (envelope, signature, release, components
  and `trusted`). `boot_chain_measure()` fills the measured-boot register set
  from a trusted report.
- Verification is ordered and fails closed at every step: envelope policy,
  manifest structure, signature, release identity, component lengths and
  component digests. The exact refusals are `ENVELOPE_INVALID`,
  `POLICY_MISMATCH` (including an unknown signer), `MANIFEST_INVALID`,
  `BAD_VERSION`, `ALGORITHM_UNSUPPORTED`, `VERIFIER_UNAVAILABLE`,
  `SIGNATURE_INVALID`, `SIGNATURE_MISMATCH`, `RELEASE_MISMATCH`,
  `COMPONENT_MISSING`, `COMPONENT_LENGTH_MISMATCH`,
  `COMPONENT_DIGEST_MISMATCH`, `COMPONENT_UNDECLARED` (an extra component
  that the signature does not cover), `COMPONENT_LIMIT` and `CAPACITY`.

### Measured-boot registers

`kernel/include/boot_measurements.h` and `kernel/src/boot_measurements.c`
publish a bounded, readable measurement set:

- 6 SHA-256 registers with extend semantics:
  `register = SHA-256(register || digest)`. Register 0 binds the signed chain
  digest; a declared component of kind `k` extends register `k`.
- A bounded 8-entry component log records the component kind, its declared
  length and its digest, so a reader can see which component produced which
  value instead of one opaque aggregate.
- A canonical aggregate digest binds the register set and the component log.
- Publish and fail semantics: a set must be reset before a new chain is
  measured, measuring into a published set is refused,
  `boot_measurements_fail()` marks the whole set failed and unpublished, and
  reads and the aggregate refuse a failed or unpublished set. A failed
  measurement therefore cannot leave stale values readable as if they were
  current.

### Boot attestation

`kernel/include/boot_attestation.h` and `kernel/src/boot_attestation.c` publish
the single readable verdict:

- `ATTESTED` when a signed chain was provisioned and every verification and
  measurement step succeeded, `UNATTESTED` when a signed chain was
  provisioned and did not verify, and `UNAVAILABLE` when no signed chain is
  provisioned on the media.
- Exact reasons: `NONE`, `NO_MANIFEST`, `ENVELOPE_INVALID`,
  `POLICY_MISMATCH`, `MANIFEST_INVALID`, `ALGORITHM_UNSUPPORTED`,
  `VERIFIER_UNAVAILABLE`, `SIGNATURE_INVALID`, `SIGNATURE_MISMATCH`,
  `RELEASE_MISMATCH`, `COMPONENT_MISMATCH` and `MEASUREMENT_FAILED`.
- The attestation is always published, so a refusal is readable rather than
  absent, and `boot_attestation_blocks_production_boot()` is true for every
  `UNATTESTED` verdict, including a failed measurement. Media without a signed
  manifest publish `UNAVAILABLE` and never claim `ATTESTED`.

### Ordinary-boot composition

`kernel/include/boot_chain_boot.h` and `kernel/src/boot_chain_boot.c` own the
production trust set (the declared release anchor, plus the declared
development anchor outside a release build), build the verification policy at
generation 1, and verify the provisioned manifest against
`BOOT_CHAIN_EXPECTED_RELEASE_ID` (1). A media without a manifest publishes
`UNAVAILABLE` / `NO_MANIFEST`; missing or empty components publish
`UNAVAILABLE` / `MANIFEST_INVALID`; a trust set or policy that cannot be built
returns `BOOT_CHAIN_VERIFIER_UNAVAILABLE` with the published `UNAVAILABLE` /
`MANIFEST_INVALID` attestation. Otherwise the published verdict and reason
follow the verification status exactly.

### Production boot gate

`kernel/src/appliance_production_boot.{h,c}` gained the attestation gate:
`enum appliance_production_boot_attestation` (`NONE`, `TRUSTED`, `REFUSED`),
the new status `APPLIANCE_PRODUCTION_BOOT_UNATTESTED` (7), and the
`set_attestation` / `attestation` / `attestation_detail` accessors. A refused
attestation forces readiness `UNAVAILABLE` and mode `FAIL_CLOSED` and returns
`APPLIANCE_PRODUCTION_BOOT_UNATTESTED` before any backend probing, so a
provisioned chain that fails cannot reach `NORMAL` mode. `NONE` and `TRUSTED`
leave the existing coordinator behavior unchanged.

### Management-plane verdict

`kernel/include/appliance_production_management.h` and
`kernel/src/appliance_production_management.c` publish the same vocabulary in
the management evidence and snapshot: both carry a `boot_attestation` value,
`appliance_production_management_attestation_string()` renders `ATTESTED` /
`UNATTESTED` / `UNAVAILABLE` (`UNKNOWN` outside the vocabulary), and the
canonical management render adds `attestation=...`. The composition mirrors
the boot-attestation verdicts without linking the verification modules, and an
evidence source that does not carry an attestation publishes `UNAVAILABLE`
rather than claiming a pass.

### Kernel integration

`kernel/include/limine_min.h` was extended with the Limine executable-file and
module request ids and the matching `struct limine_file`,
executable-file request/response and module request/response structures.
`kernel/src/kernel.c` declares both requests in `.limine_requests`, collects
the boot chain from the loaded kernel file plus the manifest and
loader-configuration modules (`hfoc-boot.man` and `limine.conf` by path
suffix), runs `boot_chain_boot_run()` before the production boot coordinator,
prints

```
[BOOT-CHAIN] verdict=... reason=... release=... signer=... components=... chain=0x... measurements=0x...
```

and hands the attestation gate to the production boot coordinator before it
runs.

### Producer and ISO composition

`scripts/hfoc-boot-chain-manifest.py` is the producer: it builds the manifest
from the exact component files, signs `SHA-256(body)` with RFC 6979
(ECDSA
P-256, DER converted to the raw `r || s` form) and seals the `HFOCSIGN`
envelope. It refuses an empty component, a duplicate component kind, an
out-of-range release or signer id and a non-P-256-sized signature.

`make iso` signs `build/iso_root/boot/hfoc-kernel.elf` plus the loader
configuration with the development key (release id 1, signer id 1) and writes
`/boot/hfoc-boot.man` into the ISO; the ISO Limine configuration declares the
manifest and the loader configuration with `module_path`, so the kernel can
measure them. The current ISO composition therefore declares the kernel and
the loader configuration (2 components); the format and the producer also
carry the bounded `BOOT_PAYLOAD` kind for the pre-kernel payload.

### Verification-only Mbed TLS profile

The production kernel links only a bounded verification subset of the vendored
Mbed TLS library:

```
asn1parse asn1write bignum bignum_core bignum_mod bignum_mod_raw
constant_time ecdsa ecp ecp_curves ecp_curves_new md platform
platform_util sha256
```

The objects are compiled into `build/verify-kernel` with `MBEDTLS_HAVE_INT32`
and the verification profile (`kernel/include/hfoc_verify_config.h`,
`kernel/include/hfoc_verify_alloc.h`), and only `image_signature_ecdsa.o` is
compiled with that profile. `kernel/include/hfoc_verify_config.h` explicitly
leaves out TLS, X.509, PK, PSA, RNG, entropy, DRBG and cipher families; every
allocation goes through the fixed arena in `image_signature_ecdsa.c`, so a
verification has a hard memory bound and never touches the kernel heap or the
TLS allocator pools.

## Evidence

- Host proof `make test-image-signature-host` covers real ECDSA P-256
  verification vectors (the declared development identity and a proof-only
  fixture identity), the declared and explicit trust sets, anchor validation
  (unknown signer, duplicate signer, unsupported algorithm, missing purposes,
  out-of-range signer, non-uncompressed key, bounded-registry capacity),
  malformed and mismatching signatures, and the policy binding (generation,
  anchor set, purpose denial) consumed by `signed_metadata_verify()`.
- Host proof `make test-boot-measurements-host` covers reset and extend
  semantics, the bounded component log, the canonical aggregate, publish and
  failure states, and the refusal to read a failed or unpublished set.
- Host proof `make test-boot-chain-host` consumes the shared vector
  `tests/boot-chain-vector.{h,c}` and covers the producer format against the
  signed vector, real signature verification of the sealed manifest, the
  measured-boot fill, a release-only policy refusing the development signer,
  and the refusal matrix: tampered envelope, untrusted signer, foreign
  release, wrong component length, tampered component digest, missing
  component, component count beyond the signed manifest, null arguments and
  capacity.
- Host proof `make test-boot-attestation-host` covers the `ATTESTED` /
  `UNATTESTED` / `UNAVAILABLE` verdicts, the exact reasons, the blocking
  predicate and the publication contract. The management host proof
  `make test-appliance-production-management-host` covers the canonical
  management render including the `attestation=` field. All four boot-chain
  targets are part of `make test-host-suite-serial`.
- The `SIGNED_BOOT` stage of the production end-to-end acceptance matrix is
  `PROVEN` with target `test-production-boot-chain-qemu` and marker
  `QEMU production boot chain test: OK`; physical UEFI Secure Boot and
  firmware-enforced signature checking remain outside it.
- QEMU proof `make test-production-boot-chain-qemu` (driver
  `scripts/test-qemu-boot-chain.sh`, marker
  `QEMU production boot chain test: OK`) boots the signed production ISO and
  requires `[BOOT-CHAIN] verdict=ATTESTED reason=NONE release=1 signer=1
  components=2` on ordinary boot, then boots the macro-gated proof ISO
  (`HFOC_BOOT_CHAIN_QEMU_TEST`) that verifies the shared vector, tampers the
  kernel component and the signature and proves both refusals, and binds the
  refused attestation into the production boot coordinator, which reports
  `APPLIANCE_PRODUCTION_BOOT_UNATTESTED` with `mode=FAIL_CLOSED`. Each proof
  marker must appear exactly once, and the Makefile target additionally
  asserts that the production kernel and ISO carry the `[BOOT-CHAIN] verdict=`
  line but never the proof markers.

## Limitations

- Physical UEFI Secure Boot and firmware-enforced signature checking are not
  implemented. The item verifies the chain the loader loaded and publishes
  measurements; it does not claim a firmware root of trust, and that claim
  stays `NOT_TESTED`.
- The release signing ceremony with the offline release key is owned by
  ITEM-380. The ISO build signs with the declared development key, and a
  release build does not compile the development anchor in, so development
  media are refused by a release kernel.
- The current ISO composition signs the kernel and the loader configuration.
  The boot-payload component kind is defined by the format and supported by
  the producer, but no boot-payload file is signed by the ISO build yet.
- The attestation is consumed by the production boot coordinator, rendered in
  the management evidence snapshot as `attestation=...` and printed on the
  boot serial line. A remote attestation protocol, a measurement quote export
  and TPM-backed measurement are not part of this item.
- Every proof here is a host or QEMU proof. Physical media, physical boot
  chain and physical Secure Boot remain `NOT_TESTED`.
- `VERSION` is not changed by this item.

## Non-Goals

- UEFI Secure Boot, firmware-enforced verification, TPM-backed measurement and
  remote attestation.
- The release signing ceremony, release key custody, escrow and rotation
  (ITEM-380).
- Signing from the kernel: the kernel carries a verification-only profile and
  no private key material.
- Changes to TLS or X.509, a remote attestation protocol or a measurement
  quote export, and any `VERSION` bump.
