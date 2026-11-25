# HFOC Appliance Secret And Credential Protection

**Date:** 2026-09-05
**Target:** ITEM-322 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `ddcda61fae40d5f4fc717b5b0c176f48960af925`
**Status:** Completed by ITEM-322

## Purpose

This document is the authoritative ITEM-322 secret and credential protection
boundary. ITEM-319 fixed the management authentication architecture before
any administrative credential implementation, ITEM-320 implemented the
bounded administrative authentication and opaque bearer session boundary, and
ITEM-321 added management authorization and route policy. ITEM-322 now adds
the bounded caller-owned secret-protection lifecycle that Wave E requires:
one-way credential fingerprints, volatile-only in-memory retention for
runtime-usable secret material, explicit rotation/reset, at-rest export
policy, and no accidental secret exposure through status or diagnostic
strings.

The Wave D HTTPS management API remains unchanged: routes are read-only,
advertise `auth=unavailable`, and do not dispatch through this protection
boundary. ITEM-323 owns persistent namespaces and ITEM-325 publishes
certificate/secret persistence policy and HTTPS secret readiness integration.

## Current State

The repository now contains:

- the ITEM-319 authoritative management authentication architecture;
- the ITEM-320 `appliance_admin_auth` boundary;
- the ITEM-321 management authorization and route policy;
- `appliance_secret_protection` under `kernel/`;
- bounded secret kinds for administrative credentials, bearer tokens,
  private-key material, and other secrets;
- protect/rotate/reset/verify lifecycle operations and at-rest export;
- canonical SHA-256 fingerprints with kind/generation domain separation;
- volatile-only retention and zeroization for material that must remain
  usable in memory.

There is still no production normal-boot management activation, no HTTPS
route that requires authentication, and no production certificate/secret
persistence activation.

## Selected Mechanism

ITEM-322 protects Wave E secret material through one caller-owned protection
slot:

1. The administrative credential is protected as a canonical one-way SHA-256
   fingerprint and is never retained raw.
2. Bearer tokens, private-key material, and other secrets are retained in a
   bounded volatile caller-owned slot only when runtime use requires it.
3. Rotation validates the full replacement before clearing the old secret and
   advancing the protection generation.
4. Reset clears every secret-bearing byte and returns the slot to `EMPTY`.
5. At-rest export is permitted only for the administrative credential
   fingerprint record; all other secret kinds fail closed because session
   tokens are never persisted and certificate/private-key persistence is
   covered by the ITEM-325 certificate/secret persistence policy.

The canonical fingerprint is not a password KDF and is not key wrapping. It
is the non-reversible representation of the approved opaque 32-byte
administrative credential shape already fixed by ITEM-319/ITEM-320. ITEM-325
publishes the certificate/secret persistence policy boundary. Real wrapping
provider and production activation remain future crypto/loader boundaries.

## Secret Kinds

| Kind | Length | In-memory retention | At-rest export |
| --- | --- | --- | --- |
| Administrative credential | 32 bytes | no raw credential | SHA-256 fingerprint |
| Bearer token | 32 bytes | volatile-only, zeroized | denied |
| Private key | 1..1024 bytes | volatile-only, zeroized | denied; policy-only wrapped/digest records |
| Other secret | 1..1024 bytes | volatile-only, zeroized | denied |

All-zero material, unknown kinds, wrong lengths, malformed slots, version
mismatches, and exhaustion states are rejected before publication. A failed
protect, rotate, verify, or export leaves the prior protected slot unchanged.

## Lifecycle And Rotation

The protection slot starts in `EMPTY` with generation zero:

- `protect()` publishes from `EMPTY` and advances generation by one;
- `rotate()` requires the same kind, clears the old secret, advances
  generation by one, and publishes the replacement;
- `reset()` requires a protected slot, clears all secret-bearing bytes, and
  leaves generation unchanged so the next `protect()` starts a fresh
  generation;
- `verify()` recomputes the current canonical fingerprint and compares it in
  constant time without mutating the slot;
- generation exhaustion fails closed and preserves the existing protected
  secret.

Rotation does not allow changing secret kind in one operation. A
credential reset/rotation remains an explicit local administrative operation;
this module is not exposed as a remote route.

## At-Rest Record

`appliance_secret_protection_export_at_rest()` returns a caller-owned record
containing the protection version, kind, generation, SHA-256 fingerprint, and
reserved bytes. It contains no raw administrative credential and is the only
record permitted for persistence by this boundary.

Bearer-token, private-key, and other-secret records are denied with
`APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED`. The module performs no
storage I/O and does not write a secret namespace.

## Exposure Prevention

Status, kind, and state strings contain no secret bytes, no fingerprint bytes,
and no material bytes. The module has no log, audit, HTTP, serial, metrics,
support-bundle, or storage output path. Existing `diagnostic_privacy`,
support-bundle deny-lists, and production-artifact secret gates remain the
shared repository exposure controls; this boundary does not weaken them.

## Ownership And Failure Handling

- `appliance_secret_protection.c` owns only the caller-owned state passed to
  it.
- `verify()` and `export_at_rest()` are read-only.
- Retained volatile material is zeroized on reset, rotation, and all clear
  paths.
- The module performs no allocation, blocking, storage I/O, audit write,
  diagnostic output, or interrupt-context access.
- No status or error path reveals whether a secret was all-zero, too short,
  or mismatched beyond the bounded module status returned to the trusted
  caller.

## Wave E Ownership

- ITEM-319: management authentication architecture.
- ITEM-320: administrative authentication implementation.
- ITEM-321: management authorization and route policy.
- ITEM-322: secret and credential protection implementation.
- ITEM-323 onward: persistent namespaces, health/diagnostics, update, and
  release integration.
- ITEM-325 (completed): certificate/secret persistence policy and HTTPS
  certificate-dependency readiness.

The existing management API continues to advertise `auth=unavailable`. No
normal-boot, HTTPS, storage, audit, or hardware transition is introduced by
this item.

## Non-Goals

- No protected HTTPS route publication.
- No authentication, session, authorization, or management API change.
- No persistent namespace, storage I/O, encryption, key wrapping, or
  certificate/private-key persistence.
- No real production private-key import into at-rest storage.
- No normal-boot secret-protection activation.
- No QEMU proof, because this item changes no observable boot or HTTPS route
  behavior.
- No release version change.

## Acceptance

The focused host fixture in `tests/appliance-secret-protection-host.c` proves
exact lifecycle transitions, canonical fingerprint verification, at-rest
export policy, volatile retention and zeroization, rotation/reset,
generation exhaustion, status strings, and no allocation. The fixture is part
of `make test` through the canonical host suite.
