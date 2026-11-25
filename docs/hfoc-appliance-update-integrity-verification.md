# HFOC Appliance Update Integrity Verification

**Date:** 2026-09-06
**Target:** ITEM-331 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Canonical architecture:**
`docs/hfoc-appliance-update-package-architecture.md` (ITEM-329)
**Manifest boundary:**
`docs/hfoc-appliance-update-manifest-validation.md` (ITEM-330)
**Status:** Completed by ITEM-331

## Purpose

This document is the authoritative ITEM-331 update integrity and signature
verification boundary. ITEM-329 fixed integrity and trust as the third
installation stage. ITEM-330 published the parsed update manifest and
validated its digest fields syntactically. ITEM-331 implements the actual
SHA-256 integrity checks and the update-purpose signed-metadata authenticity
check that fail closed before any candidate inventory lookup, preflight,
staged write, activation, or persistent mutation.

## Current State

The repository now contains:

- `appliance_update_manifest` with the parsed 25-field update manifest;
- `sha256` with deterministic SHA-256 computation;
- `verification_policy` and `trust_anchor` with active signer state;
- `signed_metadata` with update-purpose envelopes;
- `appliance_update_integrity` under `kernel/` for this ITEM-331 boundary.

There is still no production update transport, package admission route,
storage writer, boot-slot switcher, recovery mode, factory reset, or
normal-boot update activation.

## Verification Model

The verifier consumes a parsed ITEM-330 manifest, a bounded update metadata
region, a bounded update payload region, an active verification policy, and
an update-purpose signed-metadata envelope.

The metadata and payload SHA-256 digests are computed from the exact
caller-provided bytes and compared to the lowercase hexadecimal
`metadata_digest` and `payload_digest` fields in the manifest. Authenticity
comes from an envelope sealed with `signed_metadata_seal()` for
`KEY_USE_PURPOSE_UPDATE`. The signed metadata payload must be exactly 64
bytes: the raw 32-byte metadata digest followed by the raw 32-byte payload
digest.

This binding prevents a valid signed envelope for one package from being
replayed against a different metadata region or payload region. The active
verification policy is the single source of accepted signer identity.

## API

`kernel/include/appliance_update_integrity.h` publishes:

- `APPLIANCE_UPDATE_INTEGRITY_VERSION`;
- metadata and payload maximum lengths;
- the 64-byte signed-metadata binding size;
- exact statuses and status strings;
- `struct appliance_update_integrity_gate`;
- `appliance_update_integrity_init()`;
- `appliance_update_integrity_verify()`;
- `appliance_update_integrity_is_verified()`.

`appliance_update_integrity_init()` publishes an empty one-shot gate.
`appliance_update_integrity_verify()` computes and validates both digests,
verifies the update-purpose signed envelope, checks the exact digest binding,
and only then copies verified policy generation, signer ID, lengths, and raw
digests into the gate.

## Validation Order

The verifier checks in deterministic no-write order:

1. Null gate, manifest, policy, metadata, payload, or envelope is rejected.
2. An uninitialized or already verified gate is rejected.
3. The manifest shape, release profile, revision, digest fields, and embedded
   compatibility manifest are validated.
4. The active verification policy must exist.
5. Metadata length must be nonzero and bounded.
6. Payload length must be nonzero and bounded.
7. The metadata SHA-256 digest must equal `metadata_digest`.
8. The payload SHA-256 digest must equal `payload_digest`.
9. `signed_metadata_verify()` must accept the envelope under
   `KEY_USE_PURPOSE_UPDATE`.
10. The signed metadata payload must equal the two raw digests in order.

Every failure leaves the gate byte-for-byte unchanged. A successful verify is
one-shot.

## Failure Handling

Update-purpose signed-envelope failures map to exact statuses:

- `SIGNED_METADATA_NO_POLICY` to no policy;
- `SIGNED_METADATA_UNKNOWN_SIGNER` to unknown signer;
- `SIGNED_METADATA_BAD_SIGNATURE` to bad signature;
- `SIGNED_METADATA_PURPOSE_DENIED` to purpose denied;
- every other signed-metadata failure to a malformed update envelope.

Integrity and authenticity failures occur before candidate-slot inventory
publication, preflight, staged write, activation intent, or known-good
mutation. They are ordinary recoverable errors, not kernel panics.

## Ownership

- The module performs no allocation, blocking, storage I/O, audit write,
  diagnostic output, locking, or interrupt-context access.
- The module owns no candidate slot, inventory, bundle bytes, trust state,
  signed envelope, metadata, payload, storage, or normal-boot object.
- Digest computation is streaming and makes no internal copy of metadata or
  payload bytes.
- Raw credentials, private keys, tokens, secrets, shell commands, and file
  paths never appear in update metadata, diagnostics, or this module.
- No release, tag, VERSION, management route, or normal-boot activation is
  introduced.

## Wave H Ownership

- ITEM-329: update package architecture.
- ITEM-330: manifest validation and compatibility checks.
- ITEM-331: cryptographic integrity and signature verification (this
  boundary).
- ITEM-332: recovery mode.
- ITEM-333: factory reset.

## Files

- `kernel/include/appliance_update_integrity.h`
- `kernel/src/appliance_update_integrity.c`
- `tests/appliance-update-integrity-host.c`
- `docs/superpowers/specs/2026-09-06-item-331-update-integrity-verification-design.md`
- `docs/superpowers/plans/2026-09-06-item-331-update-integrity-verification-implementation.md`
- This authoritative document
