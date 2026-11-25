# HFOC Signed Media and Installer Hardening

**Date:** 2026-09-11
**Target:** ITEM-371 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

ITEM-371 makes the production installer refuse to touch a disk until it has
verified the installation media and the target hardware, and gives the product
a defined way back: uninstall with return to the previous operating system.
Before this item the installer controller validated only a discovery report, a
disk identity and an in-memory source; nothing checked the media digest or the
signature, there was no declared supported-hardware policy, and no uninstall or
return-to-previous-OS path existed anywhere in the tree.

The item keeps the existing installer stage machine and extends it with two
mandatory admission gates and one new contract:

```
prepare (evidence) -> verify media -> verify hardware -> build plan ->
publish preview -> confirm -> execute layout -> verify layout
                                                        |
                                        uninstall (dry run -> confirm)
                                    restore partition table + boot entries
```

## Signed media gate

`kernel/include/installer_media_verification.h` verifies the media in this
order, failing closed at every step:

- `installer_media_verification_run()` first verifies the media manifest
  envelope with the existing `signed_metadata_verify()` against the active
  `verification_policy`: purpose `KEY_USE_PURPOSE_IMAGE`, an active signer
  anchor, the reserved bytes, the envelope length and the envelope digest all
  have to hold.
- The manifest metadata is a fixed little-endian record: magic `HFOCMED1`,
  format version, signature algorithm, release identity, exact media length,
  the SHA-256 digest of the whole media, the exact length and SHA-256 digest of
  the installer source payloads (boot payload, Limine configuration and kernel
  image, concatenated in that order) and the signature bytes.
- The signature is checked through a caller-bound cryptographic boundary
  (`struct installer_media_verifier`): algorithm, signer id and the signature
  over the signed media digest. No verifier, an unavailable verifier or an
  unsupported algorithm denies the write.
- The module then recomputes SHA-256 over the media bytes, compares it with the
  signed media digest, recomputes the digest of the installer source payloads
  that are actually about to be written, compares it with the signed source
  digest, and checks both exact lengths and the expected release identity. A
  verified media digest therefore cannot authorize installing different bytes.

`installer_media_manifest_seal()` is the matching producer boundary: it builds
the manifest and seals it into an `HFOCSIGN` envelope, so the release tooling
and the proofs do not re-implement the wire format.

Only a report for which
`installer_media_verification_authorizes_write()` is true carries
`write_authorized`. A failed run always publishes a cleared report, so a stale
"authorized" result can never survive a failed verification.

### Cryptographic boundary and honesty

The repository has no asymmetric signature verification today:
`signed_metadata` is a keyed-policy SHA-256 envelope, `struct trust_anchor`
carries no key material, and the only compiled asymmetric verifier (Mbed TLS
P-256 ECDSA) is linked into QEMU TLS proof kernels, not into the installer
kernel. ITEM-371 therefore does not fabricate a signature check:

- the manifest, envelope, policy, digest and length checks are real;
- the asymmetric check is an explicit caller-bound verifier;
- with no verifier bound the installer fails closed with
  `INSTALLER_MEDIA_VERIFIER_UNAVAILABLE` / `PRODUCTION_INSTALLER_MEDIA_UNVERIFIED`
  and cannot plan, confirm or write.

Binding the production verifier and its key material is owned by ITEM-372
(signed boot chain and boot attestation). Until then the ordinary installer
composition refuses rather than degrading silently. The QEMU proof binds a
deterministic test verifier, and the proof declares it as such.

## Supported-hardware gate

`kernel/include/installer_supported_hardware.h` declares a bounded policy
(UEFI firmware as the declared default with legacy BIOS admitted — ITEM-517 —
at least one CPU core, at least 2 GiB RAM, virtio-blk / SATA /
NVMe controllers, 512-byte blocks, at least 12288 blocks of target disk, at
most `INSTALLER_DISCOVERY_DISK_CAPACITY` disks, optional ESP requirement) and
evaluates the ITEM-339 discovery report against it. Every refusal names an
exact reason (`FIRMWARE_UNSUPPORTED`, `CPU_ABSENT`, `MEMORY_INSUFFICIENT`,
`CONTROLLER_UNSUPPORTED`, `BLOCK_SIZE_UNSUPPORTED`, `CAPACITY_INSUFFICIENT`,
`TARGET_DISK_MISSING`, `TARGET_DISK_AMBIGUOUS`, `TOO_MANY_DISKS`,
`EFI_SYSTEM_PARTITION_MISSING`, `INCOMPLETE_SCAN`, `INVALID_REPORT`) and a
short auditable detail string. An unknown, ambiguous or incomplete report is
never treated as supported; `allow_bios_legacy` (default true) is the single
documented exception to `required_firmware`, and an `UNKNOWN` firmware mode is
never admitted by it.

## Plan and confirmation

The existing controller stage machine already forces
`EVIDENCE_VALIDATED -> PLAN_BUILT -> PREVIEW_PUBLISHED -> CONFIRMED` before
`installer_layout_execute()` may write. ITEM-371 makes the plan unreachable
until both admission gates pass:

- `production_installer_build_plan()` returns
  `PRODUCTION_INSTALLER_MEDIA_UNVERIFIED` until
  `production_installer_verify_media()` succeeded, and
  `PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE` until
  `production_installer_verify_hardware()` succeeded.
- `production_installer_publish_preview()` publishes the admission result in
  the operator-visible plan text (`media=verified hardware=supported`), and
  `production_installer_confirm()` still cannot be reached without the
  preview.

The gates are enforced twice: `build_plan` refuses without them, and
`production_installer_execute_layout` re-checks both flags at the write site
itself, so a mutated controller state, a hand-built layout plan or a caller
shortcut still cannot reach the first write with unverified media or undeclared
hardware.

## Uninstall and return to the previous OS

`kernel/include/installer_uninstall.h` adds the missing inverse operation:

- `installer_uninstall_plan_build()` cross-checks the captured evidence
  against the discovery report: the disk identity must match the report exactly,
  and the HFOC footprint (partitions and boot entry indices), the preserved
  previous-OS inventory, the pre-install `BootOrder` and the SHA-256 digest of
  the installer's partition-table snapshot must all be present. Missing,
  contradictory or foreign-disk evidence fails closed (`NO_HFOC_FOOTPRINT`,
  `NO_PREVIOUS_OS`, `BOOT_EVIDENCE_MISSING`, `SNAPSHOT_UNAVAILABLE`,
  `INVALID_ARGUMENT`).
- `installer_uninstall_preview()` publishes an operator-visible dry-run text
  (`HFOC UNINSTALL dry-run scenario=restore-previous-os disk=1
  remove-partitions=4 remove-boot-entries=1 preserve-boot-entries=2
  previous-os=restored`). Only after the preview may
  `installer_uninstall_confirm()` be called; execution without confirmation is
  refused with `NOT_CONFIRMED`.
- `installer_uninstall_execute()` first recomputes the snapshot digest and
  refuses a substituted snapshot (`SNAPSHOT_MISMATCH`) before any write, then
  restores the partition table byte for byte through the existing
  `installer_layout_rollback()`, removes every HFOC UEFI boot entry and restores
  the previous `BootOrder` through a caller-bound boot operation boundary, and
  finally verifies the observed outcome.
- `installer_uninstall_verify_outcome()` refuses a remaining HFOC partition or
  boot entry, an unrestored boot order, a lost previous-OS boot path or an
  unverified partition table (`OUTCOME_INCOMPLETE`, `PREVIOUS_OS_DAMAGED`).
  Nothing incomplete is ever rendered as a pass.

## Evidence

- Host proof `make test-installer-hardening-host` covers the signed manifest
  seal/verify round trip, every media refusal path (missing verifier, bad
  signature, tampered media digest, wrong media length, source digest and source
  length mismatch, incomplete source, foreign release, truncated/tampered
  envelope, foreign purpose, unknown signer, corrupt manifest, unsupported
  algorithm, stale report, cleared report on an early refusal), the full
  supported-hardware policy matrix, the uninstall identity/snapshot binding and
  plan/preview/confirm/execute/outcome contract, and the installer admission
  gates including the write-site re-check.
- QEMU proof `make test-production-installer-qemu` extends the real installer
  composition: the controller binds the media and hardware gates before the
  plan, a tampered media image and an undeclared machine both refuse with zero
  disk writes, the clean install proceeds, and the uninstall proof restores the
  previous partition table and boot state and removes the HFOC boot entry.
  New markers: `unverified media no-write proof OK`,
  `unsupported hardware no-write proof OK`,
  `uninstall dry-run preview proof OK`,
  `uninstall previous-OS restoration proof OK`.
- The production end-to-end acceptance matrix records `SIGNED_MEDIA` as
  `DEGRADED` with target `test-installer-hardening-host` and marker
  `Installer media and hardening host tests: ALL OK`: the focused proof passes,
  but the production asymmetric verifier is not bound yet, so the shipped
  installer refuses to install rather than pretending to be verified.

## Limitations

- The production asymmetric media verifier and its key material are owned by
  ITEM-372; this item proves the boundary and the fail-closed behavior without
  one.
- The media is verified as one contiguous buffer; streaming the digest directly
  from the installation medium is a boot-chain/installer-item concern. The
  installed source payloads are bound by their own signed digest and length.
- Uninstall restores the partition table, the boot entries and the boot order.
  Partition contents written by the install are not zeroed; the pre-install
  partition table no longer references the HFOC partitions, and physical
  sanitization is not claimed.
- The bounded pre-kernel UEFI helper (ITEM-361) still installs its payload
  without a digest check of its own; it consumes the same media and is covered
  by the ITEM-372 boot-chain work.
- No physical hardware run is claimed; every proof here is a host or QEMU
  proof.
