# HFOC Appliance Update Manifest Validation

**Date:** 2026-09-06
**Target:** ITEM-330 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Canonical architecture:**
`docs/hfoc-appliance-update-package-architecture.md` (ITEM-329)
**Status:** Completed by ITEM-330

## Purpose

This document is the authoritative ITEM-330 update manifest validation
boundary. ITEM-329 published the update package architecture and fixed the
second installation stage: bounded identity/compatibility metadata parse and
validation before any candidate inventory lookup, preflight, staged write,
activation, or persistent mutation. ITEM-330 implements that parser and
compatibility gate as an allocation-free, deny-by-default policy module.

The module validates the exact serialized metadata envelope, active release
profile identity, source revision shape, digest syntax, compatibility ranges,
and source/target compatibility. It does not compute or verify digests or
signatures; ITEM-331 owns cryptographic integrity and signature verification.

## Current State

The repository now contains:

- `release_profile` with `HFOC-APPLIANCE-V1`, `x86_64`, and fixed platform
  versions;
- `update_compatibility` with the bounded 19-field source/target
  compatibility manifest and no-write compatibility check;
- the ITEM-329 authoritative update package architecture;
- `appliance_update_manifest` under `kernel/` for the ITEM-330 boundary.

There is still no production update transport, package admission route,
storage writer, boot-slot switcher, cryptographic verifier, recovery mode,
factory reset, or normal-boot update activation.

## Manifest Format

The update package manifest is a bounded ASCII `key=value` line format with
exactly 25 unique fields. Fields may appear in any order, but each must
appear exactly once.

Identity and digest fields:

```text
update_package_version=1
release_id=HFOC-APPLIANCE-V1
architecture=x86_64
source_revision=<40 lowercase hexadecimal characters>
metadata_digest=<64 lowercase hexadecimal characters>
payload_digest=<64 lowercase hexadecimal characters>
```

Compatibility fields are the existing `update_compatibility` fields:
`compatibility_version`, source/target image IDs, and inclusive
source/target schema, kernel ABI, service ABI, and policy-generation ranges.

Every line contains exactly one `=`. Keys are lowercase `a-z0-9_` and bounded.
Values are bounded ASCII with no whitespace. The overall input and each line
have explicit maximum lengths.

## Validation

`appliance_update_manifest_parse()` validates in deterministic no-write order:

1. Null input or output is rejected.
2. Input over maximum length is rejected.
3. Blank lines, missing `=`, extra `=`, invalid keys, invalid values, and
   over-limit lines are rejected as malformed.
4. Duplicate fields are rejected.
5. Unknown fields are rejected.
6. Missing fields are rejected.
7. `update_package_version` must be exactly `1`.
8. `release_id` must be exactly `HFOC-APPLIANCE-V1`.
9. `architecture` must be exactly `x86_64`.
10. `source_revision` must be exactly 40 lowercase hexadecimal characters.
11. `metadata_digest` and `payload_digest` must each be exactly 64 lowercase
    hexadecimal characters.
12. Compatibility fields are delegated to
    `update_compatibility_manifest_parse()` and
    `update_compatibility_manifest_validate()` for range, same-image,
    version, and semantic validation.

Only after every check succeeds is the complete manifest copied to the caller
output. Every failure leaves caller-visible output byte-for-byte unchanged.

## Compatibility Gate

`appliance_update_manifest_check()` consumes the parsed manifest and
caller-provided active source and planned target state. It delegates to
`update_compatibility_check()` and returns `APPLIANCE_UPDATE_MANIFEST_OK`
only for a compatible result. Source/target image, schema, ABI, service ABI,
and policy-generation mismatches are denied with the exact underlying
`update_compatibility_status` when requested.

Invalid source and target state are rejected with explicit statuses.
Compatibility failures occur before candidate-slot inventory lookup,
preflight, staged write, activation intent, or known-good mutation.

## Ownership And Failure Handling

- The module performs no allocation, blocking, storage I/O, audit write,
  diagnostic output, locking, or interrupt-context access.
- The module owns no candidate slot, inventory, bundle bytes, trust state,
  storage, or normal-boot object.
- Digest fields are syntax-only in ITEM-330; exact SHA-256 integrity
  computation and signature verification remain ITEM-331.
- Raw credentials, private keys, tokens, secrets, shell commands, and file
  paths never appear in update metadata or diagnostic output.
- No release, tag, VERSION, management route, or normal-boot activation is
  introduced.

## Wave H Ownership

- ITEM-329: update package architecture.
- ITEM-330: manifest validation and compatibility checks (this boundary).
- ITEM-331: cryptographic integrity and signature verification.
- ITEM-332: recovery mode.
- ITEM-333: factory reset.

## Files

- `kernel/include/appliance_update_manifest.h`
- `kernel/src/appliance_update_manifest.c`
- `tests/appliance-update-manifest-host.c`
- `docs/superpowers/specs/2026-09-06-item-330-update-manifest-validation-design.md`
- `docs/superpowers/plans/2026-09-06-item-330-update-manifest-validation-implementation.md`
- This authoritative document
